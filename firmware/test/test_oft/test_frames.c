/* Host test for proto/oft_frame.c: CRC/COBS vectors, round trips, byte-exact match with the
 * Rust reference encoding, and decoder robustness. Build: see run.sh. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oft_frame.h"
#include "vectors.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static size_t unhex(const char *h, uint8_t *out) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    return n;
}

static void expect_frame(const char *name, const oft_msg_t *m, const char *hex) {
    uint8_t want[OFT_MAX_FRAME], got[OFT_MAX_FRAME];
    size_t wn = unhex(hex, want);
    size_t gn = oft_encode_frame(m, got, sizeof(got));
    if (gn != wn || memcmp(got, want, wn) != 0) {
        printf("FAIL %s: encoding differs from Rust (%zu vs %zu bytes)\n  got  ", name, gn, wn);
        for (size_t i = 0; i < gn; i++) printf("%02x", got[i]);
        printf("\n  want %s\n", hex);
        fails++;
    }
    /* And the Rust bytes decode back to the same message. */
    oft_decoder_t d; oft_decoder_init(&d);
    oft_msg_t back; bool done = false;
    for (size_t i = 0; i < wn; i++) done = oft_decoder_push(&d, want[i], &back) || done;
    CHECK(done);
    CHECK(back.type == m->type);
    if (m->type == OFT_MSG_SAMPLES) {
        CHECK(back.u.samples.n == m->u.samples.n && memcmp(back.u.samples.power, m->u.samples.power, 2 * m->u.samples.n) == 0);
        CHECK(back.u.samples.t0 == m->u.samples.t0 && back.u.samples.noise_floor == m->u.samples.noise_floor);
    } else if (m->type == OFT_MSG_PLAN) {
        CHECK(back.u.plan.n == m->u.plan.n && back.u.plan.seats[0].freq_mhz == m->u.plan.seats[0].freq_mhz);
    } else if (m->type == OFT_MSG_HELLO) {
        CHECK(back.u.hello.chip_id == m->u.hello.chip_id && back.u.hello.features == m->u.hello.features);
        CHECK(!strcmp(back.u.hello.chip, m->u.hello.chip));
    } else if (m->type == OFT_MSG_PONG) {
        CHECK(back.u.pong.t_node == m->u.pong.t_node);
    }
}

