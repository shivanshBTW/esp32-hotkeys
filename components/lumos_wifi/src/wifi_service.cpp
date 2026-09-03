#include "lumos/wifi/wifi_service.hpp"
#include "lumos/wifi/captive_dns.hpp"
#include "lumos/core/logger.hpp"
#include "lumos/core/types.hpp"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

namespace lumos {
namespace {

Logger log{"wifi"};
std::unique_ptr<CaptiveDns> g_captive_dns;

constexpr int kStaFailLimit = 5;
constexpr std::uint64_t kBackgroundRetryUs = 30ULL * 1000ULL * 1000ULL;
constexpr std::int64_t kUiPresenceHoldMs = 10000;

void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiService*>(arg);
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            self->on_sta_start();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            log.warn("STA disconnected");
            self->on_sta_disconnected();
        } else if (id == WIFI_EVENT_AP_START) {
            log.info("SoftAP started");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        self->on_got_ip();
    }
}

// mdns_init + service_add is too heavy for the default event task (sys_evt).
void mdns_worker(void* arg) {
    auto* self = static_cast<WifiService*>(arg);
    self->start_mdns();
    vTaskDelete(nullptr);
}

Result<void> wifi_err(esp_err_t err, const char* what) {
    if (err == ESP_OK) {
        return Result<void>::ok();
    }
    log.error("%s failed: %s", what, esp_err_to_name(err));
    return Result<void>::fail(ErrorCode::NetworkError, what);
}

// esp_wifi_start() returns ESP_ERR_WIFI_CONN if the radio is already up.
esp_err_t wifi_start_soft() {
    const esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    return err;
}

void schedule_mdns(WifiService* self) {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) {
        return;
    }
    if (xTaskCreate(&mdns_worker, "wifi_mdns", 8192, self, 5, nullptr) != pdPASS) {
        started = false;
        log.error("mdns task create failed");
    }
}

} // namespace

WifiService* WifiService::instance_ = nullptr;

WifiService::WifiService(Preferences& preferences) : preferences_(preferences) {}

WifiService::~WifiService() {
    stop();
}

void WifiService::ensure_netif() {
    static bool netif_inited = false;
    if (!netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        netif_inited = true;
    }
    if (sta_netif_ == nullptr) {
        sta_netif_ = esp_netif_create_default_wifi_sta();
        apply_hostname();
    }
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }
}

