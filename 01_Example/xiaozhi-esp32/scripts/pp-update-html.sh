#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=pp-style.sh
source "$SCRIPT_DIR/pp-style.sh"

PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SOURCE_DIR="$PROJECT_ROOT/02_SDCARD/03_sys_ap_html"

usage() {
  cat <<EOF
Usage:
  scripts/pp-update-html.sh [SD_VOLUME_OR_HTML_DIR]

Examples:
  scripts/pp-update-html.sh
  scripts/pp-update-html.sh "/Volumes/NO NAME"
  scripts/pp-update-html.sh "/Volumes/NO NAME/03_sys_ap_html"

Copies files from:
  $SOURCE_DIR

To:
  <SD volume>/03_sys_ap_html
EOF
}

resolve_target_dir() {
  local input="${1:-}"

  if [[ -n "$input" ]]; then
    if [[ -d "$input/03_sys_ap_html" ]]; then
      printf '%s\n' "$input/03_sys_ap_html"
      return
    fi
    if [[ "$(basename "$input")" == "03_sys_ap_html" ]]; then
      printf '%s\n' "$input"
      return
    fi
    die "Target must be an SD volume containing 03_sys_ap_html or the 03_sys_ap_html directory itself: $input"
  fi

  local matches=()
  local volume
  for volume in /Volumes/*; do
    [[ -d "$volume" ]] || continue
    [[ -d "$volume/03_sys_ap_html" ]] || continue

    if [[ -d "$volume/02_sys_ap_img" || -d "$volume/06_user_foundation_img" || -d "$volume/06_user_Foundation_img" ]]; then
      matches+=("$volume/03_sys_ap_html")
    fi
  done

  if [[ "${#matches[@]}" -eq 0 ]]; then
    die "No PhotoPainter SD card found. Pass the volume path explicitly, for example: scripts/pp-update-html.sh \"/Volumes/NO NAME\""
  fi
  if [[ "${#matches[@]}" -gt 1 ]]; then
    echo "${Y}Found multiple possible SD cards:${R}" >&2
    printf "  ${D}%s${R}\n" "${matches[@]}" >&2
    die "Pass the target path explicitly."
  fi

  printf '%s\n' "${matches[0]}"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

[[ -d "$SOURCE_DIR" ]] || die "Source directory not found: $SOURCE_DIR"

TARGET_DIR="$(resolve_target_dir "${1:-}")"
[[ -d "$TARGET_DIR" ]] || die "Target directory not found: $TARGET_DIR"

case "$TARGET_DIR" in
  /Volumes/*/03_sys_ap_html|/Volumes/*/*/03_sys_ap_html) ;;
  *) die "Refusing to update a target outside /Volumes: $TARGET_DIR" ;;
esac

pp_header "Web UI (SD card)"
printf '  %-10s %s\n' "${D}Source${R}" "$SOURCE_DIR/"
printf '  %-10s %s\n' "${D}Target${R}" "${M}${TARGET_DIR}/${R}"
pp_rule
echo ""
echo "  ${C}→${R}  ${D}rsync (mirror, delete extras on card)${R}"
echo ""

rsync -av --delete --stats \
  --exclude='.DS_Store' \
  "$SOURCE_DIR/" \
  "$TARGET_DIR/"

sync

pp_footer_done \
  "${D}HTML on the SD card is in sync with the repo copy.${R}" \
  "${D}Eject the volume safely before removing the card.${R}"
