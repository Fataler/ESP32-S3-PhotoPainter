#!/usr/bin/env bash
# Shared terminal styling for PhotoPainter helper scripts (source, do not execute).

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  R=$'\033[0m'
  B=$'\033[1m'
  D=$'\033[2m'
  C=$'\033[36m'
  G=$'\033[32m'
  Y=$'\033[33m'
  M=$'\033[35m'
else
  R= B= D= C= G= Y= M=
fi

die() {
  echo "${B}${Y}Error:${R} $*" >&2
  exit 1
}

pp_rule() {
  local w=$(( ${COLUMNS:-80} - 4 ))
  (( w < 40 )) && w=40
  printf '%s%s%s\n' "$D" "$(printf '%*s' "$w" '' | tr ' ' '─')" "$R"
}

pp_header() {
  local subtitle="$1"
  echo ""
  echo "${B}${C}PhotoPainter${R} ${D}·${R} ${B}${subtitle}${R}"
  pp_rule
}

pp_footer_done() {
  echo ""
  pp_rule
  echo "  ${G}Done.${R}"
  while [[ $# -gt 0 ]]; do
    [[ -n "${1:-}" ]] || { shift; continue; }
    echo "  $1"
    shift
  done
  pp_rule
  echo ""
}
