/* OFT Node link codec. Portable C. GPL-3.0-or-later. */
#include "oft_frame.h"
#include <string.h>

uint16_t oft_crc16(const uint8_t *data, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t oft_cobs_encode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    if (cap < n + n / 254 + 1) return 0;
    size_t code_idx = 0, out = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == 0) {
            dst[code_idx] = code;
            code_idx = out++;
            code = 1;
        } else {
            dst[out++] = src[i];
            if (++code == 0xFF) {
                dst[code_idx] = code;
                code_idx = out++;
                code = 1;
            }
        }
    }
    dst[code_idx] = code;
    return out;
}

size_t oft_cobs_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t i = 0, out = 0;
    while (i < n) {
        uint8_t code = src[i++];
        if (code == 0) return 0;
        size_t run = (size_t)code - 1;
        if (i + run > n || out + run > cap) return 0;
        for (size_t k = 0; k < run; k++) {
            if (src[i + k] == 0) return 0;
            dst[out + k] = src[i + k];
        }
        out += run;
        i += run;
        if (code != 0xFF && i < n) {
            if (out >= cap) return 0;
            dst[out++] = 0;
        }
    }
    return out;
}

/* ---- little-endian writer/reader ---- */

typedef struct { uint8_t *p; size_t n, cap; bool ok; } wr_t;
static void w8(wr_t *w, uint8_t v) { if (w->n + 1 > w->cap) { w->ok = false; return; } w->p[w->n++] = v; }
static void w16(wr_t *w, uint16_t v) { w8(w, (uint8_t)v); w8(w, (uint8_t)(v >> 8)); }
static void w32(wr_t *w, uint32_t v) { w16(w, (uint16_t)v); w16(w, (uint16_t)(v >> 16)); }
static void w64(wr_t *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }

typedef struct { const uint8_t *p; size_t n, pos; bool ok; } rd_t;
static uint8_t r8(rd_t *r) { if (r->pos + 1 > r->n) { r->ok = false; return 0; } return r->p[r->pos++]; }
static uint16_t r16(rd_t *r) { uint16_t lo = r8(r); return (uint16_t)(lo | ((uint16_t)r8(r) << 8)); }
static uint32_t r32(rd_t *r) { uint32_t lo = r16(r); return lo | ((uint32_t)r16(r) << 16); }
static uint64_t r64(rd_t *r) { uint64_t lo = r32(r); return lo | ((uint64_t)r32(r) << 32); }

static size_t encode_payload(const oft_msg_t *m, uint8_t *out, size_t cap) {
    wr_t w = {.p = out, .n = 0, .cap = cap, .ok = true};
    w8(&w, OFT_LINK_VERSION);
    w8(&w, m->type);
    switch (m->type) {
    case OFT_MSG_HELLO: {
        const oft_hello_t *h = &m->u.hello;
        w64(&w, h->chip_id);
        for (int i = 0; i < 3; i++) w8(&w, h->fw[i]);
        w8(&w, h->link_version);
        w16(&w, h->freq_min_mhz);
        w16(&w, h->freq_max_mhz);
        w8(&w, h->max_seats);
        w16(&w, h->features);
        break;
    }
    case OFT_MSG_PLAN: {
        const oft_plan_t *p = &m->u.plan;
        if (p->n > OFT_MAX_PLAN_SEATS) return 0;
        w32(&w, p->dwell_us);
        w16(&w, p->filter_bw_khz);
        w8(&w, p->gain_index);
        w8(&w, p->n);
        for (unsigned i = 0; i < p->n; i++) { w8(&w, p->seats[i].seat); w16(&w, p->seats[i].freq_mhz); }
        break;
    }
    case OFT_MSG_SAMPLES: {
        const oft_samples_t *s = &m->u.samples;
        if (s->n > OFT_MAX_SAMPLES) return 0;
        w8(&w, s->seat);
        w64(&w, s->t0);
        w16(&w, s->dt_us);
        w16(&w, (uint16_t)s->noise_floor);
        w8(&w, s->flags);
        w16(&w, s->n);
        for (unsigned i = 0; i < s->n; i++) w16(&w, (uint16_t)s->power[i]);
        break;
    }
    case OFT_MSG_SYNC_EDGE: w32(&w, m->u.sync_edge.edge_seq); w64(&w, m->u.sync_edge.t_node); break;
    case OFT_MSG_PING: w32(&w, m->u.ping.id); w64(&w, m->u.ping.t_timer); break;
    case OFT_MSG_PONG: w32(&w, m->u.pong.id); w64(&w, m->u.pong.t_timer); w64(&w, m->u.pong.t_node); break;
    case OFT_MSG_STATUS:
        w16(&w, (uint16_t)m->u.status.temp_dc);
        w16(&w, m->u.status.hop_errors);
        w16(&w, m->u.status.overruns);
        w8(&w, m->u.status.gain_index);
        break;
    case OFT_MSG_RESET:
    case OFT_MSG_DFU: break;
    case OFT_MSG_IDENTIFY: w16(&w, m->u.identify_ms); break;
    default: return 0;
    }
    return w.ok ? w.n : 0;
}

