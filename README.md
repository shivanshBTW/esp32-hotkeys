# Hotkeys

ESP32 firmware for keypad hotkeys (stub) plus a doorbell receiver, with the doorbell emitters that used to live next to LumosOS.

Wi-Fi, OTA, and config backup/restore follow the same patterns as LumosOS. This is **not** LumosOS: there is no LED plugin stack.

## Products

| Product | Chip | Role |
| --- | --- | --- |
| **hotkeys** | ESP32 / ESP32-S3 | Main binary: Wi-Fi, HTTP OTA, config, doorbell **receiver**, keypad stub |
| **doorbell-tx** | Classic ESP32 (ESP-IDF) | Doorbell emitter (optocoupler + ESP-NOW) |
| **doorbell-tx-8266** | ESP8266 (PlatformIO) | Same emitter, Arduino/PIO target |

ESP-NOW packets stay byte-identical to LumosOS. Pair a bell with either a LumosOS LED box or this firmware's receiver.

## Build / flash

Requires [ESP-IDF v5.3+](https://docs.espressif.com/projects/esp-idf/). ESP8266 TX also needs [PlatformIO](https://platformio.org/).

```bash
# Hotkeys receiver
./scripts/hotkeys_idf.sh esp32 build
./scripts/hotkeys_idf.sh esp32s3 build
PORT=/dev/cu.usbserial-0001 ./scripts/hotkeys_idf.sh esp32s3 flash

# Doorbell transmitter
./scripts/hotkeys_idf.sh doorbell-tx build              # classic ESP32
./scripts/hotkeys_idf.sh doorbell-tx-8266 build         # ESP8266
PORT=/dev/cu.usbserial-0001 ./scripts/hotkeys_idf.sh doorbell-tx flash
```

`lumos_idf.sh doorbell-tx` in the sibling `esp32-ambilight-firmware` repo forwards here.

## First boot (receiver)

1. AP `Hotkeys-Setup` if no Wi-Fi is stored (open, typically `http://192.168.4.1/`).
2. Configure STA via `POST /api/v1/wifi` or restore `GET/POST /api/v1/config` (`schema`: `hotkeys.config.v1`).
3. mDNS: `_hotkeys._tcp`, hostname default `Hotkeys`.
4. Doorbell UI: `/doorbell`. Pair with a TX the same way as LumosOS `/doorbell` (Start pairing on RX, Find nearby on `LumosOS-Bell`).
5. HTTP OTA: `POST /api/v1/ota`.

TX first-boot AP remains `LumosOS-Bell` (pairing is by MAC, not hostname).

## API (receiver)

- `GET /api/v1/status` — wifi + doorbell + `hotkeys: { enabled: false }`
- `GET/POST /api/v1/wifi`, `GET /api/v1/wifi/scan`, `POST /api/v1/wifi/retry`
- `GET/POST /api/v1/config` — wifi + doorbell + hotkeys blob
- `GET/POST /api/v1/doorbell*`, `POST /api/v1/doorbell/test`, pairing routes
- `POST /api/v1/ota`

## Keypad

Not implemented. See `HANDOVER.md` and `main/hotkeys_service.hpp`.

## License

Same GPL-3.0 + additional terms as LumosOS (`LICENSE`, `NOTICE`). Shared doorbell/Wi-Fi/OTA code originated there.
