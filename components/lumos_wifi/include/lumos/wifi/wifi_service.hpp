#pragma once

#include "lumos/core/result.hpp"
#include "lumos/preferences/preferences.hpp"
#include "lumos/wifi/neighbor_info.hpp"

#include "esp_timer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lumos {

enum class WifiMode {
    Off,
    Station,
    AccessPoint,
};

struct WifiStatus {
    WifiMode mode{WifiMode::Off};
    bool connected{false};
    bool use_static{false};
    std::string ip;
    std::string gateway;
    std::string netmask;
    std::string dns1;
    std::string dns2;
    std::string ssid;
    std::string mac;
    int rssi{0};
    bool has_saved_wifi{false};
    bool setup_mode{false};
    bool auto_retry{false};
};

struct WifiNetwork {
    std::string ssid;
    int rssi{0};
    int channel{0};
    bool secure{false};
};

class WifiService {
public:
    using StatusCallback = std::function<void(const WifiStatus&)>;

    explicit WifiService(Preferences& preferences);
    ~WifiService();

    Result<void> start();
    Result<void> connect_sta(const std::string& ssid, const std::string& password);
    Result<void> retry_saved();
    Result<void> forget_wifi();
    Result<void> start_ap(const std::string& ssid = "Hotkeys-Setup");
    void stop();

    // Blocking scan (~1–3s). Works in APSTA setup mode.
    Result<std::vector<WifiNetwork>> scan();

    WifiStatus status() const;
    void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }
    void on_got_ip();
    void on_sta_start();
    void on_sta_disconnected();

    void note_ui_activity();
    bool ui_session_active() const;
    static void note_ui_activity_global();

    // mDNS: Hotkeys.local + _hotkeys._tcp (generic product TXT)
    Result<void> start_mdns();

    // Apply preferences hostname to STA DHCP/netif (routers show this, not "espressif").
    void apply_hostname();

    // Cached peer list. Pass refresh=true only from an explicit UI action —
    // mDNS browse briefly glitches WS281x/RMT on ESP32.
    std::vector<NeighborInfo> neighbors(bool refresh = false);

private:
    void ensure_netif();
    Result<void> start_sta_from_prefs();
    Result<void> apply_sta_ip_config();
    Result<void> apply_sta_config_and_connect(bool keep_ap);
    void enter_setup_ap();
    void drop_ap_to_sta();
    void begin_background_sta_attempt();
    void arm_retry_timer();
    void stop_retry_timer();
    static void retry_timer_cb(void* arg);
    void refresh_neighbors();
    static std::string mdns_hostname_label(const std::string& hostname);
    bool has_saved_ssid() const;

    Preferences& preferences_;
    WifiStatus status_{};
    StatusCallback status_cb_;
    void* sta_netif_{nullptr};
    void* ap_netif_{nullptr};
    bool started_{false};
    std::atomic<bool> captive_dns_running_{false};
    std::atomic<bool> want_sta_connect_{false};
    std::atomic<bool> setup_ap_active_{false};
    std::atomic<bool> sta_up_{false};
    std::atomic<bool> retry_in_flight_{false};
    std::atomic<int> sta_fails_{0};
    std::atomic<std::int64_t> last_ui_ms_{0};
    esp_timer_handle_t retry_timer_{nullptr};
    static WifiService* instance_;
    char mdns_leds_txt_[8]{"150"};
    char mdns_api_txt_[8]{"0.3"};
    char mdns_chipset_txt_[16]{"ws2815"};
    std::vector<NeighborInfo> neighbors_cache_;
    std::int64_t neighbors_cache_ms_{0};
};

} // namespace lumos
