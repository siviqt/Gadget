#!/usr/bin/env bash
set -eo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$project_dir"

idf_dir=${IDF_PATH:-"$HOME/.espressif/v6.0.2/esp-idf"}
if [[ ! -f "$idf_dir/export.sh" ]]; then
    printf 'ESP-IDF v6.0.2 not found. Set IDF_PATH to your installed checkout.\n' >&2
    exit 1
fi

if [[ -z ${IDF_TOOLS_PATH:-} && -d "$HOME/.espressif/tools/python_env" ]]; then
    export IDF_TOOLS_PATH="$HOME/.espressif/tools"
fi

export TMPDIR="$project_dir/.cache/tmp"
export PYTHONDONTWRITEBYTECODE=1
export IDF_COMPONENT_CACHE_PATH="$project_dir/.cache/idf-components"
export IDF_COMPONENT_CHECK_NEW_VERSION=${IDF_COMPONENT_CHECK_NEW_VERSION:-false}
export CCACHE_DIR="$project_dir/.cache/ccache"
mkdir -p -- "$TMPDIR"

source "$idf_dir/export.sh"
exec python "$idf_dir/tools/idf.py" "$@"
