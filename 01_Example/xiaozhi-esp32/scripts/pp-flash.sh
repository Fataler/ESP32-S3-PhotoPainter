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
        echo "No ESP32 USB serial port found. Pass it explicitly, for example:" >&2
        echo "  $0 /dev/cu.usbmodem1101" >&2
        exit 1
    fi
fi

idf.py -p "$PORT" flash

