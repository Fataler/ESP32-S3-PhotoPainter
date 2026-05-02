#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

pp_header "Firmware build"
printf '  %-10s %s\n' "${D}Project${R}" "$PROJECT_DIR"
pp_rule
echo ""

idf.py build

pp_footer_done "${D}Binary: build/xiaozhi.bin${R}"
