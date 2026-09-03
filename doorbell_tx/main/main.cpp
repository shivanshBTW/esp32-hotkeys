#include "lumos/core/logger.hpp"
#include "lumos/doorbell/doorbell_mac.hpp"
#include "lumos/doorbell/doorbell_transmitter.hpp"
#include "lumos/doorbell/doorbell_tx_form.hpp"
#include "lumos/doorbell/doorbell_tx_prefs.hpp"
#include "lumos/doorbell/doorbell_tx_ui.hpp"
#include "lumos/ota/ota_service.hpp"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

lumos::Logger log{"doorbell_tx_main"};
lumos::DoorbellTransmitter* g_tx{nullptr};
lumos::OtaService g_ota;
esp_netif_t* g_ap_netif{nullptr};
esp_netif_t* g_sta_netif{nullptr};
bool g_want_sta{false};
bool g_setup_ap{false};
bool g_sta_up{false};
bool g_retry_in_flight{false};
int g_sta_fails{0};
std::atomic<std::int64_t> g_last_ui_ms{0};
esp_timer_handle_t g_retry_timer{nullptr};
std::string g_sta_ip;
int g_sta_rssi{0};

constexpr const char* kIndexHtml = LUMOS_DOORBELL_TX_INDEX_HTML;

lumos::WifiPrefs g_wifi;


std::string nvs_get(nvs_handle_t h, const char* key, const std::string& def = {}) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) {
        return def;
    }
    std::string out(len, '\0');
    if (nvs_get_str(h, key, out.data(), &len) != ESP_OK) {
        return def;
    }
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