int main(void) {
    /* CRC-16/CCITT-FALSE check value. */
    CHECK(oft_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    /* COBS known vector. */
    { uint8_t in[] = {0x11, 0x22, 0x00, 0x33}, out[16], back[16];
      size_t n = oft_cobs_encode(in, 4, out, sizeof(out));
      CHECK(n == 5 && out[0] == 3 && out[1] == 0x11 && out[2] == 0x22 && out[3] == 2 && out[4] == 0x33);
      CHECK(oft_cobs_decode(out, n, back, sizeof(back)) == 4 && memcmp(back, in, 4) == 0); }
    /* Runs around 254. */
    for (size_t len = 253; len <= 509; len += 1) {
        uint8_t in[600], out[700], back[600];
        memset(in, 1, len);
        size_t n = oft_cobs_encode(in, len, out, sizeof(out));
        CHECK(n > 0);
        for (size_t i = 0; i < n; i++) CHECK(out[i] != 0);
        CHECK(oft_cobs_decode(out, n, back, sizeof(back)) == len && memcmp(back, in, len) == 0);
    }

    oft_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = OFT_MSG_HELLO;
    m.u.hello = (oft_hello_t){.chip_id = 0x0000112233445566ULL, .fw = {0, 1, 0}, .link_version = 1, .freq_min_mhz = 5645, .freq_max_mhz = 5945, .max_seats = 8, .features = OFT_FEAT_SYNC | OFT_FEAT_DFU};
    expect_frame("hello", &m, VEC_HELLO);
    strcpy(m.u.hello.chip, "esp32c5");
    expect_frame("hello_chip", &m, VEC_HELLO_CHIP);

    memset(&m, 0, sizeof m); m.type = OFT_MSG_PLAN;
    m.u.plan.gain_index = OFT_GAIN_AUTO; m.u.plan.n = 1; m.u.plan.seats[0].seat = 0; m.u.plan.seats[0].freq_mhz = 5800;
    expect_frame("plan", &m, VEC_PLAN);

    memset(&m, 0, sizeof m); m.type = OFT_MSG_SAMPLES;
    m.u.samples = (oft_samples_t){.seat = 0, .t0 = 1000000, .dt_us = 1000, .noise_floor = -920, .flags = 0, .n = 3};
    m.u.samples.power[0] = -915; m.u.samples.power[1] = -910; m.u.samples.power[2] = -600;
    expect_frame("samples", &m, VEC_SAMPLES);

    memset(&m, 0, sizeof m); m.type = OFT_MSG_SYNC_EDGE; m.u.sync_edge = (oft_sync_edge_t){.edge_seq = 7, .t_node = 123456789};
    expect_frame("sync_edge", &m, VEC_SYNC_EDGE);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_PING; m.u.ping = (oft_ping_t){.id = 9, .t_timer = 42};
    expect_frame("ping", &m, VEC_PING);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_PONG; m.u.pong = (oft_pong_t){.id = 9, .t_timer = 42, .t_node = 4242};
    expect_frame("pong", &m, VEC_PONG);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_STATUS; m.u.status = (oft_status_t){.temp_dc = 413, .hop_errors = 1, .overruns = 2, .gain_index = 60};
    expect_frame("status", &m, VEC_STATUS);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_RESET; expect_frame("reset", &m, VEC_RESET);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_IDENTIFY; m.u.identify_ms = 3000; expect_frame("identify", &m, VEC_IDENTIFY);
    memset(&m, 0, sizeof m); m.type = OFT_MSG_DFU; expect_frame("dfu", &m, VEC_DFU);

    /* Largest Samples frame fits. */
    memset(&m, 0, sizeof m); m.type = OFT_MSG_SAMPLES; m.u.samples.n = OFT_MAX_SAMPLES; m.u.samples.dt_us = 1000;
    { uint8_t out[OFT_MAX_FRAME]; CHECK(oft_encode_frame(&m, out, sizeof(out)) > 0); }

    /* A corrupted byte fails CRC and the next frame still decodes. */
    { uint8_t a[64], b[64]; size_t na = unhex(VEC_PING, a), nb = unhex(VEC_PONG, b);
      a[2] ^= 0x40;
      oft_decoder_t d; oft_decoder_init(&d); oft_msg_t out; int got = 0;
      for (size_t i = 0; i < na; i++) if (oft_decoder_push(&d, a[i], &out)) got++;
      CHECK(got == 0 && d.bad_frames == 1);
      for (size_t i = 0; i < nb; i++) if (oft_decoder_push(&d, b[i], &out)) got++;
      CHECK(got == 1 && out.type == OFT_MSG_PONG); }

    /* Overlong garbage is dropped and the decoder recovers. */
    { oft_decoder_t d; oft_decoder_init(&d); oft_msg_t out;
      for (int i = 0; i < OFT_MAX_FRAME + 10; i++) CHECK(!oft_decoder_push(&d, 0x55, &out));
      CHECK(!oft_decoder_push(&d, 0, &out) && d.bad_frames == 1);
      uint8_t r[16]; size_t nr = unhex(VEC_RESET, r); bool ok = false;
      for (size_t i = 0; i < nr; i++) ok = oft_decoder_push(&d, r[i], &out) || ok;
      CHECK(ok && out.type == OFT_MSG_RESET); }

    /* Wrong version / unknown type are rejected. */
    { uint8_t p1[] = {2, OFT_MSG_RESET}, p2[] = {1, 0x7F}; oft_msg_t out;
      CHECK(!oft_decode_payload(p1, 2, &out)); CHECK(!oft_decode_payload(p2, 2, &out)); }

    printf(fails ? "%d FAILED\n" : "all oft_frame tests passed\n", fails);
    return fails ? 1 : 0;
}