std::string WifiService::mdns_hostname_label(const std::string& hostname) {
    std::string out;
    out.reserve(hostname.size());
    for (unsigned char c : hostname) {
        if (std::isalnum(c) || c == '-') {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out.empty() ? "hotkeys" : out;
}

void WifiService::apply_hostname() {
    auto* netif = static_cast<esp_netif_t*>(sta_netif_);
    if (netif == nullptr) {
        return;
    }
    std::string hostname = preferences_.device().hostname;
    if (hostname.empty()) {
        hostname = "Hotkeys";
        preferences_.device().hostname = hostname;
    }
    // DHCP client hostname (what routers list). Cap to ESP-IDF limit.
    if (hostname.size() > 32) {
        hostname.resize(32);
    }
    esp_err_t err = esp_netif_set_hostname(netif, hostname.c_str());
    if (err != ESP_OK) {
        log.warn("esp_netif_set_hostname failed: %s", esp_err_to_name(err));
        return;
    }
    log.info("STA DHCP hostname: %s", hostname.c_str());
}

void WifiService::on_sta_start() {
    if (want_sta_connect_ && !setup_ap_active_) {
        esp_wifi_connect();
    }
}

void WifiService::on_sta_disconnected() {
    sta_up_ = false;
    status_.connected = false;
    if (retry_in_flight_) {
        retry_in_flight_ = false;
        log.info("STA retry missed — staying on setup AP");
        return;
    }
    if (!want_sta_connect_ || setup_ap_active_) {
        return;
    }
    const int fails = ++sta_fails_;
    if (fails >= kStaFailLimit) {
        log.warn("STA failed %d times — setup AP (password kept)", fails);
        enter_setup_ap();
        return;
    }
    log.info("STA retry %d/%d", fails, kStaFailLimit);
    esp_wifi_connect();
}

bool WifiService::has_saved_ssid() const {
    return !preferences_.device().wifi_ssid.empty();
}

void WifiService::note_ui_activity() {
    last_ui_ms_ = esp_timer_get_time() / 1000;
}

void WifiService::note_ui_activity_global() {
    if (instance_ != nullptr) {
        instance_->note_ui_activity();
    }
}

bool WifiService::ui_session_active() const {
    const auto last = last_ui_ms_.load();
    if (last <= 0) {
        return false;
    }
    return (esp_timer_get_time() / 1000 - last) < kUiPresenceHoldMs;
}

void WifiService::retry_timer_cb(void* arg) {
    auto* self = static_cast<WifiService*>(arg);
    if (self == nullptr) {
        return;
    }
    self->begin_background_sta_attempt();
}

void WifiService::arm_retry_timer() {
    if (retry_timer_ == nullptr) {
        const esp_timer_create_args_t args{
            .callback = &WifiService::retry_timer_cb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_retry",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &retry_timer_) != ESP_OK) {
            log.error("wifi retry timer create failed");
            return;
        }
    }
    esp_timer_stop(retry_timer_);
    if (has_saved_ssid()) {
        esp_timer_start_periodic(retry_timer_, kBackgroundRetryUs);
    }
}

void WifiService::stop_retry_timer() {
    if (retry_timer_ != nullptr) {
        esp_timer_stop(retry_timer_);
    }
}

void WifiService::begin_background_sta_attempt() {
    if (!has_saved_ssid() || sta_up_ || !setup_ap_active_) {
        return;
    }
    if (ui_session_active()) {
        return;
    }
    if (retry_in_flight_) {
        return;
    }
    log.info("Background STA retry %s", preferences_.device().wifi_ssid.c_str());
    (void)apply_sta_config_and_connect(true);
}

Result<void> WifiService::start() {
    ensure_netif();
    instance_ = this;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (auto r = wifi_err(esp_wifi_init(&cfg), "esp_wifi_init"); !r) {
        return r;
    }
    if (auto r = wifi_err(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, this),
                          "wifi event register");
        !r) {
        return r;
    }
    if (auto r = wifi_err(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, this),
                          "ip event register");
        !r) {
        return r;
    }

    if (auto r = wifi_err(esp_wifi_set_storage(WIFI_STORAGE_RAM), "wifi set storage"); !r) {
        return r;
    }
    started_ = true;

    if (has_saved_ssid()) {
        auto result = start_sta_from_prefs();
        if (result) {
            return result;
        }
        log.warn("STA connect setup failed — starting AP");
        (void)esp_wifi_stop();
    }
    return start_ap();
}

Result<void> WifiService::start_sta_from_prefs() {
    // Do not rewrite NVS on every boot — connect_sta() persists credentials.
    sta_fails_ = 0;
    return apply_sta_config_and_connect(false);
}

