#pragma once

#include <algorithm>
#include <cstdint>

namespace lumos {

struct Rgb {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};

    constexpr Rgb() = default;
    constexpr Rgb(std::uint8_t r_, std::uint8_t g_, std::uint8_t b_) : r(r_), g(g_), b(b_) {}

    static constexpr Rgb black() { return {0, 0, 0}; }
    static constexpr Rgb white() { return {255, 255, 255}; }

    constexpr bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    constexpr bool operator!=(const Rgb& o) const { return !(*this == o); }
};

struct Rgbw {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t w{0};

    constexpr Rgbw() = default;
    constexpr Rgbw(std::uint8_t r_, std::uint8_t g_, std::uint8_t b_, std::uint8_t w_)
        : r(r_), g(g_), b(b_), w(w_) {}

    static constexpr Rgbw black() { return {0, 0, 0, 0}; }

    constexpr bool operator==(const Rgbw& o) const {
        return r == o.r && g == o.g && b == o.b && w == o.w;
    }
    constexpr bool operator!=(const Rgbw& o) const { return !(*this == o); }
};

enum class WhiteAlgorithm : std::uint8_t {
    // W = min(R,G,B); subtract W from RGB channels.
    ExtractMin = 0,
};

Rgb hsv_to_rgb(float h, float s, float v);
Rgb scale_rgb(Rgb c, float factor);
Rgb mix_rgb(Rgb a, Rgb b, float t);
Rgbw rgb_to_rgbw(Rgb c, WhiteAlgorithm algo = WhiteAlgorithm::ExtractMin);
Rgb scale_rgbw_to_rgb_preview(Rgbw c); // drop W into RGB for preview approximations
Rgbw scale_rgbw(Rgbw c, float factor);

} // namespace lumos
