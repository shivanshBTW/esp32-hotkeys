#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

namespace lumos {

// Target-specific defaults. One source tree; firmware values follow CONFIG_IDF_TARGET_*.
// Host unit tests (no sdkconfig) fall back to classic ESP32 rules.

#if defined(ARDUINO_ARCH_ESP8266) || defined(ESP8266)
inline constexpr int kDefaultLedGpio = 4;
inline constexpr int kDefaultRelayGpio = 5;
inline constexpr int kDefaultOptoGpio = 4;
inline constexpr int kMaxGpioNum = 16;
inline constexpr const char* kIdfTargetName = "esp8266";
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
inline constexpr int kDefaultLedGpio = 16;
inline constexpr int kDefaultRelayGpio = 17;
inline constexpr int kDefaultOptoGpio = 4;
inline constexpr int kMaxGpioNum = 48;
inline constexpr const char* kIdfTargetName = "esp32s3";
#elif defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32
inline constexpr int kDefaultLedGpio = 16;
inline constexpr int kDefaultRelayGpio = 17;
inline constexpr int kDefaultOptoGpio = 4;
inline constexpr int kMaxGpioNum = 39;
inline constexpr const char* kIdfTargetName = "esp32";
#else
// Desktop / host_tests
inline constexpr int kDefaultLedGpio = 16;
inline constexpr int kDefaultRelayGpio = 17;
inline constexpr int kDefaultOptoGpio = 4;
inline constexpr int kMaxGpioNum = 39;
inline constexpr const char* kIdfTargetName = "host";
#endif

/** True if `pin` is a reasonable digital output for LED data or a relay coil driver. */
inline bool is_safe_output_gpio(int pin) {
    if (pin < 0 || pin > kMaxGpioNum) {
        return false;
    }
#if defined(ARDUINO_ARCH_ESP8266) || defined(ESP8266)
    return pin == 4 || pin == 5 || pin == 12 || pin == 13 || pin == 14;
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    if (pin == 0 || pin == 3 || pin == 45 || pin == 46) {
        return false;
    }
    // USB-Serial/JTAG on most S3 modules.
    if (pin == 19 || pin == 20) {
        return false;
    }
#else
    // Classic ESP32 (+ host fallback): flash SPI, strapping, input-only.
    if (pin >= 6 && pin <= 11) {
        return false;
    }
    if (pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15) {
        return false;
    }
    // JTAG MTCK/MTDO — pad config on these can hang the interrupt WDT.
    if (pin == 13 || pin == 14) {
        return false;
    }
    if (pin >= 34) {
        return false;
    }
#endif
    return true;
}

/** Optocoupler / button input. Allows ESP32 input-only pins 34–39. */
inline bool is_safe_input_gpio(int pin) {
    if (pin < 0 || pin > kMaxGpioNum) {
        return false;
    }
#if defined(ARDUINO_ARCH_ESP8266) || defined(ESP8266)
    // Interrupt-capable, not flash/strapping. GPIO16 has no IRQ.
    return pin == 4 || pin == 5 || pin == 12 || pin == 13 || pin == 14;
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    if (pin == 0 || pin == 3 || pin == 45 || pin == 46) {
        return false;
    }
    if (pin == 19 || pin == 20) {
        return false;
    }
    return true;
#else
    if (pin >= 6 && pin <= 11) {
        return false;
    }
    if (pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15) {
        return false;
    }
    return true;
#endif
}

} // namespace lumos