Result<void> WifiService::apply_sta_ip_config() {
    auto* netif = static_cast<esp_netif_t*>(sta_netif_);
    if (netif == nullptr) {
        return Result<void>::fail(ErrorCode::NotInitialized, "sta netif missing");
    }

    const auto& d = preferences_.device();
    if (!d.wifi_use_static) {
        esp_err_t err = esp_netif_dhcpc_start(netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return Result<void>::fail(ErrorCode::NetworkError, "failed to start DHCP client");
        }
        status_.use_static = false;
        log.info("STA IP mode: DHCP");
        return Result<void>::ok();
    }

    if (d.wifi_ip.empty() || d.wifi_gateway.empty()) {
        return Result<void>::fail(ErrorCode::InvalidArgument,
                                  "static IP requires wifi_ip and wifi_gateway");
    }

    const std::string& mask = d.wifi_netmask.empty() ? "255.255.255.0" : d.wifi_netmask;
    const std::string& dns1 = d.wifi_dns1.empty() ? d.wifi_gateway : d.wifi_dns1;

    esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t ip_info{};
    if (esp_netif_str_to_ip4(d.wifi_ip.c_str(), &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(d.wifi_gateway.c_str(), &ip_info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(mask.c_str(), &ip_info.netmask) != ESP_OK) {
        return Result<void>::fail(ErrorCode::InvalidArgument, "invalid static IP/gateway/netmask");
    }

    esp_err_t err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return Result<void>::fail(ErrorCode::NetworkError, "esp_netif_set_ip_info failed");
    }

    esp_netif_dns_info_t dns_main{};
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(dns1.c_str(), &dns_main.ip.u_addr.ip4) != ESP_OK) {
        return Result<void>::fail(ErrorCode::InvalidArgument, "invalid DNS 1 address");
    }
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);

    if (!d.wifi_dns2.empty()) {
        esp_netif_dns_info_t dns_backup{};
        dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(d.wifi_dns2.c_str(), &dns_backup.ip.u_addr.ip4) != ESP_OK) {
            return Result<void>::fail(ErrorCode::InvalidArgument, "invalid DNS 2 address");
        }
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        status_.dns2 = d.wifi_dns2;
    } else {
        status_.dns2.clear();
    }

    status_.use_static = true;
    status_.ip = d.wifi_ip;
    status_.gateway = d.wifi_gateway;
    status_.netmask = mask;
    status_.dns1 = dns1;
    log.info("STA IP mode: static %s gw %s dns1 %s", d.wifi_ip.c_str(), d.wifi_gateway.c_str(),
             dns1.c_str());
    return Result<void>::ok();
}

Result<void> WifiService::apply_sta_config_and_connect(bool keep_ap) {
    const auto& d = preferences_.device();
    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), d.wifi_ssid.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), d.wifi_password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode =
        d.wifi_password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    apply_hostname();
    if (keep_ap || setup_ap_active_) {
        if (auto r = wifi_err(esp_wifi_set_mode(WIFI_MODE_APSTA), "wifi set APSTA"); !r) {
            return r;
        }
        retry_in_flight_ = true;
        want_sta_connect_ = false;
    } else {
        if (g_captive_dns) {
            g_captive_dns->stop();
            g_captive_dns.reset();
            captive_dns_running_ = false;
        }
        if (auto r = wifi_err(esp_wifi_set_mode(WIFI_MODE_STA), "wifi set STA"); !r) {
            return r;
        }
        want_sta_connect_ = true;
        retry_in_flight_ = false;
    }
    if (auto r = wifi_err(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "wifi set STA config"); !r) {
        return r;
    }

    auto ip_result = apply_sta_ip_config();
    if (!ip_result) {
        retry_in_flight_ = false;
        log.error("IP config failed: %s", ip_result.error().message.c_str());
        return ip_result;
    }

    if (auto r = wifi_err(wifi_start_soft(), "wifi start"); !r) {
        retry_in_flight_ = false;
        return r;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();

    status_.ssid = d.wifi_ssid;
    status_.connected = false;
    if (!keep_ap && !setup_ap_active_) {
        status_.mode = WifiMode::Station;
    }
    log.info("Connecting STA to %s%s", d.wifi_ssid.c_str(),
             (keep_ap || setup_ap_active_) ? " (keeping setup AP)" : "");
    return Result<void>::ok();
}

Result<void> WifiService::connect_sta(const std::string& ssid, const std::string& password) {
    ensure_netif();

    // Empty password keeps the previously saved one (UI "leave blank to keep").
    std::string effective_password = password;
    if (effective_password.empty() && !preferences_.device().wifi_password.empty() &&
        preferences_.device().wifi_ssid == ssid) {
        effective_password = preferences_.device().wifi_password;
    }

    preferences_.device().wifi_ssid = ssid;
    preferences_.device().wifi_password = effective_password;
    preferences_.save();
    sta_fails_ = 0;

    const bool keep_ap = setup_ap_active_;
    return apply_sta_config_and_connect(keep_ap);
}

