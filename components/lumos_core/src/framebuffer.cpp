#include "lumos/core/framebuffer.hpp"

namespace lumos {

Framebuffer::Framebuffer(LedIndex count) {
    resize(count);
}

void Framebuffer::resize(LedIndex count) {
    pixels_.assign(count, Rgb::black());
}

void Framebuffer::fill(Rgb color) {
    for (auto& p : pixels_) {
        p = color;
    }
}

} // namespace lumos
