/* OFT Node mode: the device as an Open FPV Timer Node (oft-spec §5). The Timer (an RP2350
 * or oftd) talks the Node link over the host UART; the scanner's per-channel levels stream
 * out as Samples frames; no detection runs here. GPL-3.0-or-later.
 *
 * Mode entry: the command task feeds every host byte to oft_sniff while in another mode;
 * the first CRC-valid Plan or Ping frame switches to OFT mode (commands.c). A Reset frame
 * reboots the device, which comes back in its boot default mode. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "scanner.h"

/* SYNC input from the Timer. Default: XIAO ESP32-C5 D2 = GPIO? -- check the XIAO pinout and
 * the HBOM; any free input works. Set to -1 to disable. */
#ifndef OFT_SYNC_GPIO
#define OFT_SYNC_GPIO 2
#endif

esp_err_t oft_init(void);

/* Bytes from the host while in another mode: true when a CRC-valid OFT frame that a Timer
 * would send first (Plan or Ping) has arrived. The frame is kept and acted on by oft_enter. */
bool oft_sniff(uint8_t b, int64_t now_us);
/* Entering OFT mode: send Hello, apply any frame oft_sniff kept. */
void oft_enter(int64_t now_us);
/* Bytes from the host in OFT mode. */
void oft_rx(uint8_t b, int64_t now_us);
/* Regularly from the command task: drain SYNC edges, send Status once a second, flush
 * sample batches that stopped growing. */
void oft_poll(int64_t now_us);

/* Scan plan from the last Plan frame: channels from the seats' frequencies (like rh_apply_plan). */
void oft_apply_plan(scan_config_t *c);
/* The scanner switched to a new plan: restart the sample batches. */
void oft_plan_applied(void);
/* One level update from the scanner task. rssi_q8/floor_q8 are metrics level codes x 256
 * (0.25 dB per code); floor_q8 0 when the hop had no floor. */
void oft_feed(unsigned ch, int64_t t_us, int32_t rssi_q8, int32_t floor_q8, bool saturated);