Result<void> WifiService::retry_saved() {
    if (!has_saved_ssid()) {
        return Result<void>::fail(ErrorCode::InvalidArgument, "no saved wifi");
    }
    sta_fails_ = 0;
    return apply_sta_config_and_connect(true);
}

Result<void> WifiService::forget_wifi() {
    preferences_.device().wifi_ssid.clear();
    preferences_.device().wifi_password.clear();
    preferences_.save();
    sta_fails_ = 0;
    want_sta_connect_ = false;
    retry_in_flight_ = false;
    sta_up_ = false;
    stop_retry_timer();
    return start_ap();
}

void WifiService::enter_setup_ap() {
    (void)start_ap();
}

void WifiService::drop_ap_to_sta() {
    setup_ap_active_ = false;
    retry_in_flight_ = false;
    want_sta_connect_ = true;
    stop_retry_timer();
    if (g_captive_dns) {
        g_captive_dns->stop();
        g_captive_dns.reset();
        captive_dns_running_ = false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

Result<void> WifiService::start_ap(const std::string& ssid) {
    ensure_netif();
    want_sta_connect_ = false;
    retry_in_flight_ = false;
    sta_up_ = false;
    setup_ap_active_ = true;
    if (g_captive_dns) {
        g_captive_dns->stop();
        g_captive_dns.reset();
        captive_dns_running_ = false;
    }

    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), ssid.c_str(),
                 sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = static_cast<std::uint8_t>(ssid.size());
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    if (auto r = wifi_err(esp_wifi_set_mode(WIFI_MODE_APSTA), "wifi set APSTA"); !r) {
        return r;
    }
    if (auto r = wifi_err(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), "wifi set AP config"); !r) {
        return r;
    }
    if (auto r = wifi_err(wifi_start_soft(), "wifi start"); !r) {
        return r;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_ip_info_t ip_info{};
    esp_netif_get_ip_info(static_cast<esp_netif_t*>(ap_netif_), &ip_info);

    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    status_.mode = WifiMode::AccessPoint;
    status_.connected = false;
    status_.ssid = ssid;
    status_.ip = ip_str;

    arm_retry_timer();

    log.info("AP %s at %s%s", ssid.c_str(), ip_str,
             has_saved_ssid() ? " (saved home Wi-Fi kept)" : "");
    if (status_cb_) {
        status_cb_(status());
    }
    return Result<void>::ok();
}

Result<std::vector<WifiNetwork>> WifiService::scan() {
    // Ensure STA interface exists for scanning (APSTA or STA).
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else if (mode == WIFI_MODE_NULL) {
        return Result<std::vector<WifiNetwork>>::fail(ErrorCode::NotInitialized, "wifi not started");
    }

    wifi_scan_config_t scan_config{};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        return Result<std::vector<WifiNetwork>>::fail(ErrorCode::NetworkError, "wifi scan failed");
    }

    std::uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return Result<std::vector<WifiNetwork>>::ok({});
    }
    if (ap_count > 40) {
        ap_count = 40;
    }

    std::vector<wifi_ap_record_t> records(ap_count);
    err = esp_wifi_scan_get_ap_records(&ap_count, records.data());
    if (err != ESP_OK) {
        return Result<std::vector<WifiNetwork>>::fail(ErrorCode::NetworkError, "get scan records failed");
    }

    std::vector<WifiNetwork> networks;
    networks.reserve(ap_count);
    std::set<std::string> seen;
    for (std::uint16_t i = 0; i < ap_count; ++i) {
        const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
        if (ssid[0] == '\0') {
            continue;
        }
        std::string name(ssid);
        if (!seen.insert(name).second) {
            continue; // keep strongest (records are usually RSSI-sorted)
        }
        WifiNetwork n;
        n.ssid = std::move(name);
        n.rssi = records[i].rssi;
        n.channel = records[i].primary;
        n.secure = records[i].authmode != WIFI_AUTH_OPEN;
        networks.push_back(std::move(n));
    }

    std::sort(networks.begin(), networks.end(),
              [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });

    log.info("WiFi scan found %u networks", static_cast<unsigned>(networks.size()));
    return Result<std::vector<WifiNetwork>>::ok(std::move(networks));
}

