#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=pp-style.sh
source "$SCRIPT_DIR/pp-style.sh"

usage() {
  cat <<EOF
Usage:
  scripts/mp3_to_ogg.sh <input.mp3> <output.ogg>

Encodes mono Opus at 16 kbps, 16 kHz, 60 ms frames (device-oriented defaults).
Requires ffmpeg with libopus.
EOF
}

[[ $# -eq 2 ]] || { usage >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || die "ffmpeg is required."

pp_header "MP3 → OGG (Opus)"
printf '  %-10s %s\n' "${D}Input${R}" "$1"
printf '  %-10s %s\n' "${D}Output${R}" "$2"
pp_rule
echo ""

ffmpeg -i "$1" -c:a libopus -b:a 16k -ac 1 -ar 16000 -frame_duration 60 "$2"

pp_footer_done "${D}Opus stream written to the path above.${R}"
