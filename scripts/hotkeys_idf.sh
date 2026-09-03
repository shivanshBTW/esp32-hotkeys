#!/usr/bin/env bash
# Build / flash the Hotkeys receiver (ESP32 / ESP32-S3) and the doorbell transmitters.
#
# Usage:
#   ./scripts/hotkeys_idf.sh esp32 build
#   ./scripts/hotkeys_idf.sh esp32 flash
#   ./scripts/hotkeys_idf.sh esp32s3 build
#   ./scripts/hotkeys_idf.sh esp32s3 flash
#
# Doorbell TX:
#   ./scripts/hotkeys_idf.sh doorbell-tx build
#   ./scripts/hotkeys_idf.sh doorbell-tx flash
#   ./scripts/hotkeys_idf.sh doorbell-tx-8266 build
#   ./scripts/hotkeys_idf.sh doorbell-tx-8266 flash

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ARG1="${1:-}"
ARG2="${2:-}"
PORT="${PORT:-}"
IDF_EXPORT="${IDF:-$HOME/esp/esp-idf/export.sh}"

usage() {
  sed -n '2,25p' "$0" | sed 's/^# \?//'
  exit 1
}

[[ -n "$ARG1" ]] || usage

if [[ "$ARG1" == "doorbell-tx-8266" ]]; then
  ACTION="${ARG2:-build}"
  exec bash "$ROOT/scripts/hotkeys_bell_8266.sh" "$ACTION"
fi

if [[ ! -f "$IDF_EXPORT" ]]; then
  echo "ESP-IDF export not found: $IDF_EXPORT" >&2
  echo "Set IDF=/path/to/esp-idf/export.sh" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "$IDF_EXPORT" >/dev/null

PRODUCT="hotkeys"
TARGET=""
ACTION="${ARG2:-build}"

case "$ARG1" in
  esp32|esp32s3)
    TARGET="$ARG1"
    ;;
  doorbell-tx)
    PRODUCT="doorbell-tx"
    TARGET="esp32"
    ;;
  *)
    echo "Unsupported target: $ARG1 (use esp32, esp32s3, doorbell-tx, doorbell-tx-8266)" >&2
    exit 1
    ;;
esac

if [[ "$PRODUCT" == "doorbell-tx" ]]; then
  BUILD_DIR="$ROOT/build-doorbell-tx"
  SDKCONFIG="$ROOT/sdkconfig.doorbell-tx"
  PROJECT_DIR="$ROOT/doorbell_tx"
else
  BUILD_DIR="$ROOT/build-$TARGET"
  SDKCONFIG="$ROOT/sdkconfig.${TARGET}"
  PROJECT_DIR="$ROOT"
fi

idf_cmd() {
  local extra=()
  extra+=(-C "$PROJECT_DIR" -B "$BUILD_DIR" -D "SDKCONFIG=$SDKCONFIG")
  if [[ -n "$PORT" ]]; then
    extra+=(-p "$PORT")
  fi
  idf.py "${extra[@]}" "$@"
}

need_set_target=0
if [[ ! -f "$SDKCONFIG" ]]; then
  need_set_target=1
elif ! grep -q "CONFIG_IDF_TARGET=\"${TARGET}\"" "$SDKCONFIG" 2>/dev/null; then
  need_set_target=1
fi

if [[ "$need_set_target" -eq 1 ]]; then
  echo "Configuring $PRODUCT $TARGET (SDKCONFIG=$SDKCONFIG, build=$BUILD_DIR)…"
  idf_cmd set-target "$TARGET"
fi

case "$ACTION" in
  build)
    idf_cmd build
    ;;
  flash)
    idf_cmd flash
    ;;
  build-flash|flash-build)
    idf_cmd build
    idf_cmd flash
    ;;
  monitor)
    idf_cmd monitor
    ;;
  menuconfig)
    idf_cmd menuconfig
    ;;
  *)
    echo "Unknown action: $ACTION (build|flash|build-flash|monitor|menuconfig)" >&2
    exit 1
    ;;
esac

echo "Done: product=$PRODUCT target=$TARGET action=$ACTION sdkconfig=$SDKCONFIG build=$BUILD_DIR"

