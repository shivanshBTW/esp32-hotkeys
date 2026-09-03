#include "lumos/core/color.hpp"

#include <cmath>

namespace lumos {

Rgb hsv_to_rgb(float h, float s, float v) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;

    float r = 0, g = 0, b = 0;
    if (h < 60.0f) {
        r = c;
        g = x;
    } else if (h < 120.0f) {
        r = x;
        g = c;
    } else if (h < 180.0f) {
        g = c;
        b = x;
    } else if (h < 240.0f) {
        g = x;
        b = c;
    } else if (h < 300.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }

    return Rgb{
        static_cast<std::uint8_t>((r + m) * 255.0f + 0.5f),
        static_cast<std::uint8_t>((g + m) * 255.0f + 0.5f),
        static_cast<std::uint8_t>((b + m) * 255.0f + 0.5f),
    };
}

Rgb scale_rgb(Rgb c, float factor) {
    factor = std::clamp(factor, 0.0f, 1.0f);
    return Rgb{
        static_cast<std::uint8_t>(c.r * factor + 0.5f),
        static_cast<std::uint8_t>(c.g * factor + 0.5f),
        static_cast<std::uint8_t>(c.b * factor + 0.5f),
    };
}

Rgb mix_rgb(Rgb a, Rgb b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return Rgb{
        static_cast<std::uint8_t>(a.r * inv + b.r * t + 0.5f),
        static_cast<std::uint8_t>(a.g * inv + b.g * t + 0.5f),
        static_cast<std::uint8_t>(a.b * inv + b.b * t + 0.5f),
    };
}

Rgbw rgb_to_rgbw(Rgb c, WhiteAlgorithm algo) {
    (void)algo; // only ExtractMin for v0.2
    const std::uint8_t w = std::min({c.r, c.g, c.b});
    return Rgbw{
        static_cast<std::uint8_t>(c.r - w),
        static_cast<std::uint8_t>(c.g - w),
        static_cast<std::uint8_t>(c.b - w),
        w,
    };
}

Rgb scale_rgbw_to_rgb_preview(Rgbw c) {
    // Approximate for UI: fold white back into RGB.
    return Rgb{
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(c.r) + c.w)),
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(c.g) + c.w)),
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(c.b) + c.w)),
    };
}

Rgbw scale_rgbw(Rgbw c, float factor) {
    factor = std::clamp(factor, 0.0f, 1.0f);
    return Rgbw{
        static_cast<std::uint8_t>(c.r * factor + 0.5f),
        static_cast<std::uint8_t>(c.g * factor + 0.5f),
        static_cast<std::uint8_t>(c.b * factor + 0.5f),
        static_cast<std::uint8_t>(c.w * factor + 0.5f),
    };
}

} // namespace lumos
