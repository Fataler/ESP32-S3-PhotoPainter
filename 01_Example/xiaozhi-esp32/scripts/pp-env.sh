#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=pp-style.sh
source "$SCRIPT_DIR/pp-style.sh"

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_DIR="$(cd "$PROJECT_DIR/../../.." && pwd)"

export IDF_PATH="${IDF_PATH:-$WORKSPACE_DIR/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$WORKSPACE_DIR/.espressif}"

if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
  for env_dir in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
    if [[ -d "$env_dir" ]]; then
      export IDF_PYTHON_ENV_PATH="$env_dir"
      break
    fi
  done
fi

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  die "ESP-IDF not found at: $IDF_PATH"
fi

if [[ -z "${IDF_PYTHON_ENV_PATH:-}" || ! -d "$IDF_PYTHON_ENV_PATH" ]]; then
  die "ESP-IDF Python env not found under: $IDF_TOOLS_PATH/python_env"
fi

export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
source "$IDF_PATH/export.sh" >/dev/null
cd "$PROJECT_DIR"
