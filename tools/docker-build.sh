#!/bin/sh
# Build the C5 firmware with Espressif's ESP-IDF image; no local toolchain needed.
# Usage: tools/docker-build.sh [idf.py args...]   (default: build)
# Output: firmware/build-c5/{bootloader/bootloader.bin,partition_table/partition-table.bin,oft-node-c5.bin}
# A plain build also writes firmware/build-c5/oft-node-c5-full.bin: all three in one file, to
# write at offset 0 of a blank board over USB. oft-node-c5.bin alone is what the Timer's page takes.
set -e
cd "$(dirname "$0")/.."
IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.5}"  
docker run --rm -v "$PWD":/project -w /project/firmware -e HOME=/tmp "$IMAGE" bash -c \
  "git config --global --add safe.directory /project && idf.py -B build-c5 set-target esp32c5 >/dev/null 2>&1 || true; idf.py -B build-c5 ${*:-build} && { [ -n \"$*\" ] || esptool.py --chip esp32c5 merge_bin -o build-c5/oft-node-c5-full.bin --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 build-c5/bootloader/bootloader.bin 0x8000 build-c5/partition_table/partition-table.bin 0x10000 build-c5/oft-node-c5.bin; }"
