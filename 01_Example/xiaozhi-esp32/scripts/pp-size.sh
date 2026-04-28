#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/pp-env.sh"

if [[ ! -f build/xiaozhi.map ]]; then
    echo "build/xiaozhi.map not found. Run scripts/pp-build.sh first." >&2
    exit 1
fi

case "${1:-archives}" in
    archives)
        python "$IDF_PATH/tools/idf_size.py" --archives build/xiaozhi.map
        ;;
    files)
        python "$IDF_PATH/tools/idf_size.py" --files build/xiaozhi.map
        ;;
    *)
        echo "Usage: scripts/pp-size.sh [archives|files]" >&2
        exit 1
        ;;
esac
