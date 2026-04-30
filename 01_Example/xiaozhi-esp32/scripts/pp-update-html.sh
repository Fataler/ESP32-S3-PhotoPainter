#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

die() {
  echo "Error: $*" >&2
  exit 1
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

    # Treat these sibling folders as a guard that this is the PhotoPainter SD card, not a random volume.
    if [[ -d "$volume/02_sys_ap_img" || -d "$volume/06_user_foundation_img" || -d "$volume/06_user_Foundation_img" ]]; then
      matches+=("$volume/03_sys_ap_html")
    fi
  done

  if [[ "${#matches[@]}" -eq 0 ]]; then
    die "No PhotoPainter SD card found. Pass the volume path explicitly, for example: scripts/pp-update-html.sh \"/Volumes/NO NAME\""
  fi
  if [[ "${#matches[@]}" -gt 1 ]]; then
    printf 'Found multiple possible SD cards:\n' >&2
    printf '  %s\n' "${matches[@]}" >&2
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

echo "Updating PhotoPainter web UI:"
echo "  from: $SOURCE_DIR/"
echo "  to:   $TARGET_DIR/"

rsync -av --delete \
  --exclude='.DS_Store' \
  "$SOURCE_DIR/" \
  "$TARGET_DIR/"

sync
echo "HTML update complete."
