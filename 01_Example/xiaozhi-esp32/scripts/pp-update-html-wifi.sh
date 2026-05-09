#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=pp-style.sh
source "$SCRIPT_DIR/pp-style.sh"

PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SOURCE_DIR="$PROJECT_ROOT/02_SDCARD/03_sys_ap_html"
DEFAULT_BASE_URL="http://esp32-s3-photopainter.local"

usage() {
  cat <<EOF
Usage:
  scripts/pp-update-html-wifi.sh [BASE_URL_OR_HOST]

Examples:
  scripts/pp-update-html-wifi.sh
  scripts/pp-update-html-wifi.sh 192.168.4.1
  scripts/pp-update-html-wifi.sh http://esp32-s3-photopainter.local

Uploads files from:
  $SOURCE_DIR

To the frame endpoint:
  /AdminHtmlUpload?name=<file>
EOF
}

human_bytes() {
  awk -v n="$1" 'BEGIN {
    if (n >= 1048576) printf "%.1f MiB", n/1048576
    else if (n >= 1024) printf "%.1f KiB", n/1024
    else printf "%d B", n
  }'
}

md5_file() {
  local path="$1"
  if command -v md5 >/dev/null 2>&1; then
    md5 -q "$path"
  elif command -v md5sum >/dev/null 2>&1; then
    md5sum "$path" | awk '{print $1}'
  else
    die "md5 or md5sum is required."
  fi
}

normalize_base_url() {
  local input="${1:-$DEFAULT_BASE_URL}"
  if [[ "$input" != http://* && "$input" != https://* ]]; then
    input="http://$input"
  fi
  printf '%s\n' "${input%/}"
}

remote_file_matches() {
  local file="$1"
  local size="$2"
  local local_md5="$3"
  local info remote_size remote_md5

  if ! info="$(curl --fail --silent --show-error \
    --connect-timeout 8 \
    --max-time 20 \
    "$BASE_URL/AdminHtmlInfo?name=$file" 2>/dev/null)"; then
    echo "${D}      ${Y}·${R} remote hash check unavailable — will upload${R}" >&2
    return 1
  fi

  if [[ "$info" != *'"exists":true'* ]]; then
    return 1
  fi

  remote_size="$(printf '%s\n' "$info" | sed -n 's/.*"size":\([0-9][0-9]*\).*/\1/p')"
  remote_md5="$(printf '%s\n' "$info" | sed -n 's/.*"md5":"\([0-9a-fA-F]*\)".*/\1/p')"
  remote_md5="$(printf '%s' "$remote_md5" | tr '[:upper:]' '[:lower:]')"
  local_md5="$(printf '%s' "$local_md5" | tr '[:upper:]' '[:lower:]')"
  [[ "$remote_size" == "$size" && "$remote_md5" == "$local_md5" ]]
}

upload_file() {
  local file="$1"
  local path="$2"
  local attempt
  local size_bytes max_time
  size_bytes="$(wc -c < "$path" | tr -d ' ')"
  max_time=$(( 60 + size_bytes / 8192 ))
  if (( max_time < 90 )); then
    max_time=90
  fi
  if (( max_time > 600 )); then
    max_time=600
  fi

  for attempt in 1 2 3; do
    if curl --fail --silent --show-error \
      --connect-timeout 8 \
      --max-time "$max_time" \
      -X POST \
      -H "Content-Type: application/octet-stream" \
      --data-binary "@$path" \
      "$BASE_URL/AdminHtmlUpload?name=$file" >/dev/null; then
      return 0
    fi

    echo "${D}      ${Y}·${R} attempt $attempt failed, retrying…${R}" >&2
    sleep 1
  done

  return 1
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

[[ -d "$SOURCE_DIR" ]] || die "Source directory not found: $SOURCE_DIR"
command -v curl >/dev/null 2>&1 || die "curl is required."

BASE_URL="$(normalize_base_url "${1:-}")"

FILES=(
  "index.html"
  "styles.min.css"
  "script.min.js"
  "bootstrap.min.css"
  "bootstrap.min.js"
  "placeholder.svg"
  "manifest.webmanifest"
)

pp_header "Web UI over Wi-Fi"
printf '  %-10s %s\n' "${D}Source${R}" "$SOURCE_DIR/"
printf '  %-10s %s\n' "${D}Target${R}" "${M}${BASE_URL}/AdminHtmlUpload${R}"
pp_rule
echo ""

skipped=0
uploaded=0

for file in "${FILES[@]}"; do
  path="$SOURCE_DIR/$file"
  [[ -f "$path" ]] || die "Missing source file: $path"
  size="$(wc -c < "$path" | tr -d ' ')"
  hb="$(human_bytes "$size")"
  md5_hash="$(md5_file "$path")"
  short_md5="${md5_hash:0:8}"

  if remote_file_matches "$file" "$size" "$md5_hash"; then
    printf "  %s  %-24s  %8s  ${D}%s${R}\n" "${G}✓${R}" "$file" "$hb" "up to date · md5 ${short_md5}…"
    ((skipped++)) || true
    continue
  fi

  printf "  %s  %-24s  %8s  ${D}%s${R}\n" "${C}↑${R}" "$file" "$hb" "uploading…"
  if upload_file "$file" "$path"; then
    printf "  %s  %-24s  %8s  ${G}%s${R}\n" "${G}✓${R}" "$file" "$hb" "uploaded"
    ((uploaded++)) || true
  else
    die "Failed to upload $file to all configured addresses."
  fi
  sleep 0.5
done

wifi_summary=""
if (( uploaded == 0 && skipped > 0 )); then
  wifi_summary="${G}All ${skipped} file(s) already up to date.${R} ${D}No upload needed.${R}"
elif (( uploaded > 0 && skipped > 0 )); then
  wifi_summary="${G}Summary:${R} ${D}uploaded${R} ${uploaded}  ${D}skipped${R} ${skipped}"
else
  wifi_summary="${G}Uploaded${R} ${uploaded} ${D}file(s).${R}"
fi
pp_footer_done "$wifi_summary" "${D}Refresh the frame page with cache bypass if you do not see changes.${R}"
