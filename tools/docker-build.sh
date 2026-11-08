#!/bin/sh
# Build the C5 firmware with Espressif's ESP-IDF image; no local toolchain needed.
# Usage: tools/docker-build.sh [idf.py args...]   (default: build)
# Output: firmware/build-c5/{bootloader/bootloader.bin,partition_table/partition-table.bin,widscan58.bin}
set -e
cd "$(dirname "$0")/.."
IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.5}"  
docker run --rm -v "$PWD":/project -w /project/firmware -e HOME=/tmp "$IMAGE" bash -c \
  "git config --global --add safe.directory /project && idf.py -B build-c5 set-target esp32c5 >/dev/null 2>&1 || true; idf.py -B build-c5 ${*:-build}"
