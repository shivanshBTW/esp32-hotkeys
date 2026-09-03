#pragma once

#include "lumos/core/board_pins.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lumos {

using LedIndex = std::uint16_t;
using Brightness = std::uint8_t; // 0-255
using Milliseconds = std::uint32_t;

enum class Chipset : std::uint8_t {
    Ws2815 = 0,
    Ws2812B,
    Ws2813,
    Sk6812Rgb,
    Sk6812Rgbw,
    Ws2815Rgbw,
};

inline constexpr bool chipset_is_rgbw(Chipset c) {
    return c == Chipset::Sk6812Rgbw || c == Chipset::Ws2815Rgbw;
}

enum class ColorOrder : std::uint8_t {
    Grb = 0,
    Rgb,
    Brg,
    Rbg,
    Gbr,
    Bgr,
};

// Default matches a common 16:9 perimeter: top/bottom 44, left/right 26.
inline constexpr LedIndex kDefaultLedCount = 140;
// kDefaultLedGpio / kDefaultRelayGpio live in board_pins.hpp (per IDF target).
inline constexpr Brightness kDefaultBrightness = 128;
inline constexpr float kDefaultGamma = 2.2f;
// Per-channel gain 0–255 (255 = unity). Green often needs ~140–180 on SK6812 RGBW.
inline constexpr std::uint8_t kDefaultChannelBalance = 255;
inline constexpr std::uint16_t kDefaultPowerLimitMa = 5000;
inline constexpr Milliseconds kDefaultHyperHdrTimeoutMs = 6500;

inline constexpr std::string_view kAppName = "Hotkeys";
inline constexpr std::string_view kAppVersion = "0.1.0";
inline constexpr std::string_view kApiVersion = "0.3";

} // namespace lumos