void WifiService::on_got_ip() {
    esp_netif_ip_info_t ip_info{};
    esp_netif_get_ip_info(static_cast<esp_netif_t*>(sta_netif_), &ip_info);
    char ip_str[16];
    char gw_str[16];
    char mask_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    esp_ip4addr_ntoa(&ip_info.gw, gw_str, sizeof(gw_str));
    esp_ip4addr_ntoa(&ip_info.netmask, mask_str, sizeof(mask_str));
    status_.connected = true;
    status_.mode = WifiMode::Station;
    status_.ip = ip_str;
    status_.gateway = gw_str;
    status_.netmask = mask_str;
    status_.use_static = preferences_.device().wifi_use_static;

    esp_netif_dns_info_t dns_main{};
    if (esp_netif_get_dns_info(static_cast<esp_netif_t*>(sta_netif_), ESP_NETIF_DNS_MAIN,
                               &dns_main) == ESP_OK &&
        dns_main.ip.type == ESP_IPADDR_TYPE_V4) {
        char dns_str[16];
        esp_ip4addr_ntoa(&dns_main.ip.u_addr.ip4, dns_str, sizeof(dns_str));
        status_.dns1 = dns_str;
    }
    esp_netif_dns_info_t dns_backup{};
    if (esp_netif_get_dns_info(static_cast<esp_netif_t*>(sta_netif_), ESP_NETIF_DNS_BACKUP,
                               &dns_backup) == ESP_OK &&
        dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
        char dns_str[16];
        esp_ip4addr_ntoa(&dns_backup.ip.u_addr.ip4, dns_str, sizeof(dns_str));
        status_.dns2 = dns_str;
    } else {
        status_.dns2.clear();
    }

    log.info("Got IP: %s (gw %s)", ip_str, gw_str);
    sta_fails_ = 0;
    sta_up_ = true;
    retry_in_flight_ = false;
    if (setup_ap_active_) {
        drop_ap_to_sta();
    } else {
        want_sta_connect_ = true;
        stop_retry_timer();
    }
    schedule_mdns(this);
    if (status_cb_) {
        status_cb_(status_);
    }
}

Result<void> WifiService::start_mdns() {
    static bool mdns_started = false;
    if (!mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return Result<void>::fail(ErrorCode::NetworkError, "mdns_init failed");
        }
        mdns_started = true;
    }

    const std::string mdns_host = mdns_hostname_label(preferences_.device().hostname);
    mdns_hostname_set(mdns_host.c_str());
    mdns_instance_name_set("Hotkeys");

    std::snprintf(mdns_api_txt_, sizeof(mdns_api_txt_), "%s", kApiVersion.data());
    std::snprintf(mdns_leds_txt_, sizeof(mdns_leds_txt_), "%s", "hotkeys");
    std::snprintf(mdns_chipset_txt_, sizeof(mdns_chipset_txt_), "%s", "hotkeys");

    mdns_txt_item_t hotkeys_txt[] = {
        {"path", "/"},
        {"version", kAppVersion.data()},
        {"api", mdns_api_txt_},
        {"product", "hotkeys"},
    };

    // Re-add is idempotent enough for our use; ignore already-exists errors.
    mdns_service_add("Hotkeys", "_http", "_tcp", 80, nullptr, 0);
    mdns_service_add("Hotkeys", "_hotkeys", "_tcp", 80, hotkeys_txt, 4);
    mdns_service_txt_set("_hotkeys", "_tcp", hotkeys_txt, 4);

    log.info("mDNS started as %s.local", mdns_host.c_str());
    return Result<void>::ok();
}

