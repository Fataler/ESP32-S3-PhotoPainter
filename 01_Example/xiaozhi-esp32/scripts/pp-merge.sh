#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

OUT_FILE="${1:-build/photopainter_merged.bin}"

pp_header "Merge firmware (bootloader + partitions + app)"
printf '  %-10s %s\n' "${D}Output${R}" "${M}$PROJECT_DIR/$OUT_FILE${R}"
pp_rule
echo ""

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

pp_footer_done "${D}Flash merged bin at 0x0 (see scripts/pp-merge.sh / AGENTS.md).${R}"
