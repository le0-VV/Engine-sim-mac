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

finalize_macos_bundle() {
    local build_dir="$1"
    local icon_output_dir="$2"
    local source_app="${build_dir}/engine-simulator.app"
    local final_app="${build_dir}/Engine Simulator.app"
    local info_plist=""
    local resources_dir=""

    if [[ ! -d "${source_app}" ]]; then
        if [[ -d "${final_app}" ]]; then
            source_app="${final_app}"
        else
            log_build "mac bundle finalization skipped; no app bundle found in ${build_dir}"
            return 0
        fi
    fi

    if [[ "${source_app}" != "${final_app}" ]]; then
        rm -rf "${final_app}"
        mv "${source_app}" "${final_app}"
    fi

    info_plist="${final_app}/Contents/Info.plist"
    resources_dir="${final_app}/Contents/Resources"
    mkdir -p "${resources_dir}"

    /usr/bin/plutil -replace CFBundleDisplayName -string "Engine Simulator" "${info_plist}"
    /usr/bin/plutil -replace CFBundleName -string "Engine Simulator" "${info_plist}"
    /usr/bin/plutil -replace CFBundleExecutable -string "engine-simulator" "${info_plist}"
    /usr/bin/plutil -replace CFBundleIdentifier -string "wang.peiyu.EngineSimulator" "${info_plist}"

    if [[ -f "${icon_output_dir}/Assets.car" ]]; then
        /usr/bin/plutil -replace CFBundleIconName -string "AppIcon" "${info_plist}"
    fi

    rm -rf "${resources_dir}/assets" "${resources_dir}/delta-basic" "${resources_dir}/es"
    cp -R "assets" "${resources_dir}/assets"
    cp -R "dependencies/submodules/delta-studio/engines/basic" "${resources_dir}/delta-basic"
    cp -R "es" "${resources_dir}/es"

    if [[ -f "${icon_output_dir}/AppIcon.icns" ]]; then
        cp -f "${icon_output_dir}/AppIcon.icns" "${resources_dir}/AppIcon.icns"
    fi
    if [[ -f "${icon_output_dir}/Assets.car" ]]; then
        cp -f "${icon_output_dir}/Assets.car" "${resources_dir}/Assets.car"
    fi

    /usr/bin/codesign --force --deep --sign - "${final_app}"
    log_build "mac bundle finalization complete app=${final_app}"
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
    ICON_DEFAULT="art/appicon Exports/appicon-macOS-Default-1024x1024@1x.png"
    ICON_DARK="art/appicon Exports/appicon-macOS-Dark-1024x1024@1x.png"
    ICON_OUTPUT_DIR="${BUILD_DIR}/generated/icon_assets"

    if [[ -f "${ICON_DEFAULT}" ]]; then
        local_start_ts="$(date +%s)"
        log_build "icon asset compile start default=${ICON_DEFAULT} dark=${ICON_DARK} out_dir=${ICON_OUTPUT_DIR}"
        ./tools/compile_macos_icon_assets.sh \
            --default "${ICON_DEFAULT}" \
            --dark "${ICON_DARK}" \
            --out-dir "${ICON_OUTPUT_DIR}"
        local_end_ts="$(date +%s)"
        log_build "icon asset compile end elapsed_s=$((local_end_ts - local_start_ts))"
    else
        log_build "icon asset compile skipped default icon missing path=${ICON_DEFAULT}"
    fi
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    cmake -S . -B "${BUILD_DIR}" -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
        -DENGINE_SIM_ENABLE_SANITIZERS=$([[ ${USE_ASAN} -eq 1 ]] && echo ON || echo OFF)
else
    cmake -S . -B "${BUILD_DIR}" \
        -DENGINE_SIM_ENABLE_SANITIZERS=$([[ ${USE_ASAN} -eq 1 ]] && echo ON || echo OFF)
fi
cmake --build "${BUILD_DIR}" -j2

if [[ "$(uname -s)" == "Darwin" ]]; then
    finalize_macos_bundle "${BUILD_DIR}" "${ICON_OUTPUT_DIR}"
fi

SCRIPT_END_TS="$(date +%s)"
log_build "build end elapsed_s=$((SCRIPT_END_TS - SCRIPT_START_TS)) build_dir=${BUILD_DIR}"