void WifiService::refresh_neighbors() {
    // Keep the browse short — long mDNS queries starve RMT and flash "off" LEDs white.
    mdns_result_t* results = nullptr;
    esp_err_t err = mdns_query_ptr("_hotkeys", "_tcp", 400, 12, &results);
    const std::int64_t now_ms = esp_timer_get_time() / 1000;
    if (err != ESP_OK) {
        log.warn("mDNS neighbor browse failed: %s", esp_err_to_name(err));
        neighbors_cache_ms_ = now_ms;
        return;
    }

    const std::string self_host = mdns_hostname_label(preferences_.device().hostname);
    const std::string self_ip = status_.ip;

    std::vector<NeighborInfo> found;
    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        NeighborInfo n;
        if (r->hostname) {
            n.hostname = r->hostname;
        } else if (r->instance_name) {
            n.hostname = r->instance_name;
        }
        n.port = r->port ? r->port : 80;
        for (mdns_ip_addr_t* a = r->addr; a != nullptr; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                char ip_str[16];
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip_str, sizeof(ip_str));
                n.ip = ip_str;
                break;
            }
        }
        for (size_t i = 0; i < r->txt_count; ++i) {
            if (r->txt[i].key == nullptr || r->txt[i].value == nullptr) {
                continue;
            }
            const std::string key = r->txt[i].key;
            const std::string val = r->txt[i].value;
            if (key == "version") {
                n.version = val;
            } else if (key == "api") {
                n.api = val;
            } else if (key == "leds") {
                n.leds = val;
            } else if (key == "chipset") {
                n.chipset = val;
            } else if (key == "path") {
                n.path = val;
            }
        }
        if (n.path.empty()) {
            n.path = "/";
        }

        std::string host_lc = mdns_hostname_label(n.hostname);
        if (!self_host.empty() && host_lc == self_host) {
            continue;
        }
        if (!self_ip.empty() && n.ip == self_ip) {
            continue;
        }
        if (n.ip.empty() && n.hostname.empty()) {
            continue;
        }
        found.push_back(std::move(n));
    }
    mdns_query_results_free(results);

    neighbors_cache_ = std::move(found);
    neighbors_cache_ms_ = now_ms;
    log.info("mDNS neighbors: %u", static_cast<unsigned>(neighbors_cache_.size()));
}

std::vector<NeighborInfo> WifiService::neighbors(bool refresh) {
    if (refresh) {
        refresh_neighbors();
    }
    return neighbors_cache_;
}

WifiStatus WifiService::status() const {
    WifiStatus s = status_;
    s.has_saved_wifi = has_saved_ssid();
    s.setup_mode = setup_ap_active_ && !sta_up_;
    s.auto_retry = s.has_saved_wifi && s.setup_mode && !ui_session_active();
    s.connected = sta_up_;

    std::uint8_t mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                      mac[3], mac[4], mac[5]);
        s.mac = buf;
    }

    if (s.setup_mode && ap_netif_ != nullptr) {
        esp_netif_ip_info_t ip_info{};
        if (esp_netif_get_ip_info(static_cast<esp_netif_t*>(ap_netif_), &ip_info) == ESP_OK) {
            char buf[16];
            esp_ip4addr_ntoa(&ip_info.ip, buf, sizeof(buf));
            s.ip = buf;
            esp_ip4addr_ntoa(&ip_info.gw, buf, sizeof(buf));
            s.gateway = buf;
            esp_ip4addr_ntoa(&ip_info.netmask, buf, sizeof(buf));
            s.netmask = buf;
        }
        s.dns1.clear();
        s.dns2.clear();
    }
    return s;
}

void WifiService::stop() {
    want_sta_connect_ = false;
    stop_retry_timer();
    if (retry_timer_ != nullptr) {
        esp_timer_delete(retry_timer_);
        retry_timer_ = nullptr;
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
    if (g_captive_dns) {
        g_captive_dns->stop();
        g_captive_dns.reset();
    }
    if (started_) {
        esp_wifi_stop();
        started_ = false;
    }
}

} // namespace lumos
