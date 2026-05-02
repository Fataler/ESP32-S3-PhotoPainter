#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

if [[ ! -f build/xiaozhi.map ]]; then
  die "build/xiaozhi.map not found. Run scripts/pp-build.sh first."
fi

MODE="${1:-archives}"
case "$MODE" in
  archives)
    pp_header "Firmware size — archives"
    ;;
  files)
    pp_header "Firmware size — objects"
    ;;
  *)
    die "Usage: scripts/pp-size.sh [archives|files]"
    ;;
esac

printf '  %-10s %s\n' "${D}Map${R}" "build/xiaozhi.map"
pp_rule
echo ""

case "$MODE" in
  archives)
    python "$IDF_PATH/tools/idf_size.py" --archives build/xiaozhi.map
    ;;
  files)
    python "$IDF_PATH/tools/idf_size.py" --files build/xiaozhi.map
    ;;
esac

echo ""
pp_rule
echo ""
