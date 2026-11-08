/* OFT Node link, wire codec (oft-spec §5; mirrors the Rust crate oft-nodelink).
 * Portable C, no ESP-IDF: also compiled into the host tests. GPL-3.0-or-later.
 *
 * Frame: COBS( payload || crc16_le(payload) ) 0x00
 * payload = version:u8 type:u8 body...   Little-endian throughout.
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the payload. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OFT_LINK_VERSION 1
#define OFT_MAX_PLAN_SEATS 8
/* Samples per frame this build can hold. The wire format allows 256 (the Timer side keeps
 * that); a Node that only ever sends small batches builds with e.g. -DOFT_MAX_SAMPLES=32 to
 * keep its static RAM small. */
#ifndef OFT_MAX_SAMPLES
#define OFT_MAX_SAMPLES 256
#endif
/* Largest payload: a Samples message (18-byte header + 2 bytes per sample), rounded up. */
#define OFT_MAX_PAYLOAD (20 + 2 * OFT_MAX_SAMPLES)
/* COBS worst case for payload + CRC, plus the delimiter. */
#define OFT_MAX_FRAME (OFT_MAX_PAYLOAD + 2 + (OFT_MAX_PAYLOAD + 2) / 254 + 1 + 1)

enum {
    OFT_MSG_HELLO = 0x01,
    OFT_MSG_PLAN = 0x02,
    OFT_MSG_SAMPLES = 0x03,
    OFT_MSG_SYNC_EDGE = 0x05,
    OFT_MSG_PING = 0x06,
    OFT_MSG_PONG = 0x07,
    OFT_MSG_STATUS = 0x09,
    OFT_MSG_RESET = 0x10,
    OFT_MSG_IDENTIFY = 0x12,
    OFT_MSG_DFU = 0x14,
};

enum { OFT_FEAT_SYNC = 1u << 0, OFT_FEAT_IQ_DUMP = 1u << 1, OFT_FEAT_DFU = 1u << 2 };
enum { OFT_SAMPLE_CLIPPED = 1u << 0, OFT_SAMPLE_POST_HOP = 1u << 1 };
#define OFT_GAIN_AUTO 0xFF

typedef struct {
    uint64_t chip_id;
    uint8_t fw[3];
    uint8_t link_version;
    uint16_t freq_min_mhz, freq_max_mhz;
    uint8_t max_seats;
    uint16_t features;
} oft_hello_t;

typedef struct {
    uint32_t dwell_us;
    uint16_t filter_bw_khz;
    uint8_t gain_index;
    uint8_t n;
    struct { uint8_t seat; uint16_t freq_mhz; } seats[OFT_MAX_PLAN_SEATS];
} oft_plan_t;

typedef struct {
    uint8_t seat;
    uint64_t t0;
    uint16_t dt_us;
    int16_t noise_floor;
    uint8_t flags;
    uint16_t n;
    int16_t power[OFT_MAX_SAMPLES];
} oft_samples_t;

typedef struct { uint32_t edge_seq; uint64_t t_node; } oft_sync_edge_t;
typedef struct { uint32_t id; uint64_t t_timer; } oft_ping_t;
typedef struct { uint32_t id; uint64_t t_timer, t_node; } oft_pong_t;
typedef struct { int16_t temp_dc; uint16_t hop_errors, overruns; uint8_t gain_index; } oft_status_t;

typedef struct {
    uint8_t type; /* OFT_MSG_* */
    union {
        oft_hello_t hello;
        oft_plan_t plan;
        oft_samples_t samples;
        oft_sync_edge_t sync_edge;
        oft_ping_t ping;
        oft_pong_t pong;
        oft_status_t status;
        uint16_t identify_ms;
    } u;
} oft_msg_t;

uint16_t oft_crc16(const uint8_t *data, size_t n);
/* COBS without the delimiter. Return the encoded/decoded length, or 0 on error (decode)
 * / when dst is too small (encode). */
size_t oft_cobs_encode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
size_t oft_cobs_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

/* Encode a message as one complete frame including the trailing 0x00. Returns the
 * length, or 0 if it does not fit. */
size_t oft_encode_frame(const oft_msg_t *m, uint8_t *out, size_t cap);

/* Streaming decoder: feed bytes; returns true when a complete, CRC-valid frame has been
 * decoded into *out. Bad frames are dropped and counted. */
typedef struct {
    uint8_t buf[OFT_MAX_FRAME];
    size_t len;
    bool overflow;
    uint32_t bad_frames;
} oft_decoder_t;

void oft_decoder_init(oft_decoder_t *d);
bool oft_decoder_push(oft_decoder_t *d, uint8_t byte, oft_msg_t *out);

/* Decode a payload (version, type, body) only; false on any error. */
bool oft_decode_payload(const uint8_t *payload, size_t n, oft_msg_t *out);
