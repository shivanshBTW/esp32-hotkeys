#pragma once

#include "lumos/core/types.hpp"

#include <cstdint>
#include <string>

namespace lumos {

enum class StartupPluginMode : std::uint8_t {
    HyperHdr = 0,
    Static,
    Bias,
    Rainbow,
    Off,
    LastUsed,
};

enum class FallbackPluginMode : std::uint8_t {
    Off = 0,
    Rainbow,
    Static,
    Bias,
    LastUsed,
};

inline const char* startup_mode_to_plugin_id(StartupPluginMode mode, const std::string& last_used) {
    switch (mode) {
    case StartupPluginMode::HyperHdr:
        return "hyperhdr";
    case StartupPluginMode::Static:
        return "static";
    case StartupPluginMode::Bias:
        return "bias";
    case StartupPluginMode::Rainbow:
        return "rainbow";
    case StartupPluginMode::Off:
        return "off";
    case StartupPluginMode::LastUsed:
        return last_used.empty() ? "hyperhdr" : last_used.c_str();
    }
    return "hyperhdr";
}

inline const char* fallback_mode_to_plugin_id(FallbackPluginMode mode, const std::string& last_used) {
    switch (mode) {
    case FallbackPluginMode::Off:
        return "off";
    case FallbackPluginMode::Rainbow:
        return "rainbow";
    case FallbackPluginMode::Static:
        return "static";
    case FallbackPluginMode::Bias:
        return "bias";
    case FallbackPluginMode::LastUsed:
        return last_used.empty() ? "bias" : last_used.c_str();
    }
    return "bias";
}

} // namespace lumos
