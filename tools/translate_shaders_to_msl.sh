#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHADER_ROOT="${ROOT_DIR}/dependencies/submodules/delta-studio/engines/basic/shaders"
OUT_DIR="${ROOT_DIR}/build/generated/msl"
KEEP_SPIRV=0
BUILD_METALLIB=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Translate Delta Studio basic engine shaders to Metal Shading Language (MSL).
Pipeline:
  GLSL/HLSL -> SPIR-V -> MSL via glslangValidator/dxc + spirv-cross

Limitations:
  - Output is an initial porting baseline, not guaranteed drop-in for this engine.
  - Resource bindings and semantics may require manual edits.
  - Generated MSL is not auto-integrated into runtime; this script only generates files.

Options:
  --out-dir <dir>       Output directory (default: ${OUT_DIR})
  --keep-spirv          Keep intermediate SPIR-V binaries
  --build-metallib      Compile generated .metal into .metallib (requires xcrun)
  --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --out-dir)
        OUT_DIR="$2"
        shift 2
        ;;
    --keep-spirv)
        KEEP_SPIRV=1
        shift
        ;;
    --build-metallib)
        BUILD_METALLIB=1
        shift
        ;;
    --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        usage
        exit 1
        ;;
    esac
done

require_tool() {
    local tool="$1"
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Missing required tool: ${tool}" >&2
        exit 1
    fi
}

require_tool glslangValidator
require_tool spirv-cross
require_tool dxc
if [[ ${BUILD_METALLIB} -eq 1 ]]; then
    require_tool xcrun
fi

mkdir -p "${OUT_DIR}"

GLSL_OUT_DIR="${OUT_DIR}/glsl"
HLSL_OUT_DIR="${OUT_DIR}/hlsl"
mkdir -p "${GLSL_OUT_DIR}" "${HLSL_OUT_DIR}"

translate_glsl() {
    local in_file="$1"
    local stage="$2"
    local stem="$3"
    local spirv="${GLSL_OUT_DIR}/${stem}.spv"
    local metal="${GLSL_OUT_DIR}/${stem}.metal"

    glslangValidator -V -S "${stage}" -o "${spirv}" "${in_file}"
    spirv-cross "${spirv}" --msl --output "${metal}"

    if [[ ${KEEP_SPIRV} -eq 0 ]]; then
        rm -f "${spirv}"
    fi
}

translate_hlsl_entry() {
    local in_file="$1"
    local entry="$2"
    local profile="$3"
    local stage="$4"
    local stem="$5"
    local spirv="${HLSL_OUT_DIR}/${stem}.spv"
    local metal="${HLSL_OUT_DIR}/${stem}.metal"

    dxc -spirv -T "${profile}" -E "${entry}" "${in_file}" -Fo "${spirv}"
    spirv-cross "${spirv}" --msl --entry "${entry}" --stage "${stage}" --output "${metal}"

    if [[ ${KEEP_SPIRV} -eq 0 ]]; then
        rm -f "${spirv}"
    fi
}

compile_metallib_tree() {
    local input_dir="$1"
    local air_dir="${input_dir}/air"
    local lib_dir="${input_dir}/metallib"
    mkdir -p "${air_dir}" "${lib_dir}"

    while IFS= read -r -d '' metal_file; do
        local base_name
        base_name="$(basename "${metal_file}" .metal)"
        local air_file="${air_dir}/${base_name}.air"
        local lib_file="${lib_dir}/${base_name}.metallib"
        xcrun metal -std=metal3.0 -c "${metal_file}" -o "${air_file}"
        xcrun metallib "${air_file}" -o "${lib_file}"
    done < <(find "${input_dir}" -maxdepth 1 -name "*.metal" -print0)
}

echo "Translating GLSL shaders..."
translate_glsl "${SHADER_ROOT}/glsl/delta_engine_shader.vert" vert "delta_engine_shader_vert"
translate_glsl "${SHADER_ROOT}/glsl/delta_engine_shader.frag" frag "delta_engine_shader_frag"
translate_glsl "${SHADER_ROOT}/glsl/delta_console_shader.vert" vert "delta_console_shader_vert"
translate_glsl "${SHADER_ROOT}/glsl/delta_console_shader.frag" frag "delta_console_shader_frag"
translate_glsl "${SHADER_ROOT}/glsl/delta_saq_shader.vert" vert "delta_saq_shader_vert"
translate_glsl "${SHADER_ROOT}/glsl/delta_saq_shader.frag" frag "delta_saq_shader_frag"

echo "Translating HLSL shaders..."
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_engine_shader.fx" "VS_STANDARD" "vs_6_0" "vert" "delta_engine_shader_vs_standard"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_engine_shader.fx" "VS_SKINNED" "vs_6_0" "vert" "delta_engine_shader_vs_skinned"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_engine_shader.fx" "PS" "ps_6_0" "frag" "delta_engine_shader_ps"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_console_shader.fx" "VS_CONSOLE" "vs_6_0" "vert" "delta_console_shader_vs"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_console_shader.fx" "PS_CONSOLE" "ps_6_0" "frag" "delta_console_shader_ps"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_saq_shader.fx" "VS_SAQ" "vs_6_0" "vert" "delta_saq_shader_vs"
translate_hlsl_entry "${SHADER_ROOT}/hlsl/delta_saq_shader.fx" "PS_SAQ" "ps_6_0" "frag" "delta_saq_shader_ps"

if [[ ${BUILD_METALLIB} -eq 1 ]]; then
    echo "Compiling generated MSL into metallib..."
    compile_metallib_tree "${GLSL_OUT_DIR}"
    compile_metallib_tree "${HLSL_OUT_DIR}"
fi

echo "Done. Output: ${OUT_DIR}"
