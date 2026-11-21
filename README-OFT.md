# oft-node-c5 — OFT Node firmware for the XIAO ESP32-C5

A fork of [widscan58](https://github.com/CapnBry/widscan58) (GPL-3.0-or-later; upstream is the
`upstream` remote) that adds an **OFT Node mode**: the C5 streams per-seat signal levels to an
OFT Timer over the host UART using the Node link (oft-spec §5). Detection runs on the Timer.
Upstream's scanner, RotorHazard mode and monitor mode are unchanged and still work.

## Acknowledgements

This firmware is [widscan58](https://github.com/CapnBry/widscan58) by CapnBry with an OFT
mode added. Everything that makes the radio work is theirs and, underneath,
[esp-sdr](https://github.com/ESPARGOS/esp-sdr)'s and [C5VRX](https://github.com/Twotoz/C5VRX)'s;
see the Credits in [README.md](README.md).

The idea of timing FPV laps with an ESP32-C5 comes from Louis Hitchcock's
[FPVGateC5MK](https://github.com/LouisHitchcock/FPVGateC5MK),
[FPVGateC5RX](https://github.com/LouisHitchcock/FPVGateC5RX) and
[FPVGate](https://github.com/LouisHitchcock/FPVGate). Their licence (CC BY-NC-SA 4.0) does
not combine with GPL, so the OFT mode uses none of their code. The full list of projects
OFT draws on is in the [oft README](https://github.com/Open-FPV-Timer/oft#acknowledgements).

## What the OFT mode adds

| File | Role |
|---|---|
| `firmware/src/proto/oft_frame.{c,h}` | Portable codec: COBS + CRC-16 frames and all Node-link messages. Byte-identical to the Rust `oft-nodelink` crate (tested). |
| `firmware/src/proto/oft.{c,h}` | ESP glue: Hello, Plan → scanner channels, Samples batching, Ping/Pong, SYNC edge ISR, Status, Reset, Dfu. |
| `scan/metrics.{c,h}` | `metrics_level_t` gains `rssi_q8` / `floor_q8` (level code × 256) so OFT gets 1/1024 dB resolution instead of 0.25 dB codes. |
| `scan/scanner.{c,h}` | `scan_config_t.oft`; `do_levels` feeds `oft_feed` per channel per hop. |
| `proto/commands.c` | `PROTO_OFT`: the first CRC-valid Plan or Ping frame from the host switches the device into OFT mode until reboot. |
| `metrics.c`, `commands.c` | `metrics_log2_dev` and `other_cfg` moved to the heap: the C5's static RAM must stay below the RF capture bank (`sram_guard.ld`) and upstream ends 40 bytes under it. OFT buffers are heap-allocated for the same reason; `oft_feed` and the SYNC ISR stay in flash. |

Mode switching needs no command: boot in RotorHazard mode as before; a Timer speaking the
Node link is recognised by its frames. `PROTOCOL?` answers `PROTOCOL OFT`.

Units on the link: power in **0.1 dB relative to full scale** (level code 255 = 0 dBFS).

## Build

```sh
tools/docker-build.sh            # ESP-IDF 5.5 in Docker, output in firmware/build-c5/
firmware/test/test_oft/run.sh    # host test of the codec against the Rust reference vectors
```

Flash (XIAO C5 over USB-C; the bootloader goes at 0x2000 on the C5):

```sh
esptool --chip esp32c5 -p /dev/ttyACM0 --before no-reset --after watchdog-reset write-flash \
  0x2000 firmware/build-c5/bootloader/bootloader.bin \
  0x8000 firmware/build-c5/partition_table/partition-table.bin \
  0x10000 firmware/build-c5/widscan58.bin
```

See upstream's `CLAUDE.md` for the C5 rev 1.0 USB-reset caveat and PlatformIO builds.

## Flasher page

https://oft.reconfpv.com/ flashes a board from the browser and sets its pins (`web/index.html`,
`web/src/setup.ts`). It is deliberately small: once a Node is flashed, everything else is done
on the Timer's page. Upstream's dashboard (spectrum, lap timer, power meter) is still built,
at `scanner.html` (`web/src/main.ts`), linked from the flasher's footer.

## Wiring to the Timer

Host UART = UART0 on XIAO **D6 (GPIO11) TX** and **D7 (GPIO12) RX**, 921600 8N1 (upstream's
default). SYNC input: default GPIO2, the MTMS pad on the back of the XIAO (`OFT_SYNC_GPIO`
in `oft.h`). Like the UART pins it is a GPIO setting: `GPIO <n> SYNC` then `GPIO SAVE`, or
Set pins on the flasher page, moves it to another pin (for example `GPIO 8 SYNC` for
header pin D8) with no rebuild.

## Status (2026-11-08)

Builds with IDF 5.5.5 in Docker and passes upstream's RF-bank SRAM guard with ~600 B to spare (5.5.1 lacks a private PHY symbol upstream uses); not yet run on hardware. `firmware/build-c5/widscan58.bin` is the app image. Open items: Identify (status LED),
C5 temperature in Status, post-hop sample flag, hop-error count.
