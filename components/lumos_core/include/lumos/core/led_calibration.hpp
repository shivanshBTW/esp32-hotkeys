#pragma once

#include "lumos/core/perimeter_map.hpp"
#include "lumos/core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lumos {

// Edge ignore helpers for a clockwise perimeter: top → right → bottom → left.
// Physical strip may have unused LEDs at the ends and folded LEDs at corners.
struct EdgeIgnoreParams {
    std::uint16_t skip_start{0};
    std::uint16_t skip_end{0};
    std::uint16_t corner_tr{0}; // end of top (top→right fold)
    std::uint16_t corner_br{0}; // end of right
    std::uint16_t corner_bl{0}; // end of bottom
    std::uint16_t corner_tl{0}; // end of left (left→top fold)
};

inline void sort_unique_indices(std::vector<std::uint16_t>& indices, LedIndex led_count) {
    indices.erase(std::remove_if(indices.begin(), indices.end(),
                                 [led_count](std::uint16_t i) { return i >= led_count; }),
                  indices.end());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
}

// Build ignore indices in logical space. skip_start/end are along the wire (physical);
// corner_* are logical TV-side ends (folds).
inline std::vector<std::uint16_t> edge_ignore_indices(LedIndex led_count, std::uint16_t top,
                                                      std::uint16_t right, std::uint16_t bottom,
                                                      std::uint16_t left, const EdgeIgnoreParams& e,
                                                      const PerimeterMaps& maps = {}) {
    std::vector<std::uint16_t> out;
    const std::uint32_t sum =
        static_cast<std::uint32_t>(top) + right + bottom + left;
    if (led_count == 0 || sum != led_count) {
        return out;
    }

    const auto push_logical = [&](std::uint16_t logical) {
        if (logical < led_count) {
            out.push_back(logical);
        }
    };

    const bool have_maps = maps.physical_to_logical.size() == led_count;
    for (std::uint16_t w = 0; w < e.skip_start && w < led_count; ++w) {
        push_logical(have_maps ? maps.physical_to_logical[w] : w);
    }
    for (std::uint16_t k = 0; k < e.skip_end && k < led_count; ++k) {
        const std::uint16_t w = static_cast<std::uint16_t>(led_count - 1 - k);
        push_logical(have_maps ? maps.physical_to_logical[w] : w);
    }

    const int right0 = top;
    const int bottom0 = top + right;
    const int left0 = top + right + bottom;

    const auto push_range_logical = [&](int begin, int end) {
        begin = std::max(0, begin);
        end = std::min(static_cast<int>(led_count), end);
        for (int i = begin; i < end; ++i) {
            push_logical(static_cast<std::uint16_t>(i));
        }
    };

    // Ignore the last N LEDs of each logical side (corner folds).
    push_range_logical(right0 - static_cast<int>(e.corner_tr), right0);
    push_range_logical(bottom0 - static_cast<int>(e.corner_br), bottom0);
    push_range_logical(left0 - static_cast<int>(e.corner_bl), left0);
    push_range_logical(static_cast<int>(led_count) - static_cast<int>(e.corner_tl),
                       static_cast<int>(led_count));

    sort_unique_indices(out, led_count);
    return out;
}

inline void merge_ignore_indices(std::vector<std::uint16_t>& dst,
                                 const std::vector<std::uint16_t>& add, LedIndex led_count) {
    dst.insert(dst.end(), add.begin(), add.end());
    sort_unique_indices(dst, led_count);
}

// Bit mask: bit i set ⇒ LED i ignored (forced off at present, unless bypassed).
inline std::vector<std::uint8_t> ignore_mask_from_indices(LedIndex led_count,
                                                          const std::vector<std::uint16_t>& indices) {
    std::vector<std::uint8_t> mask((led_count + 7) / 8, 0);
    for (std::uint16_t idx : indices) {
        if (idx < led_count) {
            mask[idx / 8] = static_cast<std::uint8_t>(mask[idx / 8] | (1u << (idx % 8)));
        }
    }
    return mask;
}

inline bool ignore_mask_test(const std::vector<std::uint8_t>& mask, std::size_t i) {
    if (mask.empty() || i / 8 >= mask.size()) {
        return false;
    }
    return (mask[i / 8] & (1u << (i % 8))) != 0;
}

} // namespace lumos
