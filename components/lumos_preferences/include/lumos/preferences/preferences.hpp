#pragma once

#include "lumos/core/color.hpp"
#include "lumos/core/led_calibration.hpp"
#include "lumos/core/led_geometry.hpp"
#include "lumos/core/mode_map.hpp"
#include "lumos/core/perimeter_map.hpp"
#include "lumos/core/result.hpp"
#include "lumos/core/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumos {

struct LedLayout {
    std::uint16_t top{44};
    std::uint16_t right{26};
    std::uint16_t bottom{44};
    std::uint16_t left{26};

    LedIndex total() const {
        return static_cast<LedIndex>(top + right + bottom + left);
    }
};

struct DoorbellSettings {
    bool enabled{false};
    int relay_pin{kDefaultRelayGpio};
    bool active_high{true};
    bool tone{false}; // PWM beep for a buzzer; leave off for a relay module
    std::uint16_t press_ms{400};
    std::string paired_tx_mac; // "AA:BB:CC:DD:EE:FF"; empty = unpaired (ignore all)
};

struct DeviceSettings {
    // Physical wire length (driver / framebuffer size). NVS key remains "led_count".
    LedIndex led_count{kDefaultLedCount};
    int gpio{kDefaultLedGpio};
    Chipset chipset{Chipset::Ws2815};
    ColorOrder color_order{ColorOrder::Grb};
    Brightness brightness{kDefaultBrightness};
    float gamma{kDefaultGamma};
    std::uint8_t balance_r{kDefaultChannelBalance};
    std::uint8_t balance_g{kDefaultChannelBalance};
    std::uint8_t balance_b{kDefaultChannelBalance};
    std::uint16_t power_limit_ma{kDefaultPowerLimitMa};
    WhiteAlgorithm white_algorithm{WhiteAlgorithm::ExtractMin};
    // Active LEDs per TV edge (HyperHDR). Sum is active_led_count, not physical.
    LedLayout layout{};
    // Wire orientation relative to logical CW-from-top-left order (HyperHDR / UI).
    PerimeterStart perimeter_start{PerimeterStart::TopLeft};
    PerimeterDirection perimeter_direction{PerimeterDirection::Clockwise};
    // Physical wire indices that stay off (middle disables). Ends use edge_ignore skips.
    std::vector<std::uint16_t> ignored_leds{};
    EdgeIgnoreParams edge_ignore{};
    StartupPluginMode startup_plugin{StartupPluginMode::HyperHdr};
    FallbackPluginMode fallback_plugin{FallbackPluginMode::Bias};
    Milliseconds hyperhdr_timeout_ms{kDefaultHyperHdrTimeoutMs};
    std::string last_used_plugin{"hyperhdr"};
    std::string wifi_ssid;
    std::string wifi_password;
    std::string hostname{"Hotkeys"};

    // Static IPv4 (when wifi_use_static is true). Empty strings keep defaults on apply.
    bool wifi_use_static{false};
    std::string wifi_ip;       // e.g. 192.168.1.50
    std::string wifi_gateway;  // e.g. 192.168.1.1
    std::string wifi_netmask{"255.255.255.0"};
    std::string wifi_dns1;     // primary DNS; defaults to gateway if empty
    std::string wifi_dns2;     // secondary DNS; optional

    DoorbellSettings doorbell{};

    LedIndex active_led_count() const { return layout.total(); }
    LedIndex physical_led_count() const { return led_count; }

    LedGeometry geometry() const {
        return build_led_geometry(
            led_count, LedLayoutCounts{layout.top, layout.right, layout.bottom, layout.left},
            edge_ignore, ignored_leds, perimeter_start, perimeter_direction);
    }

    // If layout is empty/invalid, seed a 16:9-ish active layout that fits under physical.
    void normalize_layout();
    void normalize_ignored_leds();
};

class Preferences {
public:
    Result<void> init();
    Result<void> load();
    Result<void> save();

    DeviceSettings& device() { return device_; }
    const DeviceSettings& device() const { return device_; }

    // Plugin params stored as string key/value maps under plugin id.
    std::unordered_map<std::string, std::string>& plugin_params(const std::string& plugin_id);
    const std::unordered_map<std::string, std::string>& plugin_params(
        const std::string& plugin_id) const;
    const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>&
    all_plugin_params() const {
        return plugin_params_;
    }
    void replace_all_plugin_params(
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> next) {
        plugin_params_ = std::move(next);
    }

    void set_plugin_param(const std::string& plugin_id, const std::string& key,
                          const std::string& value);
    std::string get_plugin_param(const std::string& plugin_id, const std::string& key,
                                 const std::string& default_value = {}) const;

    static const char* startup_mode_to_plugin_id(StartupPluginMode mode,
                                                 const std::string& last_used) {
        return lumos::startup_mode_to_plugin_id(mode, last_used);
    }
    static const char* fallback_mode_to_plugin_id(FallbackPluginMode mode,
                                                  const std::string& last_used) {
        return lumos::fallback_mode_to_plugin_id(mode, last_used);
    }

private:
    Result<void> load_plugin_blob();
    Result<void> save_plugin_blob();

    DeviceSettings device_{};
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        plugin_params_;
    bool initialized_{false};
};

} // namespace lumos