void load_wifi_prefs() {
    nvs_handle_t h{};
    if (nvs_open(lumos::kDoorbellWifiNvsNs, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    g_wifi.ssid = nvs_get(h, "ssid");
    g_wifi.password = nvs_get(h, "pass");
    g_wifi.hostname = nvs_get(h, "host", "LumosOS-Bell");
    std::uint8_t st = 0;
    nvs_get_u8(h, "stat", &st);
    g_wifi.use_static = st != 0;
    g_wifi.ip = nvs_get(h, "ip");
    g_wifi.gateway = nvs_get(h, "gw");
    g_wifi.netmask = nvs_get(h, "mask", "255.255.255.0");
    g_wifi.dns1 = nvs_get(h, "dns1");
    g_wifi.dns2 = nvs_get(h, "dns2");
    nvs_close(h);
}

void save_wifi_prefs() {
    nvs_handle_t h{};
    if (nvs_open(lumos::kDoorbellWifiNvsNs, NVS_READWRITE, &h) != ESP_OK) {
        log.error("nvs_open dbwifi failed");
        return;
    }
    nvs_set_str(h, "ssid", g_wifi.ssid.c_str());
    nvs_set_str(h, "pass", g_wifi.password.c_str());
    nvs_set_str(h, "host", g_wifi.hostname.c_str());
    nvs_set_u8(h, "stat", g_wifi.use_static ? 1 : 0);
    nvs_set_str(h, "ip", g_wifi.ip.c_str());
    nvs_set_str(h, "gw", g_wifi.gateway.c_str());
    nvs_set_str(h, "mask", g_wifi.netmask.c_str());
    nvs_set_str(h, "dns1", g_wifi.dns1.c_str());
    nvs_set_str(h, "dns2", g_wifi.dns2.c_str());
    nvs_commit(h);
    nvs_close(h);
}

void start_mdns() {
    static bool started = false;
    if (!started) {
        if (mdns_init() != ESP_OK) {
            log.warn("mdns_init failed");
            return;
        }
        started = true;
    }
    const auto host = lumos::doorbell_mdns_label(g_wifi.hostname);
    mdns_hostname_set(host.c_str());
    mdns_instance_name_set(g_wifi.hostname.c_str());
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    log.info("mDNS http://%s.local", host.c_str());
}

void apply_hostname() {
    if (g_sta_netif == nullptr) {
        return;
    }
    if (g_wifi.hostname.empty()) {
        g_wifi.hostname = "LumosOS-Bell";
    }
    if (g_wifi.hostname.size() > 32) {
        g_wifi.hostname.resize(32);
    }
    esp_netif_set_hostname(g_sta_netif, g_wifi.hostname.c_str());
}

esp_err_t apply_sta_ip_config() {
    if (g_sta_netif == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_wifi.use_static) {
        esp_err_t err = esp_netif_dhcpc_start(g_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return err;
        }
        return ESP_OK;
    }
    if (g_wifi.ip.empty() || g_wifi.gateway.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string& mask = g_wifi.netmask.empty() ? "255.255.255.0" : g_wifi.netmask;
    const std::string& dns1 = g_wifi.dns1.empty() ? g_wifi.gateway : g_wifi.dns1;
    esp_netif_dhcpc_stop(g_sta_netif);
    esp_netif_ip_info_t ip_info{};
    if (esp_netif_str_to_ip4(g_wifi.ip.c_str(), &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(g_wifi.gateway.c_str(), &ip_info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(mask.c_str(), &ip_info.netmask) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_set_ip_info(g_sta_netif, &ip_info);
    esp_netif_dns_info_t dns_main{};
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(dns1.c_str(), &dns_main.ip.u_addr.ip4) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    if (!g_wifi.dns2.empty()) {
        esp_netif_dns_info_t dns_backup{};
        dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(g_wifi.dns2.c_str(), &dns_backup.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
        }
    }
    log.info("STA static %s gw %s", g_wifi.ip.c_str(), g_wifi.gateway.c_str());
    return ESP_OK;
}

void start_ap_portal();
void apply_sta_config_and_connect(bool keep_ap);
void arm_retry_timer();
void stop_retry_timer();
void begin_background_sta_attempt();

bool has_saved_wifi() {
    return !g_wifi.ssid.empty();
}

void note_ui_activity() {
    g_last_ui_ms = esp_timer_get_time() / 1000;
}

bool ui_session_active() {
    const auto last = g_last_ui_ms.load();
    if (last <= 0) {
        return false;
    }
    return (esp_timer_get_time() / 1000 - last) < lumos::kDoorbellUiPresenceHoldMs;
}

void retry_timer_cb(void*) {
    begin_background_sta_attempt();
}

void stop_retry_timer() {
    if (g_retry_timer != nullptr) {
        esp_timer_stop(g_retry_timer);
    }
}

void arm_retry_timer() {
    if (g_retry_timer == nullptr) {
        const esp_timer_create_args_t args{
            .callback = &retry_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_retry",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &g_retry_timer) != ESP_OK) {
            log.error("wifi retry timer create failed");
            return;
        }
    }
    esp_timer_stop(g_retry_timer);
    if (has_saved_wifi()) {
        esp_timer_start_periodic(g_retry_timer, lumos::kDoorbellBackgroundRetryUs);
    }
}

void apply_sta_config_and_connect(bool keep_ap) {
    apply_hostname();
    wifi_config_t sta{};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), g_wifi.ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), g_wifi.password.c_str(),
                 sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = g_wifi.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (keep_ap || g_setup_ap) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        g_retry_in_flight = true;
        g_want_sta = false;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        g_want_sta = true;
        g_retry_in_flight = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    (void)apply_sta_ip_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    log.info("Connecting STA to %s%s", g_wifi.ssid.c_str(),
             (keep_ap || g_setup_ap) ? " (keeping setup AP)" : "");
}

void connect_sta() {
    g_sta_fails = 0;
    apply_sta_config_and_connect(g_setup_ap);
}

void retry_saved() {
    g_sta_fails = 0;
    apply_sta_config_and_connect(true);
}

void begin_background_sta_attempt() {
    if (!has_saved_wifi() || g_sta_up || !g_setup_ap || g_retry_in_flight || ui_session_active()) {
        return;
    }
    log.info("Background STA retry %s", g_wifi.ssid.c_str());
    apply_sta_config_and_connect(true);
}

void drop_ap_to_sta() {
    g_setup_ap = false;
    g_retry_in_flight = false;
    g_want_sta = true;
    stop_retry_timer();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void start_ap_portal() {
    g_want_sta = false;
    g_retry_in_flight = false;
    g_sta_up = false;
    g_setup_ap = true;
    if (g_tx != nullptr) {
        g_tx->set_sta_linked(false);
    }
    g_sta_ip.clear();
    wifi_config_t ap{};
    std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), lumos::kDoorbellApSsid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = static_cast<std::uint8_t>(std::strlen(lumos::kDoorbellApSsid));
    ap.ap.channel = (g_tx != nullptr) ? g_tx->config().channel : 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    esp_netif_ip_info_t ip{};
    if (g_ap_netif != nullptr && esp_netif_get_ip_info(g_ap_netif, &ip) == ESP_OK) {
        log.info("SoftAP %s  " IPSTR "%s", lumos::kDoorbellApSsid, IP2STR(&ip.ip),
                 has_saved_wifi() ? " (saved home Wi-Fi kept)" : "");
    }
    arm_retry_timer();
}

void reboot_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

void wifi_event_handler(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START && g_want_sta && !g_setup_ap) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            log.warn("STA disconnected");
            g_sta_up = false;
            if (g_tx != nullptr) {
                g_tx->set_sta_linked(false);
            }
            g_sta_ip.clear();
            if (g_retry_in_flight) {
                g_retry_in_flight = false;
                log.info("STA retry missed — staying on setup AP");
                return;
            }
            if (!g_want_sta || g_setup_ap) {
                return;
            }
            ++g_sta_fails;
            if (g_sta_fails >= lumos::kDoorbellStaFailLimit) {
                log.warn("STA failed %d times — setup AP (password kept)", g_sta_fails);
                start_ap_portal();
            } else {
                log.info("STA retry %d/%d", g_sta_fails, lumos::kDoorbellStaFailLimit);
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            const auto* ev = static_cast<wifi_event_ap_staconnected_t*>(data);
            log.info("AP client join " MACSTR, MAC2STR(ev->mac));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_sta_fails = 0;
        g_sta_up = true;
        g_retry_in_flight = false;
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        char ip[16];
        esp_ip4addr_ntoa(&ev->ip_info.ip, ip, sizeof(ip));
        g_sta_ip = ip;
        wifi_ap_record_t ap{};
        g_sta_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
        log.info("Got IP %s", ip);
        if (g_setup_ap) {
            drop_ap_to_sta();
        } else {
            g_want_sta = true;
            stop_retry_timer();
        }
        static std::atomic<bool> mdns_started{false};
        bool expected = false;
        if (mdns_started.compare_exchange_strong(expected, true)) {
            if (xTaskCreate(
                    [](void*) {
                        start_mdns();
                        vTaskDelete(nullptr);
                    },
                    "wifi_mdns", 8192, nullptr, 5, nullptr) != pdPASS) {
                mdns_started = false;
                log.error("mdns task create failed");
            }
        }
        if (g_tx != nullptr) {
            g_tx->set_sta_linked(true);
        }
    }
}

esp_err_t send_text(httpd_req_t* req, const char* body, const char* type = "text/plain") {
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_cjson(httpd_req_t* req, cJSON* root, int status = 200) {
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return send_text(req, "{\"error\":\"json\"}", "application/json");
    }
    if (status != 200) {
        httpd_resp_set_status(req, std::to_string(status).c_str());
    }
    const esp_err_t err = send_text(req, printed, "application/json");
    cJSON_free(printed);
    return err;
}

esp_err_t read_body(httpd_req_t* req, std::string& out) {
    const int total = req->content_len;
    if (total <= 0 || total > 8192) {
        return ESP_FAIL;
    }
    out.resize(static_cast<std::size_t>(total));
    int got = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, out.data() + got, total - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    return ESP_OK;
}

esp_err_t get_index(httpd_req_t* req) {
    note_ui_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t get_api(httpd_req_t* req) {
    note_ui_activity();
    cJSON* root = cJSON_CreateObject();
    if (g_tx != nullptr) {
        const auto st = g_tx->status();
        const auto& cfg = st.cfg;
        cJSON_AddStringToObject(root, "board", lumos::kIdfTargetName);
        cJSON_AddStringToObject(root, "own_mac", st.own_mac.c_str());
        cJSON_AddStringToObject(root, "rx_mac",
                                cfg.rx_mac_valid ? lumos::format_mac(cfg.rx_mac).c_str() : "");
        cJSON_AddBoolToObject(root, "paired", st.paired);
        cJSON_AddNumberToObject(root, "channel", cfg.channel);
        cJSON_AddNumberToObject(root, "opto_pin", cfg.opto_pin);
        cJSON_AddNumberToObject(root, "opto_level", st.opto_level);
        cJSON_AddBoolToObject(root, "active_low", cfg.active_low);
        cJSON_AddBoolToObject(root, "espnow_ready", st.espnow_ready);
        cJSON_AddNumberToObject(root, "last_seq", st.last_seq);
        cJSON_AddNumberToObject(root, "last_send_ms", st.last_send_ms);
        cJSON_AddBoolToObject(root, "pairing", st.pairing);
        cJSON_AddBoolToObject(root, "scanning", st.scanning);
        cJSON_AddNumberToObject(root, "pairing_ms", st.pairing_ms);
        cJSON* peers = cJSON_AddArrayToObject(root, "peers");
        for (int i = 0; i < st.peer_count; ++i) {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "mac", lumos::format_mac(st.peers[i].mac).c_str());
            cJSON_AddStringToObject(p, "name", st.peers[i].name);
            cJSON_AddNumberToObject(p, "channel", st.peers[i].channel);
            cJSON_AddNumberToObject(p, "rssi", st.peers[i].rssi);
            cJSON_AddItemToArray(peers, p);
        }
    }
    cJSON_AddStringToObject(root, "wifi_ssid", g_wifi.ssid.c_str());
    cJSON_AddStringToObject(root, "hostname", g_wifi.hostname.c_str());
    cJSON_AddBoolToObject(root, "wifi_use_static", g_wifi.use_static);
    cJSON_AddStringToObject(root, "wifi_ip", g_wifi.ip.c_str());
    cJSON_AddStringToObject(root, "wifi_gateway", g_wifi.gateway.c_str());
    cJSON_AddStringToObject(root, "wifi_netmask", g_wifi.netmask.c_str());
    cJSON_AddStringToObject(root, "wifi_dns1", g_wifi.dns1.c_str());
    cJSON_AddStringToObject(root, "wifi_dns2", g_wifi.dns2.c_str());
    cJSON_AddBoolToObject(root, "wifi_connected", !g_sta_ip.empty());
    cJSON_AddStringToObject(root, "sta_ip", g_sta_ip.c_str());
    std::string live_gw;
    std::string live_mask;
    std::string live_dns;
    const bool setup_mode = g_setup_ap && !g_sta_up;
    esp_netif_t* netif = !g_sta_ip.empty() ? g_sta_netif : (setup_mode ? g_ap_netif : nullptr);
    if (netif != nullptr) {
        esp_netif_ip_info_t ip_info{};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char buf[16];
            esp_ip4addr_ntoa(&ip_info.gw, buf, sizeof(buf));
            live_gw = buf;
            esp_ip4addr_ntoa(&ip_info.netmask, buf, sizeof(buf));
            live_mask = buf;
        }
        if (!g_sta_ip.empty()) {
            esp_netif_dns_info_t dns_main{};
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK &&
                dns_main.ip.type == ESP_IPADDR_TYPE_V4) {
                char buf[16];
                esp_ip4addr_ntoa(&dns_main.ip.u_addr.ip4, buf, sizeof(buf));
                live_dns = buf;
            }
        }
    }
    cJSON_AddStringToObject(root, "sta_gateway", live_gw.c_str());
    cJSON_AddStringToObject(root, "sta_netmask", live_mask.c_str());
    cJSON_AddStringToObject(root, "sta_dns", live_dns.c_str());
    cJSON_AddNumberToObject(root, "rssi", g_sta_rssi);
    cJSON_AddStringToObject(root, "ap_ip", lumos::kDoorbellApIp);
    cJSON_AddBoolToObject(root, "sta_linked", g_tx != nullptr && g_tx->sta_linked());
    const bool saved = has_saved_wifi();
    cJSON_AddBoolToObject(root, "has_saved_wifi", saved);
    cJSON_AddBoolToObject(root, "setup_mode", setup_mode);
    cJSON_AddBoolToObject(root, "auto_retry", saved && setup_mode && !ui_session_active());
    return send_cjson(req, root);
}

esp_err_t post_save(httpd_req_t* req) {
    note_ui_activity();
    if (g_tx == nullptr) {
        return send_text(req, "not ready");
    }
    char buf[512];
    const int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_text(req, "bad body");
    }
    buf[len] = 0;
    const std::string body(buf);
    auto cfg = g_tx->config();
    const auto mac = lumos::doorbell_form_value(body, "rx_mac");
    if (!mac.empty()) {
        cfg.rx_mac_valid = lumos::parse_mac(mac, cfg.rx_mac);
    }
    const auto ch = lumos::doorbell_form_value(body, "channel");
    if (!ch.empty()) {
        cfg.channel = static_cast<std::uint8_t>(std::clamp(std::atoi(ch.c_str()), 1, 13));
    }
    const auto pin = lumos::doorbell_form_value(body, "pin");
    if (!pin.empty()) {
        cfg.opto_pin = std::atoi(pin.c_str());
    }
    const auto al = lumos::doorbell_form_value(body, "active_low");
    cfg.active_low = (al != "0" && al != "false");
    g_tx->apply_config(cfg);
    g_tx->save_nvs();
    return send_text(req, "Saved.");
}

