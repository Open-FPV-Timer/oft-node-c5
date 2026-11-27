/* OFT Node mode. GPL-3.0-or-later. See oft.h. */
#include "oft.h"
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "link.h"
#include "oft_frame.h"
#include "profile.h"
#include "radio.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

#define OFT_FW_MAJOR 0
#define OFT_FW_MINOR 1
#define OFT_FW_PATCH 0

/* Samples per frame. Smaller frames mean less latency and a better uniform-dt fit when the
 * hop rate jitters; 32 at ~1 kHz is a frame every ~32 ms. */
#define BATCH_N 32
/* Flush a partial batch that has not grown for this long (low hop rates). */
#define BATCH_STALE_US 100000
#define STATUS_PERIOD_US 1000000
#define SYNC_RING 16

typedef struct {
    int64_t t_first, t_last;
    uint16_t n;
    int16_t power[BATCH_N];
    int32_t floor_q8;
    uint8_t flags;
} batch_t;

_Static_assert(BATCH_N <= OFT_MAX_SAMPLES, "batch must fit the codec's sample cap");

/* Buffers live on the heap: the C5's static sections must stay below the RF capture SRAM
 * bank (radio/c5/sram_guard.ld), and upstream already sits close to it. */
typedef struct { bool mapped; uint16_t freq_mhz; } seat_t;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static batch_t *batch;                 /* [PROFILE_MAX_CHANNELS] */
static seat_t *seat;                   /* [PROFILE_MAX_CHANNELS] */
static oft_decoder_t *dec;             /* one decoder: sniffing and OFT mode read the same stream */
static oft_msg_t *pending;
static bool have_pending;
static int64_t next_status_us;
/* The Timer has sent a plan since OFT mode was entered: it has seen our Hello. */
static bool got_plan;
static uint64_t chip_id;
static uint32_t hop_errors;

/* SYNC edges: timestamps from the ISR, drained by oft_poll. */
static volatile int64_t *sync_ts;      /* [SYNC_RING] */
static volatile uint32_t sync_wr, sync_rd, sync_seq;
static int8_t sync_gpio = -1;          /* the SYNC pin, or -1 (oft_sync_pin) */
static bool sync_on;                   /* its edge interrupt is running */

/* Level code x256 -> 0.1 dB relative: code 255 is 0 dBFS, 0.25 dB per code, so one code is
 * 2.5 units of 0.1 dB: (q8 / 256 - 255) * 2.5. Codes are unclamped here, so a signal above
 * the 8-bit range reads above 0. */
static int16_t q8_to_ddb(int32_t q8) {
    int32_t v = (int32_t)(((int64_t)(q8 - 255 * 256) * 25) / 2560);
    if (v > INT16_MAX) v = INT16_MAX;
    if (v < INT16_MIN) v = INT16_MIN;
    return (int16_t)v;
}

static void send(const oft_msg_t *m) {
    uint8_t buf[OFT_MAX_FRAME];
    size_t n = oft_encode_frame(m, buf, sizeof(buf));
    if (n) link_raw(buf, n);
}

static void send_hello(void) {
    scan_config_t c;
    scanner_get_config(&c);
    const profile_t *p = c.profile;
    oft_msg_t m = {.type = OFT_MSG_HELLO};
    m.u.hello = (oft_hello_t){
        .chip_id = chip_id,
        .fw = {OFT_FW_MAJOR, OFT_FW_MINOR, OFT_FW_PATCH},
        .link_version = OFT_LINK_VERSION,
        .freq_min_mhz = (uint16_t)(profile_equiv_khz(p, p->tune_lo_khz) / 1000.0 + 0.5),
        .freq_max_mhz = (uint16_t)(profile_equiv_khz(p, p->tune_hi_khz) / 1000.0 + 0.5),
        .max_seats = PROFILE_MAX_CHANNELS,
        .features = (sync_gpio >= 0 ? OFT_FEAT_SYNC : 0) | OFT_FEAT_DFU,
        .chip = CONFIG_IDF_TARGET, /* "esp32c5", "esp32s3" */
    };
    send(&m);
}

/* Chip temperature in 0.1 degC, or OFT_TEMP_UNKNOWN. The sensor is set up on first use, so it
 * costs nothing outside OFT mode. Runs in the command task, never in the capture path. */
#define OFT_TEMP_UNKNOWN INT16_MIN
static int16_t read_temp_dc(void) {
#if SOC_TEMP_SENSOR_SUPPORTED
    static temperature_sensor_handle_t tsens;
    static bool tried;
    if (!tried) {
        tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
        if (temperature_sensor_install(&cfg, &tsens) != ESP_OK) tsens = NULL;
        else if (temperature_sensor_enable(tsens) != ESP_OK) {
            temperature_sensor_uninstall(tsens);
            tsens = NULL;
        }
    }
    float c;
    if (tsens && temperature_sensor_get_celsius(tsens, &c) == ESP_OK) return (int16_t)(c * 10.0f);
#endif
    return OFT_TEMP_UNKNOWN;
}

