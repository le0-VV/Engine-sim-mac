#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BIN="${ROOT_DIR}/build/engine-sim-app.app/Contents/MacOS/engine-sim-app"
LOG_DIR="${ROOT_DIR}/build/logs"

usage() {
  cat <<'USAGE'
Usage:
  tools/backtrace.sh [--binary /path/to/binary] [--log /path/to/logfile] [--graphics] [--timeout SECONDS] [--] [args...]

Runs the engine-sim binary under LLDB, captures a backtrace on exit/crash,
and writes the LLDB session output to a log file.
With --graphics --timeout, the timer starts when the first render call is hit.

Examples:
  tools/backtrace.sh
  tools/backtrace.sh --graphics
  tools/backtrace.sh --graphics --timeout 5
  tools/backtrace.sh --binary /tmp/engine-sim-app --log /tmp/backtrace.log
  tools/backtrace.sh -- --some-arg value
USAGE
}

BIN_PATH="${DEFAULT_BIN}"
LOG_PATH=""
MODE="default"
RENDER_TIMEOUT=""
ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)
      BIN_PATH="$2"
      shift 2
      ;;
    --log)
      LOG_PATH="$2"
      shift 2
      ;;
    --graphics)
      MODE="graphics"
      shift
      ;;
    --timeout)
      RENDER_TIMEOUT="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      ARGS+=("$@")
      break
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ! -x "${BIN_PATH}" ]]; then
  echo "Binary not found or not executable: ${BIN_PATH}" >&2
  exit 1
fi

if ! command -v lldb >/dev/null 2>&1; then
  echo "lldb not found. Install Xcode Command Line Tools to use this script." >&2
  exit 1
fi

if [[ -z "${LOG_PATH}" ]]; then
  mkdir -p "${LOG_DIR}"
  TS="$(date +"%Y%m%d_%H%M%S")"
  LOG_PATH="${LOG_DIR}/backtrace_${TS}.log"
fi

if [[ -n "${RENDER_TIMEOUT}" && "${MODE}" != "graphics" ]]; then
  echo "--timeout is only supported with --graphics (timeout starts at first render)." >&2
  exit 1
fi

if [[ -n "${RENDER_TIMEOUT}" && ! "${RENDER_TIMEOUT}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "Invalid --timeout value: ${RENDER_TIMEOUT} (expected seconds, e.g. 2 or 2.5)" >&2
  exit 1
fi

LLDB_ARGS=(
  --batch
)

if [[ "${MODE}" == "graphics" ]]; then
  LLDB_ARGS+=(
    --one-line "breakpoint set --name EngineSimApplication::renderScene"
    --one-line "breakpoint set --name EngineSimApplication::render"
  )

  if [[ -n "${RENDER_TIMEOUT}" ]]; then
    LLDB_BP_CMD="breakpoint command add 1 -o \"breakpoint disable 1\" -o \"command script import threading, time, lldb; threading.Thread(target=lambda: (time.sleep(${RENDER_TIMEOUT}), lldb.debugger.HandleCommand('process interrupt')), daemon=True).start()\" -o \"continue\""
    LLDB_ARGS+=(
      --one-line "${LLDB_BP_CMD}"
    )
  fi
fi

LLDB_ARGS+=(
  --one-line "run"
  --one-line "bt all"
  --one-line "quit"
  -- "${BIN_PATH}" "${ARGS[@]}"
)

lldb "${LLDB_ARGS[@]}" > "${LOG_PATH}" 2>&1

echo "Backtrace log written to: ${LOG_PATH}"