esp_err_t post_test(httpd_req_t* req) {
    note_ui_activity();
    if (g_tx == nullptr) {
        return send_text(req, "not ready");
    }
    g_tx->test_send();
    return send_text(req, "Sent (if paired).");
}

esp_err_t post_discover(httpd_req_t* req) {
    note_ui_activity();
    if (g_tx == nullptr) {
        return send_text(req, "not ready");
    }
    g_tx->start_pairing();
    return send_text(req, "Scanning nearby LumosOS receivers…");
}

esp_err_t post_pair(httpd_req_t* req) {
    note_ui_activity();
    if (g_tx == nullptr) {
        return send_text(req, "not ready");
    }
    char buf[256];
    const int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_text(req, "bad body");
    }
    buf[len] = 0;
    const auto mac = lumos::doorbell_form_value(buf, "rx_mac");
    std::uint8_t parsed[6]{};
    if (!lumos::parse_mac(mac, parsed)) {
        return send_text(req, "mac required");
    }
    if (!g_tx->select_peer(parsed)) {
        return send_text(req, "pair failed");
    }
    return send_text(req, "Paired.");
}

esp_err_t get_wifi_scan(httpd_req_t* req) {
    note_ui_activity();
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    wifi_scan_config_t scan{};
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return send_text(req, "{\"error\":\"scan failed\"}", "application/json");
    }
    std::uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    std::vector<wifi_ap_record_t> rec(n);
    if (n > 0) {
        esp_wifi_scan_get_ap_records(&n, rec.data());
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* nets = cJSON_AddArrayToObject(root, "networks");
    std::set<std::string> seen;
    for (std::uint16_t i = 0; i < n; ++i) {
        const char* ssid = reinterpret_cast<const char*>(rec[i].ssid);
        if (ssid[0] == '\0') {
            continue;
        }
        std::string name(ssid);
        if (!seen.insert(name).second) {
            continue;
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", name.c_str());
        cJSON_AddNumberToObject(o, "rssi", rec[i].rssi);
        cJSON_AddNumberToObject(o, "channel", rec[i].primary);
        cJSON_AddBoolToObject(o, "secure", rec[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(nets, o);
    }
    return send_cjson(req, root);
}

const char* json_str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : nullptr;
}

esp_err_t post_wifi(httpd_req_t* req) {
    note_ui_activity();
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_text(req, "{\"error\":\"bad body\"}", "application/json");
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_text(req, "{\"error\":\"invalid json\"}", "application/json");
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(json, "forget"))) {
        cJSON_Delete(json);
        g_wifi = lumos::WifiPrefs{};
        save_wifi_prefs();
        xTaskCreate(&reboot_task, "reboot", 2048, nullptr, 5, nullptr);
        return send_text(req, "{\"ok\":true,\"rebooting\":true}", "application/json");
    }
    if (const char* s = json_str(json, "ssid"); s && s[0]) {
        g_wifi.ssid = s;
    }
    if (const char* s = json_str(json, "password"); s && s[0]) {
        g_wifi.password = s;
    }
    if (const char* s = json_str(json, "hostname"); s && s[0]) {
        g_wifi.hostname = s;
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "use_static"); cJSON_IsBool(v)) {
        g_wifi.use_static = cJSON_IsTrue(v);
    }
    if (const char* s = json_str(json, "ip"); s) {
        g_wifi.ip = s;
    }
    if (const char* s = json_str(json, "gateway"); s) {
        g_wifi.gateway = s;
    }
    if (const char* s = json_str(json, "netmask"); s && s[0]) {
        g_wifi.netmask = s;
    }
    if (const char* s = json_str(json, "dns1"); s) {
        g_wifi.dns1 = s;
    }
    if (const char* s = json_str(json, "dns2"); s) {
        g_wifi.dns2 = s;
    }
    cJSON_Delete(json);
    if (g_wifi.ssid.empty()) {
        return send_text(req, "{\"error\":\"ssid required\"}", "application/json");
    }
    save_wifi_prefs();
    connect_sta();
    return send_text(req, "{\"ok\":true}", "application/json");
}

