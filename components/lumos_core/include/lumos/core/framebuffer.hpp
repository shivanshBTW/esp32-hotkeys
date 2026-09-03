#pragma once

#include "lumos/core/color.hpp"
#include "lumos/core/types.hpp"

#include <vector>
#if __cplusplus >= 202002L
#include <span>
#endif

namespace lumos {

class Framebuffer {
public:
    Framebuffer() = default;
    explicit Framebuffer(LedIndex count);

    void resize(LedIndex count);
    LedIndex size() const { return static_cast<LedIndex>(pixels_.size()); }

    Rgb& operator[](LedIndex i) { return pixels_[i]; }
    const Rgb& operator[](LedIndex i) const { return pixels_[i]; }

    void fill(Rgb color);
    void clear() { fill(Rgb::black()); }

#if __cplusplus >= 202002L
    std::span<Rgb> span() { return pixels_; }
    std::span<const Rgb> span() const { return pixels_; }
#endif

    const std::vector<Rgb>& data() const { return pixels_; }
    std::vector<Rgb>& data() { return pixels_; }

private:
    std::vector<Rgb> pixels_;
};

} // namespace lumos
