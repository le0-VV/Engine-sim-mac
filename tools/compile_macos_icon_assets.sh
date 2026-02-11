#!/usr/bin/env bash

set -euo pipefail

DEFAULT_ICON=""
DARK_ICON=""
OUT_DIR=""

usage() {
    cat <<EOF
Usage: $(basename "$0") --default <png> [--dark <png>] --out-dir <dir>

Compiles macOS app icon assets via Xcode actool and emits:
  - AppIcon.icns
  - Assets.car
  - partial.plist
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --default)
        DEFAULT_ICON="$2"
        shift 2
        ;;
    --dark)
        DARK_ICON="$2"
        shift 2
        ;;
    --out-dir)
        OUT_DIR="$2"
        shift 2
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

if [[ -z "${DEFAULT_ICON}" || -z "${OUT_DIR}" ]]; then
    echo "Missing required arguments." >&2
    usage
    exit 1
fi

if [[ ! -f "${DEFAULT_ICON}" ]]; then
    echo "Default icon not found: ${DEFAULT_ICON}" >&2
    exit 1
fi

if [[ -z "${DARK_ICON}" || ! -f "${DARK_ICON}" ]]; then
    DARK_ICON="${DEFAULT_ICON}"
fi

if ! command -v xcrun >/dev/null 2>&1; then
    echo "xcrun is required to compile icon assets." >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to generate iconset image sizes." >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

ASSETS_DIR="${TMP_DIR}/Assets.xcassets"
APPICONSET_DIR="${ASSETS_DIR}/AppIcon.appiconset"
mkdir -p "${APPICONSET_DIR}" "${OUT_DIR}"

cat > "${ASSETS_DIR}/Contents.json" <<'EOF'
{
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
EOF

python3 - "${DEFAULT_ICON}" "${DARK_ICON}" "${APPICONSET_DIR}" <<'PY'
import json
import sys
from pathlib import Path
from PIL import Image

default_icon = Path(sys.argv[1])
dark_icon = Path(sys.argv[2])
appiconset_dir = Path(sys.argv[3])

light = Image.open(default_icon).convert("RGBA")
dark = Image.open(dark_icon).convert("RGBA")

if light.size[0] != light.size[1]:
    raise SystemExit(f"Default icon must be square; got {light.size}")
if dark.size[0] != dark.size[1]:
    raise SystemExit(f"Dark icon must be square; got {dark.size}")

base_sizes = [16, 32, 128, 256, 512]
entries = []

for base in base_sizes:
    for scale in (1, 2):
        pixel_size = base * scale
        if pixel_size > 1024:
            continue

        suffix = "@2x" if scale == 2 else ""
        light_name = f"light_{base}x{base}{suffix}.png"
        dark_name = f"dark_{base}x{base}{suffix}.png"

        light.resize((pixel_size, pixel_size), Image.Resampling.LANCZOS).save(appiconset_dir / light_name)
        dark.resize((pixel_size, pixel_size), Image.Resampling.LANCZOS).save(appiconset_dir / dark_name)

        entries.append({
            "filename": light_name,
            "idiom": "mac",
            "scale": f"{scale}x",
            "size": f"{base}x{base}",
        })
        entries.append({
            "appearances": [
                {
                    "appearance": "luminosity",
                    "value": "dark",
                }
            ],
            "filename": dark_name,
            "idiom": "mac",
            "scale": f"{scale}x",
            "size": f"{base}x{base}",
        })

contents = {
    "images": entries,
    "info": {
        "author": "xcode",
        "version": 1,
    },
}

(appiconset_dir / "Contents.json").write_text(json.dumps(contents, indent=2) + "\n", encoding="utf-8")
PY

ACTOOL_LOG="${OUT_DIR}/actool.log"
xcrun actool "${ASSETS_DIR}" \
    --compile "${OUT_DIR}" \
    --platform macosx \
    --target-device mac \
    --minimum-deployment-target 13.0 \
    --app-icon AppIcon \
    --output-partial-info-plist "${OUT_DIR}/partial.plist" \
    > "${ACTOOL_LOG}" 2>&1

if [[ ! -f "${OUT_DIR}/AppIcon.icns" ]]; then
    echo "actool did not produce AppIcon.icns" >&2
    exit 1
fi

if [[ ! -f "${OUT_DIR}/Assets.car" ]]; then
    echo "actool did not produce Assets.car" >&2
    exit 1
fi