esp_err_t post_wifi_retry(httpd_req_t* req) {
    note_ui_activity();
    if (!has_saved_wifi()) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "{\"error\":\"no saved wifi\"}", "application/json");
    }
    retry_saved();
    return send_text(req, "{\"ok\":true}", "application/json");
}

esp_err_t post_wifi_presence(httpd_req_t* req) {
    note_ui_activity();
    return send_text(req, "{\"ok\":true}", "application/json");
}

esp_err_t get_android_probe(httpd_req_t* req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t get_apple_probe(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, lumos::kDoorbellAppleCaptiveHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t get_windows_probe(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, lumos::kDoorbellWindowsNcsi, HTTPD_RESP_USE_STRLEN);
}

esp_err_t get_config(httpd_req_t* req) {
    note_ui_activity();
    const bool secrets = std::strstr(req->uri, "secrets=1") != nullptr;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product", lumos::kDoorbellConfigProduct);
    cJSON* device = cJSON_AddObjectToObject(root, "device");
    if (g_tx != nullptr) {
        const auto cfg = g_tx->config();
        cJSON_AddStringToObject(device, "rx_mac",
                                cfg.rx_mac_valid ? lumos::format_mac(cfg.rx_mac).c_str() : "");
        cJSON_AddNumberToObject(device, "channel", cfg.channel);
        cJSON_AddNumberToObject(device, "opto_pin", cfg.opto_pin);
        cJSON_AddBoolToObject(device, "active_low", cfg.active_low);
        cJSON_AddNumberToObject(device, "tx_id", static_cast<double>(cfg.tx_id));
    }
    cJSON_AddStringToObject(device, "wifi_ssid", g_wifi.ssid.c_str());
    cJSON_AddStringToObject(device, "hostname", g_wifi.hostname.c_str());
    if (secrets) {
        cJSON_AddStringToObject(device, "wifi_password", g_wifi.password.c_str());
    }
    cJSON_AddBoolToObject(device, "wifi_use_static", g_wifi.use_static);
    cJSON_AddStringToObject(device, "wifi_ip", g_wifi.ip.c_str());
    cJSON_AddStringToObject(device, "wifi_gateway", g_wifi.gateway.c_str());
    cJSON_AddStringToObject(device, "wifi_netmask", g_wifi.netmask.c_str());
    cJSON_AddStringToObject(device, "wifi_dns1", g_wifi.dns1.c_str());
    cJSON_AddStringToObject(device, "wifi_dns2", g_wifi.dns2.c_str());
    return send_cjson(req, root);
}

esp_err_t post_config(httpd_req_t* req) {
    note_ui_activity();
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_text(req, "{\"error\":\"bad body\"}", "application/json");
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_text(req, "{\"error\":\"invalid json\"}", "application/json");
    }
    cJSON* device = cJSON_GetObjectItem(json, "device");
    if (!cJSON_IsObject(device)) {
        device = json;
    }
    const bool clear_ip = cJSON_IsTrue(cJSON_GetObjectItem(json, "clear_static_ip"));
    if (g_tx != nullptr) {
        auto cfg = g_tx->config();
        if (const char* s = json_str(device, "rx_mac"); s) {
            cfg.rx_mac_valid = lumos::parse_mac(s, cfg.rx_mac);
        }
        if (const cJSON* v = cJSON_GetObjectItem(device, "channel"); cJSON_IsNumber(v)) {
            cfg.channel = static_cast<std::uint8_t>(std::clamp(v->valueint, 1, 13));
        }
        if (const cJSON* v = cJSON_GetObjectItem(device, "opto_pin"); cJSON_IsNumber(v)) {
            cfg.opto_pin = v->valueint;
        }
        if (const cJSON* v = cJSON_GetObjectItem(device, "active_low"); cJSON_IsBool(v)) {
            cfg.active_low = cJSON_IsTrue(v);
        }
        g_tx->apply_config(cfg);
        g_tx->save_nvs();
    }
    if (const char* s = json_str(device, "wifi_ssid"); s) {
        g_wifi.ssid = s;
    }
    if (const char* s = json_str(device, "wifi_password"); s) {
        g_wifi.password = s;
    }
    if (const char* s = json_str(device, "hostname"); s && s[0]) {
        g_wifi.hostname = s;
    }
    if (const cJSON* v = cJSON_GetObjectItem(device, "wifi_use_static"); cJSON_IsBool(v)) {
        g_wifi.use_static = cJSON_IsTrue(v);
    }
    if (const char* s = json_str(device, "wifi_ip"); s) {
        g_wifi.ip = s;
    }
    if (const char* s = json_str(device, "wifi_gateway"); s) {
        g_wifi.gateway = s;
    }
    if (const char* s = json_str(device, "wifi_netmask"); s && s[0]) {
        g_wifi.netmask = s;
    }
    if (const char* s = json_str(device, "wifi_dns1"); s) {
        g_wifi.dns1 = s;
    }
    if (const char* s = json_str(device, "wifi_dns2"); s) {
        g_wifi.dns2 = s;
    }
    if (clear_ip) {
        g_wifi.use_static = false;
        g_wifi.ip.clear();
    }
    cJSON_Delete(json);
    save_wifi_prefs();
    xTaskCreate(&reboot_task, "reboot", 2048, nullptr, 5, nullptr);
    return send_text(req, "{\"ok\":true,\"reboot\":true}", "application/json");
}

