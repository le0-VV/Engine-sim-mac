#!/usr/bin/env bash
set -euo pipefail

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

if [[ ${RUN_METAL_SHADERS} -eq 1 ]]; then
    ./tools/translate_shaders_to_msl.sh --build-metallib
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
