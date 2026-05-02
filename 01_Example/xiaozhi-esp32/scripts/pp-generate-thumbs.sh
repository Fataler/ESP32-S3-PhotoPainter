#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=pp-style.sh
source "$SCRIPT_DIR/pp-style.sh"

usage() {
  cat <<EOF
Usage:
  scripts/pp-generate-thumbs.sh [--force] [SD_VOLUME_OR_COLLECTION_DIR]

Examples:
  scripts/pp-generate-thumbs.sh
  scripts/pp-generate-thumbs.sh --force
  scripts/pp-generate-thumbs.sh "/Volumes/NO NAME"
  scripts/pp-generate-thumbs.sh "/Volumes/NO NAME/06_user_foundation_img"

Generates JPEG thumbnails in:
  <collection>/.thumbs/<original-name>.jpg
EOF
}

FORCE=0
TARGET_INPUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force)
      FORCE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      TARGET_INPUT="$1"
      shift
      ;;
  esac
done

resolve_collection_dir() {
  local input="${1:-}"

  if [[ -n "$input" ]]; then
    if [[ -d "$input/06_user_foundation_img" ]]; then
      printf '%s\n' "$input/06_user_foundation_img"
      return
    fi
    if [[ -d "$input/06_user_Foundation_img" ]]; then
      printf '%s\n' "$input/06_user_Foundation_img"
      return
    fi
    if [[ "$(basename "$input")" == "06_user_foundation_img" || "$(basename "$input")" == "06_user_Foundation_img" ]]; then
      [[ -d "$input" ]] || die "Collection directory not found: $input"
      printf '%s\n' "$input"
      return
    fi
    die "Target must be an SD volume or the 06_user_foundation_img directory itself: $input"
  fi

  local matches=()
  local volume
  for volume in /Volumes/*; do
    [[ -d "$volume" ]] || continue
    if [[ -d "$volume/06_user_foundation_img" ]]; then
      matches+=("$volume/06_user_foundation_img")
    elif [[ -d "$volume/06_user_Foundation_img" ]]; then
      matches+=("$volume/06_user_Foundation_img")
    fi
  done

  if [[ "${#matches[@]}" -eq 0 ]]; then
    die "No PhotoPainter SD collection found. Pass the volume path explicitly, for example: scripts/pp-generate-thumbs.sh \"/Volumes/NO NAME\""
  fi
  if [[ "${#matches[@]}" -gt 1 ]]; then
    echo "${Y}Found multiple possible collections:${R}" >&2
    printf "  ${D}%s${R}\n" "${matches[@]}" >&2
    die "Pass the target path explicitly."
  fi

  printf '%s\n' "${matches[0]}"
}

COLLECTION_DIR="$(resolve_collection_dir "$TARGET_INPUT")"
THUMB_DIR="$COLLECTION_DIR/.thumbs"
mkdir -p "$THUMB_DIR"

if ! command -v sips >/dev/null 2>&1; then
  die "sips is required on macOS to generate thumbnails."
fi

pp_header "Gallery thumbnails"
printf '  %-12s %s\n' "${D}Collection${R}" "$COLLECTION_DIR"
printf '  %-12s %s\n' "${D}Thumbs${R}" "$THUMB_DIR"
[[ "$FORCE" -eq 1 ]] && printf '  %-12s %s\n' "${D}Mode${R}" "${Y}--force${R} ${D}(rebuild)${R}"
pp_rule
echo ""

generated=0
skipped=0
failed=0

while IFS= read -r -d '' image; do
  name="$(basename "$image")"
  thumb="$THUMB_DIR/$name.jpg"

  if [[ "$FORCE" -eq 0 && -f "$thumb" && "$thumb" -nt "$image" ]]; then
    ((skipped+=1)) || true
    continue
  fi

  tmp="$thumb.tmp.jpg"
  if sips -s format jpeg -s formatOptions 88 -Z 420 "$image" --out "$tmp" >/dev/null 2>&1; then
    mv "$tmp" "$thumb"
    ((generated+=1)) || true
    printf "  %s  %s\n" "${G}✓${R}" "$name"
  else
    rm -f "$tmp"
    ((failed+=1)) || true
    printf "  %s  %s\n" "${Y}✗${R}" "$name" >&2
  fi
done < <(find "$COLLECTION_DIR" -maxdepth 1 -type f \( -iname '*.bmp' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \) ! -name 'sys_decode.bmp' -print0)

sync

echo ""
pp_rule
echo "  ${G}Summary:${R} ${D}generated${R} ${generated}  ${D}skipped${R} ${skipped}  ${Y}failed${R} ${failed}"
echo "  ${D}Thumbnails live under .thumbs next to your gallery images.${R}"
pp_rule
echo ""
