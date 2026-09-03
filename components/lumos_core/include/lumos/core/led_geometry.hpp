#pragma once

#include "lumos/core/led_calibration.hpp"
#include "lumos/core/perimeter_map.hpp"
#include "lumos/core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lumos {

struct LedLayoutCounts {
    std::uint16_t top{0};
    std::uint16_t right{0};
    std::uint16_t bottom{0};
    std::uint16_t left{0};

    LedIndex total() const {
        return static_cast<LedIndex>(top + right + bottom + left);
    }
};

struct HyperHdrSummary {
    LedIndex leds{0};
    std::uint16_t top{0};
    std::uint16_t right{0};
    std::uint16_t bottom{0};
    std::uint16_t left{0};
};

struct LedGeometry {
    LedIndex physical{0};
    LedLayoutCounts layout{};
    EdgeIgnoreParams edge{};
    std::vector<std::uint16_t> ignored_physical{}; // middle disables (wire indices)
    PerimeterStart start{PerimeterStart::TopLeft};
    PerimeterDirection direction{PerimeterDirection::Clockwise};

    // active (logical CW-from-TL) → physical wire index
    std::vector<std::uint16_t> active_to_physical;
    // physical → active index, or 0xFFFF if not active
    std::vector<std::uint16_t> physical_to_active;
    std::vector<std::uint8_t> physical_ignore_mask;

    LedIndex active_count() const { return layout.total(); }

    HyperHdrSummary hyperhdr_summary() const {
        return HyperHdrSummary{
            .leds = active_count(),
            .top = layout.top,
            .right = layout.right,
            .bottom = layout.bottom,
            .left = layout.left,
        };
    }

    bool is_physical_ignored(LedIndex phys) const {
        return ignore_mask_test(physical_ignore_mask, phys);
    }
};

inline LedIndex count_ignored_in_range(const std::vector<std::uint16_t>& ignored, LedIndex begin,
                                       LedIndex end_inclusive) {
    LedIndex n = 0;
    for (std::uint16_t i : ignored) {
        if (i >= begin && i <= end_inclusive) {
            ++n;
        }
    }
    return n;
}

inline LedIndex edge_active_count(LedIndex start, LedIndex end_inclusive,
                                  const std::vector<std::uint16_t>& ignored_physical) {
    if (end_inclusive < start) {
        return 0;
    }
    const LedIndex span =
        static_cast<LedIndex>(end_inclusive - start + 1);
    const LedIndex ign = count_ignored_in_range(ignored_physical, start, end_inclusive);
    return span > ign ? static_cast<LedIndex>(span - ign) : 0;
}

// Rebuild maps. Layout counts are ACTIVE LEDs per TV side (HyperHDR).
// Physical wire: [skip_start][perimeter span][skip_end], with middle ignores inside the span.
inline LedGeometry build_led_geometry(LedIndex physical, LedLayoutCounts layout,
                                      const EdgeIgnoreParams& edge,
                                      const std::vector<std::uint16_t>& ignored_physical,
                                      PerimeterStart start, PerimeterDirection direction) {
    LedGeometry g;
    g.physical = physical;
    g.layout = layout;
    g.edge = edge;
    g.ignored_physical = ignored_physical;
    g.start = start;
    g.direction = direction;
    sort_unique_indices(g.ignored_physical, physical);

    g.physical_ignore_mask.assign((physical + 7) / 8, 0);
    g.physical_to_active.assign(physical, 0xFFFF);
    g.active_to_physical.clear();

    if (physical == 0) {
        return g;
    }

    const LedIndex skip0 = std::min(edge.skip_start, physical);
    const LedIndex skip1 = std::min(edge.skip_end, static_cast<std::uint16_t>(physical - skip0));
    for (LedIndex i = 0; i < skip0; ++i) {
        g.physical_ignore_mask[i / 8] =
            static_cast<std::uint8_t>(g.physical_ignore_mask[i / 8] | (1u << (i % 8)));
    }
    for (LedIndex k = 0; k < skip1; ++k) {
        const LedIndex i = static_cast<LedIndex>(physical - 1 - k);
        g.physical_ignore_mask[i / 8] =
            static_cast<std::uint8_t>(g.physical_ignore_mask[i / 8] | (1u << (i % 8)));
    }
    for (std::uint16_t i : g.ignored_physical) {
        if (i < physical) {
            g.physical_ignore_mask[i / 8] =
                static_cast<std::uint8_t>(g.physical_ignore_mask[i / 8] | (1u << (i % 8)));
        }
    }

    // Span between skips, excluding ignored — ordered in wire travel direction.
    std::vector<std::uint16_t> span_active_wire;
    const LedIndex span_begin = skip0;
    const LedIndex span_end = static_cast<LedIndex>(physical - skip1); // exclusive
    for (LedIndex w = span_begin; w < span_end; ++w) {
        if (!ignore_mask_test(g.physical_ignore_mask, w)) {
            span_active_wire.push_back(static_cast<std::uint16_t>(w));
        }
    }

    const LedIndex active = layout.total();
    if (active == 0 || span_active_wire.size() < active) {
        // Fallback: identity-ish map for whatever we can
        g.active_to_physical = span_active_wire;
        for (LedIndex a = 0; a < g.active_to_physical.size(); ++a) {
            g.physical_to_active[g.active_to_physical[a]] = a;
        }
    } else {
        // Map logical active ring (CW from TL by layout counts) onto wire span via perimeter order.
        // First build logical→span-offset using the same rules as build_perimeter_maps, but the
        // "physical" domain here is the compacted span_active_wire list.
        const auto peri = build_perimeter_maps(active, layout.top, layout.right, layout.bottom,
                                               layout.left, start, direction);

        g.active_to_physical.assign(active, 0);
        for (LedIndex logical = 0; logical < active; ++logical) {
            const auto span_i = peri.logical_to_physical[logical];
            if (span_i < span_active_wire.size()) {
                const auto wire = span_active_wire[span_i];
                g.active_to_physical[logical] = wire;
                g.physical_to_active[wire] = logical;
            }
        }
    }

    // Any wire LED not mapped to HyperHDR active stays forced-off at present().
    // Covers leftover span LEDs when physical > skips + active (not yet measured).
    for (LedIndex w = 0; w < physical; ++w) {
        if (g.physical_to_active[w] == 0xFFFF) {
            g.physical_ignore_mask[w / 8] =
                static_cast<std::uint8_t>(g.physical_ignore_mask[w / 8] | (1u << (w % 8)));
        }
    }
    return g;
}

inline bool geometry_counts_valid(const LedGeometry& g) {
    if (g.physical == 0) {
        return false;
    }
    LedIndex ignored_middle = 0;
    const LedIndex skip0 = std::min(g.edge.skip_start, g.physical);
    const LedIndex skip1 =
        std::min(g.edge.skip_end, static_cast<std::uint16_t>(g.physical - skip0));
    const LedIndex span_begin = skip0;
    const LedIndex span_end = static_cast<LedIndex>(g.physical - skip1);
    for (std::uint16_t i : g.ignored_physical) {
        if (i >= span_begin && i < span_end) {
            ++ignored_middle;
        }
    }
    const LedIndex expected_span =
        static_cast<LedIndex>(g.physical - skip0 - skip1);
    const LedIndex active_in_span = expected_span > ignored_middle
                                        ? static_cast<LedIndex>(expected_span - ignored_middle)
                                        : 0;
    return active_in_span == g.active_count() && g.active_to_physical.size() == g.active_count();
}

} // namespace lumos
