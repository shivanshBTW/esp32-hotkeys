#pragma once

#include "lumos/core/color.hpp"
#include "lumos/core/types.hpp"

#include <cstdint>

namespace lumos {

// Espressif led_strip_set_pixel(R,G,B) always writes wire bytes as [G,R,B].
// Given logical RGB + desired strip order, compute the set_pixel() arguments
// so the physical diodes match the logical color.
inline constexpr void logical_to_led_strip_args(Rgb c, ColorOrder order, std::uint8_t& red_arg,
                                                std::uint8_t& green_arg, std::uint8_t& blue_arg) {
    switch (order) {
    case ColorOrder::Grb: // wire [G,R,B] ← set_pixel(R,G,B)
        red_arg = c.r;
        green_arg = c.g;
        blue_arg = c.b;
        break;
    case ColorOrder::Rgb: // wire [R,G,B] ← set_pixel(G,R,B)
        red_arg = c.g;
        green_arg = c.r;
        blue_arg = c.b;
        break;
    case ColorOrder::Brg: // wire [B,R,G] ← set_pixel(R,B,G)
        red_arg = c.r;
        green_arg = c.b;
        blue_arg = c.g;
        break;
    case ColorOrder::Rbg: // wire [R,B,G] ← set_pixel(B,R,G)
        red_arg = c.b;
        green_arg = c.r;
        blue_arg = c.g;
        break;
    case ColorOrder::Gbr: // wire [G,B,R] ← set_pixel(B,G,R)
        red_arg = c.b;
        green_arg = c.g;
        blue_arg = c.r;
        break;
    case ColorOrder::Bgr: // wire [B,G,R] ← set_pixel(G,B,R)
        red_arg = c.g;
        green_arg = c.b;
        blue_arg = c.r;
        break;
    }
}

// Host-test helper: first wire byte for a logical red under the given order.
inline constexpr Rgb to_wire_order(Rgb c, ColorOrder order) {
    std::uint8_t ra = 0;
    std::uint8_t ga = 0;
    std::uint8_t ba = 0;
    logical_to_led_strip_args(c, order, ra, ga, ba);
    // set_pixel packs [ga, ra, ba] onto the wire.
    return {ga, ra, ba};
}

} // namespace lumos