static void send_status(void) {
    unsigned gain;
    radio_gain_mode_t gm = radio_get_gain(&gain);
    oft_msg_t m = {.type = OFT_MSG_STATUS};
    m.u.status = (oft_status_t){
        .temp_dc = read_temp_dc(),
        .hop_errors = (uint16_t)hop_errors,
        .overruns = (uint16_t)link_drops(),
        .gain_index = gm == RADIO_GAIN_HARDWARE ? OFT_GAIN_AUTO : (uint8_t)gain,
    };
    send(&m);
}

/* Not IRAM: a flash-cache miss costs a microsecond or two of edge timestamp jitter, well
 * inside the clock budget, and the C5's static RAM below the RF bank is nearly full. */
static void sync_isr(void *arg) {
    (void)arg;
    uint32_t w = sync_wr;
    if (w - sync_rd < SYNC_RING) {
        sync_ts[w % SYNC_RING] = esp_timer_get_time();
        sync_wr = w + 1;
    }
}

esp_err_t oft_sync_pin(int gpio) {
    if (sync_on) {
        gpio_isr_handler_remove((gpio_num_t)sync_gpio);
        sync_on = false;
    }
    sync_gpio = (int8_t)gpio;
    if (gpio < 0 || !sync_ts) return ESP_OK; /* none, or oft_init starts it */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) return e;
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = gpio_isr_handler_add((gpio_num_t)gpio, sync_isr, NULL);
    if (e != ESP_OK) return e;
    sync_on = true;
    return ESP_OK;
}

esp_err_t oft_init(void) {
    /* The 48-bit station MAC. Not esp_efuse_mac_get_default: on a chip with an 802.15.4
     * radio (the C5) that one is 8 bytes, the two extension bytes in the middle, so it
     * overran a 6-byte buffer and the 6 bytes kept left out most of the part that differs
     * from board to board. Three XIAOs out of four had the same id (2026-11-27). */
    uint8_t mac[8] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    chip_id = 0;
    for (int i = 0; i < 6; i++) chip_id = (chip_id << 8) | mac[i];
    batch = calloc(PROFILE_MAX_CHANNELS, sizeof(*batch));
    seat = calloc(PROFILE_MAX_CHANNELS, sizeof(*seat));
    dec = calloc(1, sizeof(*dec));
    pending = calloc(1, sizeof(*pending));
    sync_ts = calloc(SYNC_RING, sizeof(*sync_ts));
    if (!batch || !seat || !dec || !pending || !sync_ts) return ESP_ERR_NO_MEM;
    oft_decoder_init(dec);
    return oft_sync_pin(sync_gpio); /* the pin gpio_out_init chose */
}

/* ---- plan ---- */

static void apply_plan_msg(const oft_plan_t *p) {
    got_plan = true;
    portENTER_CRITICAL(&mux);
    memset(seat, 0, PROFILE_MAX_CHANNELS * sizeof(*seat));
    for (unsigned i = 0; i < p->n; i++) {
        if (p->seats[i].seat >= PROFILE_MAX_CHANNELS) continue;
        seat[p->seats[i].seat].mapped = true;
        seat[p->seats[i].seat].freq_mhz = p->seats[i].freq_mhz;
    }
    portEXIT_CRITICAL(&mux);
    scan_config_t c;
    scanner_get_config(&c);
    oft_apply_plan(&c);
    scanner_set_config(&c);
}

void oft_apply_plan(scan_config_t *c) {
    const profile_t *p = c->profile;
    c->nch = PROFILE_MAX_CHANNELS;
    c->enabled = 0;
    for (unsigned j = 0; j < PROFILE_MAX_CHANNELS; j++) {
        snprintf(c->ch[j].name, sizeof(c->ch[j].name), "S%u", j + 1);
        uint32_t khz;
        if (seat[j].mapped && profile_from_equiv_khz(p, seat[j].freq_mhz * 1000u, &khz)) {
            c->ch[j].khz = khz;
            c->enabled |= 1u << j;
        } else {
            c->ch[j].khz = p->ch[j < p->nch ? j : 0].khz; /* placeholder, not scanned */
        }
    }
    c->running = c->enabled != 0;
}

void oft_plan_applied(void) {
    portENTER_CRITICAL(&mux);
    for (unsigned j = 0; j < PROFILE_MAX_CHANNELS; j++) batch[j].n = 0;
    portEXIT_CRITICAL(&mux);
}

/* ---- samples ---- */

