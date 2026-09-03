#pragma once

#include "lumos/core/result.hpp"
#include "lumos/hotkeys/hotkeys_types.hpp"
#include "lumos/preferences/preferences.hpp"
#include "lumos/wifi/wifi_service.hpp"

#include <cJSON.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace lumos {

class HotkeysService {
public:
    HotkeysService(Preferences& preferences, WifiService& wifi);

    Result<void> start();
    void apply_settings();

    const HotkeysSettings& settings() const { return settings_; }
    HotkeysStatus status() const;

    // Persist settings (clamped). Empty HA token in `next` keeps the stored token.
    Result<void> update(const HotkeysSettings& next, bool token_provided);

    // JSON for GET / config. Token is omitted unless include_secrets.
    cJSON* to_json(bool include_secrets) const;
    // Merge a POST/config object into settings and save.
    Result<void> apply_json(const cJSON* obj);

    // Queue a fire off the HTTP task. Safe to call after the response is sent.
    void test_fire(int id);

private:
    static void fire_task(void* arg);
    void run_fire(int id);
    bool build_request(int id, std::string& method, std::string& url, std::string& body,
                       std::array<HotkeyHeader, kHotkeyHeaderCount + 1>& headers, int& header_n,
                       std::string& err) const;
    bool merge_json(const cJSON* obj, HotkeysSettings& next, bool& token_provided) const;
    void persist();
    int action_count() const;
    void configure_keypad();
    void release_keypad_pins();
    void scan_once();
    static void scan_task(void* arg);

    Preferences& preferences_;
    WifiService& wifi_;
    HotkeysSettings settings_{};
    bool started_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> scan_ready_{false};
    bool scan_task_started_{false};
    std::array<int, 4> active_rows_{0, 0, 0, 0};
    std::array<int, 4> active_cols_{0, 0, 0, 0};
    std::uint16_t scan_raw_{0};
    std::uint16_t scan_stable_{0};
    int pending_id_{-1};
    int last_id_{-1};
    int last_key_{-1};
    int last_http_status_{0};
    std::string last_error_;
    std::uint32_t last_fire_ms_{0};
};

} // namespace lumos
