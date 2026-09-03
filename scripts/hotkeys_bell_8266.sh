#!/usr/bin/env bash
# Build / flash the doorbell transmitter for ESP8266 (Arduino / PlatformIO).
#
# Usage:
#   ./scripts/hotkeys_bell_8266.sh build
#   ./scripts/hotkeys_bell_8266.sh flash
#   ./scripts/hotkeys_bell_8266.sh build-flash
#   ./scripts/hotkeys_bell_8266.sh monitor
#
# Env:
#   PORT   serial port (optional)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-build}"
PORT="${PORT:-}"
PROJECT="$ROOT/doorbell_tx_8266"

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO CLI (pio) not found." >&2
  echo "Install: pipx install platformio or brew install platformio" >&2
  exit 1
fi

pio_cmd() {
  local extra=()
  if [[ -n "$PORT" ]]; then
    extra+=(--upload-port "$PORT" --monitor-port "$PORT")
  fi
  pio "${extra[@]}" "$@"
}

cd "$PROJECT"

case "$ACTION" in
  build)
    pio_cmd run
    ;;
  flash|upload)
    if [[ -n "$PORT" ]]; then
      pio_cmd run -t upload --upload-port "$PORT"
    else
      pio_cmd run -t upload
    fi
    ;;
  build-flash|flash-build)
    if [[ -n "$PORT" ]]; then
      pio_cmd run -t upload --upload-port "$PORT"
    else
      pio_cmd run -t upload
    fi
    ;;
  monitor)
    if [[ -n "$PORT" ]]; then
      pio device monitor --port "$PORT" -b 115200
    else
      pio device monitor -b 115200
    fi
    ;;
  *)
    echo "Unknown action: $ACTION (build|flash|build-flash|monitor)" >&2
    exit 1
    ;;
esac

echo "Done: product=doorbell-tx target=esp8266 action=$ACTION"

