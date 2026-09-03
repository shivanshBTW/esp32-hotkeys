# AI handover — esp32-hotkeys

This firmware is **Hotkeys**, not LumosOS. Do not reintroduce the LED plugin/renderer/DDP/HyperHDR stack.

A later chat owns keypad scanning and actions. Doorbell TX/RX must stay wire-compatible with LumosOS.

## Repo map

| Path | What |
| --- | --- |
| `main/main.cpp` | Hotkeys receiver: REST + doorbell RX wiring |
| `components/hotkeys_webui` | Home + `/doorbell` pages (LumosOS UI minus LED/plugin/calibration) |
| `main/hotkeys_service.hpp` | **Stub** — implement keypad here (or split a `components/hotkeys_*` later) |
| `components/lumos_doorbell` | ESP-NOW TX + RX + packet/MAC (`lumos::` names kept on purpose) |
| `components/lumos_wifi` | STA/AP, reconnect, static IP, captive DNS, hostname, mDNS `_hotkeys._tcp` |
| `components/lumos_ota` | `POST /api/v1/ota` |
| `components/lumos_preferences` | NVS: wifi, hostname, doorbell, leftover LED fields unused by this app |
| `components/lumos_core` | logger, result, `board_pins.hpp`, app name `Hotkeys` / `0.1.0` |
| `doorbell_tx/` | Classic ESP32 emitter (ESP-IDF) |
| `doorbell_tx_8266/` | ESP8266 emitter (PlatformIO) |
| `scripts/hotkeys_idf.sh` | `esp32` / `esp32s3` / `doorbell-tx` / `doorbell-tx-8266` |

Sibling repo: `../esp32-ambilight-firmware` keeps **receiver only** and forwards TX builds here.

## ESP-NOW contract (do not change without Lumos)

Defined in `components/lumos_doorbell/include/lumos/doorbell/doorbell_packet.hpp`:

- Magic `0x4C444242` (`LDBB`), version `1`
- Packed sizes: `DoorbellPacket` 12 bytes, `DoorbellPairHello` 35 bytes
- Event types: PRESS=1, PAIR_HELLO=2, PAIR_CLAIM=3
- Roles: TX=1, RX=2

Pairing is by **MAC**, not hostname. TX default AP/hostname can stay `LumosOS-Bell`.

## Wi-Fi / OTA / config invariants

Keep these working when you add keypad:

- AP fallback `Hotkeys-Setup`, STA reconnect + retry timer, static IP fields
- Config schema `hotkeys.config.v1` (`GET/POST /api/v1/config`)
- OTA via `lumos_ota` (need OTA partitions in `partitions.csv`)
- Doorbell REST shapes same as Lumos (`/api/v1/doorbell*`) so tools stay interchangeable

mDNS service is `_hotkeys._tcp` with TXT `product=hotkeys`. Do not advertise `_lumosos` / LED chipset TXT from this firmware.

## Where to add keypad

1. Replace `HotkeysService` stub: scan matrix, debounce, dispatch.
2. Persist mapping in preferences (`plugin_params("hotkeys")` is an opaque string map already, or add a dedicated struct).
3. Expose `GET /api/v1/status` → `hotkeys.enabled` / bindings.
4. Extend `hotkeys.config.v1` blob (do not break wifi/doorbell keys).
5. Home UI `/` already has Wi-Fi / OTA / config / doorbell. Add keypad controls in the **Hotkeys** section (do not replace the rest of the page).

## Do not

- Change doorbell packet bytes without coordinating LumosOS RX
- Copy `lumos_plugin`, renderer, DDP, or LED driver into this repo
- Point TX include paths back at `esp32-ambilight-firmware`

## Builds

```bash
./scripts/hotkeys_idf.sh esp32s3 build
./scripts/hotkeys_idf.sh doorbell-tx build
./scripts/hotkeys_idf.sh doorbell-tx-8266 build
```
