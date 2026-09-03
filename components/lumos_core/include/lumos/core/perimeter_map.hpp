#pragma once

#include "lumos/core/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace lumos {

// Where wire index 0 sits on the TV, and which way the strip runs.
enum class PerimeterStart : std::uint8_t {
    TopLeft = 0,
    TopRight,
    BottomRight,
    BottomLeft,
};

enum class PerimeterDirection : std::uint8_t {
    Clockwise = 0,
    CounterClockwise,
};

enum class TvSide : std::uint8_t {
    Top = 0,
    Right,
    Bottom,
    Left,
};

struct PerimeterMaps {
    // logical (CW from top-left) → wire index
    std::vector<std::uint16_t> logical_to_physical;
    // wire index → logical
    std::vector<std::uint16_t> physical_to_logical;
    bool identity{true};
};

inline constexpr std::array<TvSide, 4> kSidesCwFromTl = {TvSide::Top, TvSide::Right, TvSide::Bottom,
                                                         TvSide::Left};

// First side the wire travels after leaving the start corner.
inline TvSide wire_first_side(PerimeterStart start, PerimeterDirection dir) {
    if (dir == PerimeterDirection::Clockwise) {
        switch (start) {
        case PerimeterStart::TopLeft:
            return TvSide::Top;
        case PerimeterStart::TopRight:
            return TvSide::Right;
        case PerimeterStart::BottomRight:
            return TvSide::Bottom;
        case PerimeterStart::BottomLeft:
            return TvSide::Left;
        }
    }
    switch (start) {
    case PerimeterStart::TopLeft:
        return TvSide::Left;
    case PerimeterStart::TopRight:
        return TvSide::Top;
    case PerimeterStart::BottomRight:
        return TvSide::Right;
    case PerimeterStart::BottomLeft:
        return TvSide::Bottom;
    }
    return TvSide::Top;
}

inline std::array<TvSide, 4> wire_side_order(PerimeterStart start, PerimeterDirection dir) {
    const TvSide first = wire_first_side(start, dir);
    std::array<TvSide, 4> out{};
    const int step = (dir == PerimeterDirection::Clockwise) ? 1 : -1;
    int idx = static_cast<int>(first);
    for (int i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<TvSide>((idx + 4) % 4);
        idx += step;
    }
    return out;
}

inline std::uint16_t side_count(TvSide side, std::uint16_t top, std::uint16_t right,
                                std::uint16_t bottom, std::uint16_t left) {
    switch (side) {
    case TvSide::Top:
        return top;
    case TvSide::Right:
        return right;
    case TvSide::Bottom:
        return bottom;
    case TvSide::Left:
        return left;
    }
    return 0;
}

inline std::uint16_t logical_side_base(TvSide side, std::uint16_t top, std::uint16_t right,
                                       std::uint16_t bottom) {
    switch (side) {
    case TvSide::Top:
        return 0;
    case TvSide::Right:
        return top;
    case TvSide::Bottom:
        return static_cast<std::uint16_t>(top + right);
    case TvSide::Left:
        return static_cast<std::uint16_t>(top + right + bottom);
    }
    return 0;
}

// Logical side storage is CW from TL: Top L→R, Right T→B, Bottom R→L, Left B→T.
// When the wire traverses a side opposite to that, reverse within the side.
inline bool side_reversed_on_wire(TvSide side, PerimeterDirection dir) {
    // CW wire matches logical within-side direction; CCW reverses each side.
    return dir == PerimeterDirection::CounterClockwise;
}

inline PerimeterMaps build_perimeter_maps(LedIndex led_count, std::uint16_t top, std::uint16_t right,
                                          std::uint16_t bottom, std::uint16_t left,
                                          PerimeterStart start, PerimeterDirection dir) {
    PerimeterMaps maps;
    maps.logical_to_physical.assign(led_count, 0);
    maps.physical_to_logical.assign(led_count, 0);
    const std::uint32_t sum =
        static_cast<std::uint32_t>(top) + right + bottom + left;
    if (led_count == 0 || sum != led_count) {
        for (LedIndex i = 0; i < led_count; ++i) {
            maps.logical_to_physical[i] = i;
            maps.physical_to_logical[i] = i;
        }
        maps.identity = true;
        return maps;
    }

    maps.identity = (start == PerimeterStart::TopLeft && dir == PerimeterDirection::Clockwise);
    if (maps.identity) {
        for (LedIndex i = 0; i < led_count; ++i) {
            maps.logical_to_physical[i] = i;
            maps.physical_to_logical[i] = i;
        }
        return maps;
    }

    const auto order = wire_side_order(start, dir);
    LedIndex wire = 0;
    for (TvSide side : order) {
        const std::uint16_t n = side_count(side, top, right, bottom, left);
        const std::uint16_t base = logical_side_base(side, top, right, bottom);
        const bool rev = side_reversed_on_wire(side, dir);
        for (std::uint16_t i = 0; i < n; ++i) {
            const std::uint16_t logical =
                static_cast<std::uint16_t>(base + (rev ? (n - 1 - i) : i));
            if (wire < led_count && logical < led_count) {
                maps.logical_to_physical[logical] = wire;
                maps.physical_to_logical[wire] = logical;
            }
            ++wire;
        }
    }
    return maps;
}

// Identify-color id: 0=red, 1=green, 2=blue, 3=amber (logical Top/Right/Bottom/Left).
// color_on_tv_side[0..3] = color id seen on Top/Right/Bottom/Left while lighting with
// identity map (chunk order Top,Right,Bottom,Left). Returns matching wire orientation.
inline std::optional<std::pair<PerimeterStart, PerimeterDirection>> solve_orientation_from_colors(
    const std::array<std::uint8_t, 4>& color_on_tv_side) {
    std::array<TvSide, 4> wire_order{};
    bool used[4] = {false, false, false, false};
    for (int tv = 0; tv < 4; ++tv) {
        const int color = color_on_tv_side[static_cast<std::size_t>(tv)];
        if (color < 0 || color > 3 || used[color]) {
            return std::nullopt;
        }
        used[color] = true;
        wire_order[static_cast<std::size_t>(color)] = static_cast<TvSide>(tv);
    }
    for (int s = 0; s < 4; ++s) {
        for (int d = 0; d < 2; ++d) {
            const auto start = static_cast<PerimeterStart>(s);
            const auto dir = static_cast<PerimeterDirection>(d);
            if (wire_side_order(start, dir) == wire_order) {
                return std::make_pair(start, dir);
            }
        }
    }
    return std::nullopt;
}

} // namespace lumos