/* Take a copy of channel j's batch and clear it. Caller encodes outside the lock. */
static bool take_batch(unsigned j, batch_t *out) {
    portENTER_CRITICAL(&mux);
    bool any = batch[j].n > 0;
    if (any) {
        *out = batch[j];
        batch[j].n = 0;
    }
    portEXIT_CRITICAL(&mux);
    return any;
}

static void send_batch(unsigned j, const batch_t *b) {
    oft_msg_t m = {.type = OFT_MSG_SAMPLES};
    oft_samples_t *s = &m.u.samples;
    s->seat = (uint8_t)j;
    s->t0 = (uint64_t)b->t_first;
    s->dt_us = b->n > 1 ? (uint16_t)((b->t_last - b->t_first) / (b->n - 1)) : 1000;
    s->noise_floor = b->floor_q8 ? q8_to_ddb(b->floor_q8) : INT16_MIN;
    s->flags = b->flags;
    s->n = b->n;
    memcpy(s->power, b->power, sizeof(int16_t) * b->n);
    send(&m);
}

/* Called from the scanner's IRAM hop loop, but kept in flash: the sample is already
 * timestamped (cap_mid), so a cache miss here costs hop rate, not timing accuracy. */
void oft_feed(unsigned ch, int64_t t_us, int32_t rssi_q8, int32_t floor_q8, bool saturated) {
    if (ch >= PROFILE_MAX_CHANNELS) return;
    bool full = false;
    batch_t copy;
    portENTER_CRITICAL(&mux);
    batch_t *b = &batch[ch];
    if (b->n == 0) {
        b->t_first = t_us;
        b->flags = 0;
    }
    b->t_last = t_us;
    b->power[b->n++] = q8_to_ddb(rssi_q8);
    b->floor_q8 = floor_q8;
    if (saturated) b->flags |= OFT_SAMPLE_CLIPPED;
    if (b->n >= BATCH_N) {
        copy = *b;
        b->n = 0;
        full = true;
    }
    portEXIT_CRITICAL(&mux);
    if (full) send_batch(ch, &copy);
}

/* ---- host messages ---- */

static void handle(const oft_msg_t *m, int64_t now_us) {
    switch (m->type) {
    case OFT_MSG_PLAN: apply_plan_msg(&m->u.plan); break;
    case OFT_MSG_PING: {
        oft_msg_t r = {.type = OFT_MSG_PONG};
        r.u.pong = (oft_pong_t){.id = m->u.ping.id, .t_timer = m->u.ping.t_timer, .t_node = (uint64_t)esp_timer_get_time()};
        send(&r);
        break;
    }
    case OFT_MSG_RESET: esp_restart(); break;
    case OFT_MSG_IDENTIFY: /* TODO: blink the status LED for identify_ms */ break;
    case OFT_MSG_DFU: radio_reboot_to_bootloader(); break;
    default: break; /* Node -> Timer types echoed back: ignore */
    }
    (void)now_us;
}

bool oft_sniff(uint8_t b, int64_t now_us) {
    (void)now_us;
    oft_msg_t m;
    if (!oft_decoder_push(dec, b, &m)) return false;
    if (m.type != OFT_MSG_PLAN && m.type != OFT_MSG_PING) return false;
    *pending = m;
    have_pending = true;
    return true;
}

void oft_enter(int64_t now_us) {
    oft_decoder_init(dec);
    got_plan = false;
    send_hello();
    if (have_pending) {
        have_pending = false;
        handle(pending, now_us);
    }
    next_status_us = now_us + STATUS_PERIOD_US;
}

void oft_rx(uint8_t b, int64_t now_us) {
    oft_msg_t m;
    if (oft_decoder_push(dec, b, &m)) handle(&m, now_us);
}

void oft_poll(int64_t now_us) {
    /* SYNC edges. */
    while (sync_rd != sync_wr) {
        int64_t t = sync_ts[sync_rd % SYNC_RING];
        sync_rd++;
        oft_msg_t m = {.type = OFT_MSG_SYNC_EDGE};
        m.u.sync_edge = (oft_sync_edge_t){.edge_seq = ++sync_seq, .t_node = (uint64_t)t};
        send(&m);
    }
    /* Stale partial batches. */
    for (unsigned j = 0; j < PROFILE_MAX_CHANNELS; j++) {
        bool stale;
        portENTER_CRITICAL(&mux);
        stale = batch[j].n > 0 && now_us - batch[j].t_last > BATCH_STALE_US;
        portEXIT_CRITICAL(&mux);
        if (stale) {
            batch_t copy;
            if (take_batch(j, &copy)) send_batch(j, &copy);
        }
    }
    if (now_us >= next_status_us) {
        /* The one Hello sent on entering OFT mode can be lost (the Timer's decoder may still
         * be chewing on boot text). A Timer answers a Hello with a Plan, so repeat it until
         * one arrives. */
        if (!got_plan) send_hello();
        send_status();
        next_status_us = now_us + STATUS_PERIOD_US;
    }
}