size_t oft_encode_frame(const oft_msg_t *m, uint8_t *out, size_t cap) {
    uint8_t payload[OFT_MAX_PAYLOAD + 2];
    size_t n = encode_payload(m, payload, OFT_MAX_PAYLOAD);
    if (!n) return 0;
    uint16_t crc = oft_crc16(payload, n);
    payload[n++] = (uint8_t)crc;
    payload[n++] = (uint8_t)(crc >> 8);
    if (cap < 2) return 0;
    size_t len = oft_cobs_encode(payload, n, out, cap - 1);
    if (!len) return 0;
    out[len++] = 0;
    return len;
}

bool oft_decode_payload(const uint8_t *payload, size_t n, oft_msg_t *out) {
    rd_t r = {.p = payload, .n = n, .pos = 0, .ok = true};
    if (r8(&r) != OFT_LINK_VERSION) return false;
    out->type = r8(&r);
    switch (out->type) {
    case OFT_MSG_HELLO: {
        oft_hello_t *h = &out->u.hello;
        h->chip_id = r64(&r);
        for (int i = 0; i < 3; i++) h->fw[i] = r8(&r);
        h->link_version = r8(&r);
        h->freq_min_mhz = r16(&r);
        h->freq_max_mhz = r16(&r);
        h->max_seats = r8(&r);
        h->features = r16(&r);
        break;
    }
    case OFT_MSG_PLAN: {
        oft_plan_t *p = &out->u.plan;
        p->dwell_us = r32(&r);
        p->filter_bw_khz = r16(&r);
        p->gain_index = r8(&r);
        p->n = r8(&r);
        if (p->n > OFT_MAX_PLAN_SEATS) return false;
        for (unsigned i = 0; i < p->n; i++) { p->seats[i].seat = r8(&r); p->seats[i].freq_mhz = r16(&r); }
        break;
    }
    case OFT_MSG_SAMPLES: {
        oft_samples_t *s = &out->u.samples;
        s->seat = r8(&r);
        s->t0 = r64(&r);
        s->dt_us = r16(&r);
        s->noise_floor = (int16_t)r16(&r);
        s->flags = r8(&r);
        s->n = r16(&r);
        if (s->n > OFT_MAX_SAMPLES) return false;
        for (unsigned i = 0; i < s->n; i++) s->power[i] = (int16_t)r16(&r);
        break;
    }
    case OFT_MSG_SYNC_EDGE: out->u.sync_edge.edge_seq = r32(&r); out->u.sync_edge.t_node = r64(&r); break;
    case OFT_MSG_PING: out->u.ping.id = r32(&r); out->u.ping.t_timer = r64(&r); break;
    case OFT_MSG_PONG: out->u.pong.id = r32(&r); out->u.pong.t_timer = r64(&r); out->u.pong.t_node = r64(&r); break;
    case OFT_MSG_STATUS:
        out->u.status.temp_dc = (int16_t)r16(&r);
        out->u.status.hop_errors = r16(&r);
        out->u.status.overruns = r16(&r);
        out->u.status.gain_index = r8(&r);
        break;
    case OFT_MSG_RESET:
    case OFT_MSG_DFU: break;
    case OFT_MSG_IDENTIFY: out->u.identify_ms = r16(&r); break;
    default: return false;
    }
    return r.ok;
}

void oft_decoder_init(oft_decoder_t *d) { memset(d, 0, sizeof(*d)); }

bool oft_decoder_push(oft_decoder_t *d, uint8_t byte, oft_msg_t *out) {
    if (byte != 0) {
        if (d->len < sizeof(d->buf)) d->buf[d->len++] = byte;
        else d->overflow = true;
        return false;
    }
    size_t len = d->len;
    bool overflow = d->overflow;
    d->len = 0;
    d->overflow = false;
    if (overflow) { d->bad_frames++; return false; }
    if (len == 0) return false; /* back-to-back delimiters */
    uint8_t raw[OFT_MAX_FRAME];
    size_t n = oft_cobs_decode(d->buf, len, raw, sizeof(raw));
    if (n < 4) { d->bad_frames++; return false; }
    uint16_t crc = (uint16_t)(raw[n - 2] | ((uint16_t)raw[n - 1] << 8));
    if (oft_crc16(raw, n - 2) != crc) { d->bad_frames++; return false; }
    if (!oft_decode_payload(raw, n - 2, out)) { d->bad_frames++; return false; }
    return true;
}
