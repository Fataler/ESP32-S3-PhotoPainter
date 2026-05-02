#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  matches=(/dev/cu.usbmodem*)
  if [[ -e "${matches[0]}" ]]; then
    PORT="${matches[0]}"
  else
    die "No ESP32 USB serial port found. Pass it explicitly (example: $0 /dev/cu.usbmodem1101)"
  fi
fi

pp_header "Serial monitor"
printf '  %-10s %s\n' "${D}Port${R}" "${M}$PORT${R}"
printf '  %-10s %s\n' "${D}Project${R}" "$PROJECT_DIR"
pp_rule
echo "${D}  (Ctrl+] to exit idf monitor)${R}"
echo ""

idf.py -p "$PORT" monitor
