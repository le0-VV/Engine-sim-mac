#!/usr/bin/env bash
set -euo pipefail

SCRIPT_START_TS="$(date +%s)"

RUN_METAL_SHADERS=0
USE_ASAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
    --metal-shaders)
        RUN_METAL_SHADERS=1
        shift
        ;;
    --asan)
        USE_ASAN=1
        shift
        ;;
    *)
        echo "Unknown argument: $1" >&2
        echo "Usage: $(basename "$0") [--metal-shaders] [--asan]" >&2
        exit 1
        ;;
    esac
done

log_build() {
    echo "[build-log] $*"
}

report_tool() {
    local tool="$1"
    if command -v "${tool}" >/dev/null 2>&1; then
        local version_line=""
        version_line="$("${tool}" --version 2>&1 | head -n 1 || true)"
        log_build "tool ${tool} found: ${version_line:-version-unavailable}"
    else
        log_build "tool ${tool} not found"
    fi
}

log_build "build start metal_shaders=${RUN_METAL_SHADERS} asan=${USE_ASAN}"
for t in glslangValidator dxc spirv-cross xcrun; do
    report_tool "${t}"
done

if [[ ${RUN_METAL_SHADERS} -eq 1 ]]; then
    local_start_ts="$(date +%s)"
    log_build "shader translation start"
    ./tools/translate_shaders_to_msl.sh --build-metallib
    local_end_ts="$(date +%s)"
    log_build "shader translation end elapsed_s=$((local_end_ts - local_start_ts))"
fi

BUILD_DIR="build"
if [[ ${USE_ASAN} -eq 1 ]]; then
    BUILD_DIR="build-asan"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    cmake -S . -B "${BUILD_DIR}" -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
        -DENGINE_SIM_ENABLE_SANITIZERS=$([[ ${USE_ASAN} -eq 1 ]] && echo ON || echo OFF)
else
    cmake -S . -B "${BUILD_DIR}" \
        -DENGINE_SIM_ENABLE_SANITIZERS=$([[ ${USE_ASAN} -eq 1 ]] && echo ON || echo OFF)
fi
cmake --build "${BUILD_DIR}" -j2

SCRIPT_END_TS="$(date +%s)"
log_build "build end elapsed_s=$((SCRIPT_END_TS - SCRIPT_START_TS)) build_dir=${BUILD_DIR}"
