#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

OUT_FILE="${1:-build/photopainter_merged.bin}"

idf.py build
python -m esptool --chip esp32s3 merge_bin \
    -o "$OUT_FILE" \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xd000 build/ota_data_initial.bin \
    0x20000 build/xiaozhi.bin

echo "Merged firmware ready: $PROJECT_DIR/$OUT_FILE"

