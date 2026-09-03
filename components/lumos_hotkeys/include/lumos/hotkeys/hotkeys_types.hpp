#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace lumos {

inline constexpr int kHotkeySlotCount = 16;
inline constexpr std::size_t kHotkeyNameMax = 32;
inline constexpr std::size_t kHotkeyUrlMax = 192;
inline constexpr std::size_t kHotkeyBodyMax = 256;
inline constexpr std::size_t kHotkeyHeaderMax = 96;
inline constexpr std::size_t kHotkeyServiceMax = 64;
inline constexpr std::size_t kHotkeyTokenMax = 768;
inline constexpr int kHotkeyHeaderCount = 2;

enum class HotkeyType : std::uint8_t {
    Http = 0,
    HomeAssistant = 1,
};

struct HotkeyHeader {
    std::string name;
    std::string value;
};

struct HotkeyAction {
    std::string name;
    HotkeyType type{HotkeyType::Http};
    std::string method{"POST"};
    std::string url;
    std::array<HotkeyHeader, kHotkeyHeaderCount> headers{};
    std::string body;
    std::string service;
    std::string entity_id;
    std::string data;
};

struct HotkeysSettings {
    std::string ha_base_url;
    std::string ha_token;
    bool keypad_enabled{false};
    std::array<int, 4> row_pins{0, 0, 0, 0};
    std::array<int, 4> col_pins{0, 0, 0, 0};
    std::array<HotkeyAction, kHotkeySlotCount> actions{};
};

struct HotkeysStatus {
    bool enabled{false};
    bool keypad_scanning{false};
    int action_count{0};
    int last_id{-1};
    int last_key{-1};
    int last_http_status{0};
    std::string last_error;
    std::uint32_t last_fire_ms{0};
};

inline const char* hotkey_type_to_str(HotkeyType type) {
    return type == HotkeyType::HomeAssistant ? "ha" : "http";
}

inline HotkeyType hotkey_type_from_str(const char* s) {
    if (s != nullptr && (s[0] == 'h' || s[0] == 'H') && s[1] == 'a' && s[2] == '\0') {
        return HotkeyType::HomeAssistant;
    }
    return HotkeyType::Http;
}

inline bool hotkey_slot_empty(const HotkeyAction& a) {
    if (a.type == HotkeyType::HomeAssistant) {
        return a.service.empty();
    }
    return a.url.empty();
}

void clamp_hotkeys_settings(HotkeysSettings& s);

} // namespace lumos