esp_err_t http_404(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
}

void start_http() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 28;
    config.max_open_sockets = 7;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        log.error("httpd_start failed");
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index, .user_ctx = nullptr},
        {.uri = "/api", .method = HTTP_GET, .handler = get_api, .user_ctx = nullptr},
        {.uri = "/save", .method = HTTP_POST, .handler = post_save, .user_ctx = nullptr},
        {.uri = "/test", .method = HTTP_POST, .handler = post_test, .user_ctx = nullptr},
        {.uri = "/discover", .method = HTTP_POST, .handler = post_discover, .user_ctx = nullptr},
        {.uri = "/pair", .method = HTTP_POST, .handler = post_pair, .user_ctx = nullptr},
        {.uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = get_wifi_scan, .user_ctx = nullptr},
        {.uri = "/api/v1/wifi", .method = HTTP_POST, .handler = post_wifi, .user_ctx = nullptr},
        {.uri = "/api/v1/wifi/retry", .method = HTTP_POST, .handler = post_wifi_retry, .user_ctx = nullptr},
        {.uri = "/api/v1/wifi/presence",
         .method = HTTP_POST,
         .handler = post_wifi_presence,
         .user_ctx = nullptr},
        {.uri = "/api/v1/config", .method = HTTP_GET, .handler = get_config, .user_ctx = nullptr},
        {.uri = "/api/v1/config", .method = HTTP_POST, .handler = post_config, .user_ctx = nullptr},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = get_android_probe, .user_ctx = nullptr},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = get_apple_probe, .user_ctx = nullptr},
        {.uri = "/canonical.html", .method = HTTP_GET, .handler = get_apple_probe, .user_ctx = nullptr},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = get_windows_probe, .user_ctx = nullptr},
    };
    for (const auto& r : routes) {
        httpd_register_uri_handler(server, &r);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404);
    if (!g_ota.start(server)) {
        log.error("OTA route failed");
    }
    log.info("HTTP on :80");
}

void start_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    load_wifi_prefs();
    apply_hostname();
    if (!g_wifi.ssid.empty()) {
        connect_sta();
    } else {
        start_ap_portal();
    }
}

} // namespace

extern "C" void app_main() {
    log.info("Booting doorbell transmitter (ESP32, no LEDs)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    static lumos::DoorbellTransmitter tx;
    tx.load_nvs();
    start_wifi();
    g_tx = &tx;
    start_http();
    auto started = tx.start();
    if (!started) {
        log.error("transmitter start failed: %s", started.error().message.c_str());
    }
    if (!g_sta_ip.empty()) {
        tx.set_sta_linked(true);
    }
    log.info("ready  this_mac=%s  setup=http://192.168.4.1/  lan=http://lumosos-bell.local",
             tx.own_mac().c_str());
}
