#include <Arduino.h>

#include "lumos/core/logger.hpp"
#include "lumos/doorbell/doorbell_mac.hpp"
#include "lumos/doorbell/doorbell_platform.hpp"
#include "lumos/doorbell/doorbell_transmitter.hpp"
#include "lumos/doorbell/doorbell_tx_form.hpp"
#include "lumos/doorbell/doorbell_tx_prefs.hpp"
#include "lumos/doorbell/doorbell_tx_ui.hpp"

#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Ticker.h>
#include <Updater.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace {

lumos::Logger g_log{"doorbell_tx_main"};
lumos::DoorbellTransmitter* g_tx{nullptr};
lumos::DoorbellTransmitter g_tx_obj;
ESP8266WebServer server(80);
Ticker g_retry_ticker;

bool g_want_sta{false};
bool g_setup_ap{false};
bool g_sta_up{false};
bool g_retry_in_flight{false};
int g_sta_fails{0};
std::int64_t g_last_ui_ms{0};
std::string g_sta_ip;
int g_sta_rssi{0};
lumos::WifiPrefs g_wifi;
WiFiEventHandler g_got_ip;
WiFiEventHandler g_disc;
bool g_mdns_started{false};

constexpr int kEepromSize = 512;
constexpr int kWifiOff = 128;

struct WifiBlob {
    char magic[4];
    std::uint8_t ver;
    char ssid[33];
    char pass[65];
    char host[33];
    std::uint8_t use_static;
    char ip[16];
    char gw[16];
    char mask[16];
    char dns1[16];
    char dns2[16];
};

static const char* const kIndexHtml = LUMOS_DOORBELL_TX_INDEX_HTML;

void note_ui_activity() {
    g_last_ui_ms = static_cast<std::int64_t>(millis());
}

bool ui_session_active() {
    if (g_last_ui_ms <= 0) {
        return false;
    }
    return (static_cast<std::int64_t>(millis()) - g_last_ui_ms) < lumos::kDoorbellUiPresenceHoldMs;
}

bool has_saved_wifi() {
    return !g_wifi.ssid.empty();
}

void load_wifi_prefs() {
    WifiBlob blob{};
    EEPROM.get(kWifiOff, blob);
    if (std::memcmp(blob.magic, "DBWF", 4) != 0 || blob.ver != 1) {
        return;
    }
    g_wifi.ssid = blob.ssid;
    g_wifi.password = blob.pass;
    g_wifi.hostname = blob.host[0] ? blob.host : "LumosOS-Bell";
    g_wifi.use_static = blob.use_static != 0;
    g_wifi.ip = blob.ip;
    g_wifi.gateway = blob.gw;
    g_wifi.netmask = blob.mask[0] ? blob.mask : "255.255.255.0";
    g_wifi.dns1 = blob.dns1;
    g_wifi.dns2 = blob.dns2;
}

void save_wifi_prefs() {
    WifiBlob blob{};
    std::memcpy(blob.magic, "DBWF", 4);
    blob.ver = 1;
    std::strncpy(blob.ssid, g_wifi.ssid.c_str(), sizeof(blob.ssid) - 1);
    std::strncpy(blob.pass, g_wifi.password.c_str(), sizeof(blob.pass) - 1);
    std::strncpy(blob.host, g_wifi.hostname.c_str(), sizeof(blob.host) - 1);
    blob.use_static = g_wifi.use_static ? 1 : 0;
    std::strncpy(blob.ip, g_wifi.ip.c_str(), sizeof(blob.ip) - 1);
    std::strncpy(blob.gw, g_wifi.gateway.c_str(), sizeof(blob.gw) - 1);
    std::strncpy(blob.mask, g_wifi.netmask.c_str(), sizeof(blob.mask) - 1);
    std::strncpy(blob.dns1, g_wifi.dns1.c_str(), sizeof(blob.dns1) - 1);
    std::strncpy(blob.dns2, g_wifi.dns2.c_str(), sizeof(blob.dns2) - 1);
    EEPROM.put(kWifiOff, blob);
    EEPROM.commit();
}

void apply_hostname() {
    if (g_wifi.hostname.empty()) {
        g_wifi.hostname = "LumosOS-Bell";
    }
    if (g_wifi.hostname.size() > 32) {
        g_wifi.hostname.resize(32);
    }
    WiFi.hostname(g_wifi.hostname.c_str());
}

void apply_sta_ip_config() {
    if (!g_wifi.use_static) {
        WiFi.config(0u, 0u, 0u);
        return;
    }
    if (g_wifi.ip.empty() || g_wifi.gateway.empty()) {
        return;
    }
    IPAddress ip, gw, mask, dns1, dns2;
    if (!ip.fromString(g_wifi.ip.c_str()) || !gw.fromString(g_wifi.gateway.c_str())) {
        return;
    }
    const std::string& mask_s = g_wifi.netmask.empty() ? "255.255.255.0" : g_wifi.netmask;
    if (!mask.fromString(mask_s.c_str())) {
        return;
    }
    const std::string& d1 = g_wifi.dns1.empty() ? g_wifi.gateway : g_wifi.dns1;
    dns1.fromString(d1.c_str());
    if (!g_wifi.dns2.empty()) {
        dns2.fromString(g_wifi.dns2.c_str());
        WiFi.config(ip, gw, mask, dns1, dns2);
    } else {
        WiFi.config(ip, gw, mask, dns1);
    }
    g_log.info("STA static %s gw %s", g_wifi.ip.c_str(), g_wifi.gateway.c_str());
}

void start_mdns() {
    const auto host = lumos::doorbell_mdns_label(g_wifi.hostname);
    if (g_mdns_started) {
        MDNS.end();
    }
    if (MDNS.begin(host.c_str())) {
        MDNS.addService("http", "tcp", 80);
        g_mdns_started = true;
        g_log.info("mDNS http://%s.local", host.c_str());
    } else {
        g_log.warn("mdns_init failed");
    }
}

void start_ap_portal();
void apply_sta_config_and_connect(bool keep_ap);
void begin_background_sta_attempt();

void stop_retry_timer() {
    g_retry_ticker.detach();
}

void arm_retry_timer() {
    stop_retry_timer();
    if (has_saved_wifi()) {
        g_retry_ticker.attach_ms(static_cast<uint32_t>(lumos::kDoorbellBackgroundRetryUs / 1000ULL),
                                 begin_background_sta_attempt);
    }
}

void apply_sta_config_and_connect(bool keep_ap) {
    apply_hostname();
    apply_sta_ip_config();
    if (keep_ap || g_setup_ap) {
        WiFi.mode(WIFI_AP_STA);
        g_retry_in_flight = true;
        g_want_sta = false;
    } else {
        WiFi.mode(WIFI_STA);
        g_want_sta = true;
        g_retry_in_flight = false;
    }
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.begin(g_wifi.ssid.c_str(), g_wifi.password.c_str());
    g_log.info("Connecting STA to %s%s", g_wifi.ssid.c_str(),
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
    g_log.info("Background STA retry %s", g_wifi.ssid.c_str());
    apply_sta_config_and_connect(true);
}

void drop_ap_to_sta() {
    g_setup_ap = false;
    g_retry_in_flight = false;
    g_want_sta = true;
    stop_retry_timer();
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
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
    const uint8_t ch = (g_tx != nullptr) ? g_tx->config().channel : 1;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(lumos::kDoorbellApSsid, nullptr, ch);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    g_log.info("SoftAP %s  %s%s", lumos::kDoorbellApSsid, lumos::kDoorbellApIp,
             has_saved_wifi() ? " (saved home Wi-Fi kept)" : "");
    arm_retry_timer();
}

void on_got_ip(const WiFiEventStationModeGotIP& ev) {
    g_sta_fails = 0;
    g_sta_up = true;
    g_retry_in_flight = false;
    g_sta_ip = ev.ip.toString().c_str();
    g_sta_rssi = WiFi.RSSI();
    g_log.info("Got IP %s", g_sta_ip.c_str());
    if (g_setup_ap) {
        drop_ap_to_sta();
    } else {
        g_want_sta = true;
        stop_retry_timer();
    }
    start_mdns();
    if (g_tx != nullptr) {
        g_tx->set_sta_linked(true);
    }
}

void on_disconnected(const WiFiEventStationModeDisconnected&) {
    g_log.warn("STA disconnected");
    g_sta_up = false;
    if (g_tx != nullptr) {
        g_tx->set_sta_linked(false);
    }
    g_sta_ip.clear();
    if (g_retry_in_flight) {
        g_retry_in_flight = false;
        g_log.info("STA retry missed — staying on setup AP");
        return;
    }
    if (!g_want_sta || g_setup_ap) {
        return;
    }
    ++g_sta_fails;
    if (g_sta_fails >= lumos::kDoorbellStaFailLimit) {
        g_log.warn("STA failed %d times — setup AP (password kept)", g_sta_fails);
        start_ap_portal();
    } else {
        g_log.info("STA retry %d/%d", g_sta_fails, lumos::kDoorbellStaFailLimit);
        WiFi.begin(g_wifi.ssid.c_str(), g_wifi.password.c_str());
    }
}

void reboot_soon() {
    delay(800);
    ESP.restart();
}

void send_text(int status, const char* body, const char* type = "text/plain") {
    server.send(status, type, body);
}

void send_json(int status, const std::string& body) {
    server.send(status, "application/json", body.c_str());
}

std::string read_body() {
    if (server.hasArg("plain")) {
        return server.arg("plain").c_str();
    }
    return {};
}

std::string arg_or_form(const char* key) {
    if (server.hasArg(key)) {
        return server.arg(key).c_str();
    }
    return lumos::doorbell_form_value(read_body(), key);
}

const char* json_cstr(JsonVariantConst v) {
    return v.is<const char*>() ? v.as<const char*>() : nullptr;
}

void handle_index() {
    note_ui_activity();
    server.send_P(200, "text/html", kIndexHtml);
}

void handle_api() {
    note_ui_activity();
    JsonDocument doc;
    if (g_tx != nullptr) {
        const auto st = g_tx->status();
        const auto& cfg = st.cfg;
        doc["board"] = "esp8266";
        doc["own_mac"] = st.own_mac;
        doc["rx_mac"] = cfg.rx_mac_valid ? lumos::format_mac(cfg.rx_mac) : "";
        doc["paired"] = st.paired;
        doc["channel"] = cfg.channel;
        doc["opto_pin"] = cfg.opto_pin;
        doc["opto_level"] = st.opto_level;
        doc["active_low"] = cfg.active_low;
        doc["espnow_ready"] = st.espnow_ready;
        doc["last_seq"] = st.last_seq;
        doc["last_send_ms"] = st.last_send_ms;
        doc["pairing"] = st.pairing;
        doc["scanning"] = st.scanning;
        doc["pairing_ms"] = st.pairing_ms;
        JsonArray peers = doc["peers"].to<JsonArray>();
        for (int i = 0; i < st.peer_count; ++i) {
            JsonObject p = peers.add<JsonObject>();
            p["mac"] = lumos::format_mac(st.peers[i].mac);
            p["name"] = st.peers[i].name;
            p["channel"] = st.peers[i].channel;
            p["rssi"] = st.peers[i].rssi;
        }
    }
    doc["wifi_ssid"] = g_wifi.ssid;
    doc["hostname"] = g_wifi.hostname;
    doc["wifi_use_static"] = g_wifi.use_static;
    doc["wifi_ip"] = g_wifi.ip;
    doc["wifi_gateway"] = g_wifi.gateway;
    doc["wifi_netmask"] = g_wifi.netmask;
    doc["wifi_dns1"] = g_wifi.dns1;
    doc["wifi_dns2"] = g_wifi.dns2;
    doc["wifi_connected"] = !g_sta_ip.empty();
    doc["sta_ip"] = g_sta_ip;
    const bool setup_mode = g_setup_ap && !g_sta_up;
    std::string live_gw;
    std::string live_mask;
    std::string live_dns;
    if (!g_sta_ip.empty()) {
        live_gw = WiFi.gatewayIP().toString().c_str();
        live_mask = WiFi.subnetMask().toString().c_str();
        live_dns = WiFi.dnsIP().toString().c_str();
    } else if (setup_mode) {
        live_gw = WiFi.softAPIP().toString().c_str();
        live_mask = "255.255.255.0";
    }
    doc["sta_gateway"] = live_gw;
    doc["sta_netmask"] = live_mask;
    doc["sta_dns"] = live_dns;
    doc["rssi"] = g_sta_rssi;
    doc["ap_ip"] = lumos::kDoorbellApIp;
    doc["sta_linked"] = g_tx != nullptr && g_tx->sta_linked();
    const bool saved = has_saved_wifi();
    doc["has_saved_wifi"] = saved;
    doc["setup_mode"] = setup_mode;
    doc["auto_retry"] = saved && setup_mode && !ui_session_active();
    std::string out;
    serializeJson(doc, out);
    send_json(200, out);
}

void handle_save() {
    note_ui_activity();
    if (g_tx == nullptr) {
        send_text(200, "not ready");
        return;
    }
    auto cfg = g_tx->config();
    const auto mac = arg_or_form("rx_mac");
    if (!mac.empty()) {
        cfg.rx_mac_valid = lumos::parse_mac(mac, cfg.rx_mac);
    }
    const auto ch = arg_or_form("channel");
    if (!ch.empty()) {
        cfg.channel = static_cast<std::uint8_t>(std::clamp(std::atoi(ch.c_str()), 1, 13));
    }
    const auto pin = arg_or_form("pin");
    if (!pin.empty()) {
        cfg.opto_pin = std::atoi(pin.c_str());
    }
    const auto al = arg_or_form("active_low");
    cfg.active_low = (al != "0" && al != "false");
    g_tx->apply_config(cfg);
    g_tx->save_nvs();
    send_text(200, "Saved.");
}

void handle_test() {
    note_ui_activity();
    if (g_tx == nullptr) {
        send_text(200, "not ready");
        return;
    }
    if (!g_tx->config().rx_mac_valid) {
        send_text(200, "Not paired — Find nearby, then pick the LED board.");
        return;
    }
    g_tx->test_send();
    const auto st = g_tx->status();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Sent press seq=%u to %s (broadcast too).",
                  static_cast<unsigned>(st.last_seq), lumos::format_mac(st.cfg.rx_mac).c_str());
    send_text(200, buf);
}

void handle_discover() {
    note_ui_activity();
    if (g_tx == nullptr) {
        send_text(200, "not ready");
        return;
    }
    g_tx->start_pairing();
    send_text(200, "Scanning nearby LumosOS receivers…");
}

void handle_pair() {
    note_ui_activity();
    if (g_tx == nullptr) {
        send_text(200, "not ready");
        return;
    }
    const auto mac = arg_or_form("rx_mac");
    std::uint8_t parsed[6]{};
    if (!lumos::parse_mac(mac, parsed)) {
        send_text(200, "mac required");
        return;
    }
    if (!g_tx->select_peer(parsed)) {
        send_text(200, "pair failed");
        return;
    }
    send_text(200, "Paired.");
}

void handle_wifi_scan() {
    note_ui_activity();
    if (WiFi.getMode() == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
    }
    const int n = WiFi.scanNetworks(false, true);
    JsonDocument doc;
    JsonArray nets = doc["networks"].to<JsonArray>();
    std::set<std::string> seen;
    const int count = n < 0 ? 0 : (n > 40 ? 40 : n);
    for (int i = 0; i < count; ++i) {
        const std::string name = WiFi.SSID(i).c_str();
        if (name.empty() || !seen.insert(name).second) {
            continue;
        }
        JsonObject o = nets.add<JsonObject>();
        o["ssid"] = name;
        o["rssi"] = WiFi.RSSI(i);
        o["channel"] = WiFi.channel(i);
        o["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
    }
    WiFi.scanDelete();
    std::string out;
    serializeJson(doc, out);
    send_json(200, out);
}

void handle_wifi_post() {
    note_ui_activity();
    const std::string body = read_body();
    JsonDocument json;
    if (deserializeJson(json, body)) {
        send_json(200, "{\"error\":\"invalid json\"}");
        return;
    }
    if (json["forget"].is<bool>() && json["forget"].as<bool>()) {
        g_wifi = lumos::WifiPrefs{};
        save_wifi_prefs();
        send_json(200, "{\"ok\":true,\"rebooting\":true}");
        reboot_soon();
        return;
    }
    if (const char* s = json_cstr(json["ssid"]); s && s[0]) {
        g_wifi.ssid = s;
    }
    if (const char* s = json_cstr(json["password"]); s && s[0]) {
        g_wifi.password = s;
    }
    if (const char* s = json_cstr(json["hostname"]); s && s[0]) {
        g_wifi.hostname = s;
    }
    if (json["use_static"].is<bool>()) {
        g_wifi.use_static = json["use_static"].as<bool>();
    }
    if (const char* s = json_cstr(json["ip"])) {
        g_wifi.ip = s;
    }
    if (const char* s = json_cstr(json["gateway"])) {
        g_wifi.gateway = s;
    }
    if (const char* s = json_cstr(json["netmask"]); s && s[0]) {
        g_wifi.netmask = s;
    }
    if (const char* s = json_cstr(json["dns1"])) {
        g_wifi.dns1 = s;
    }
    if (const char* s = json_cstr(json["dns2"])) {
        g_wifi.dns2 = s;
    }
    if (g_wifi.ssid.empty()) {
        send_json(200, "{\"error\":\"ssid required\"}");
        return;
    }
    save_wifi_prefs();
    connect_sta();
    send_json(200, "{\"ok\":true}");
}

void handle_wifi_retry() {
    note_ui_activity();
    if (!has_saved_wifi()) {
        send_json(400, "{\"error\":\"no saved wifi\"}");
        return;
    }
    retry_saved();
    send_json(200, "{\"ok\":true}");
}

void handle_wifi_presence() {
    note_ui_activity();
    send_json(200, "{\"ok\":true}");
}

void handle_config_get() {
    note_ui_activity();
    const bool secrets = server.uri().indexOf("secrets=1") >= 0;
    JsonDocument doc;
    doc["product"] = lumos::kDoorbellConfigProduct;
    JsonObject device = doc["device"].to<JsonObject>();
    if (g_tx != nullptr) {
        const auto cfg = g_tx->config();
        device["rx_mac"] = cfg.rx_mac_valid ? lumos::format_mac(cfg.rx_mac) : "";
        device["channel"] = cfg.channel;
        device["opto_pin"] = cfg.opto_pin;
        device["active_low"] = cfg.active_low;
        device["tx_id"] = cfg.tx_id;
    }
    device["wifi_ssid"] = g_wifi.ssid;
    device["hostname"] = g_wifi.hostname;
    if (secrets) {
        device["wifi_password"] = g_wifi.password;
    }
    device["wifi_use_static"] = g_wifi.use_static;
    device["wifi_ip"] = g_wifi.ip;
    device["wifi_gateway"] = g_wifi.gateway;
    device["wifi_netmask"] = g_wifi.netmask;
    device["wifi_dns1"] = g_wifi.dns1;
    device["wifi_dns2"] = g_wifi.dns2;
    std::string out;
    serializeJson(doc, out);
    send_json(200, out);
}

void handle_config_post() {
    note_ui_activity();
    const std::string body = read_body();
    JsonDocument json;
    if (deserializeJson(json, body)) {
        send_json(200, "{\"error\":\"invalid json\"}");
        return;
    }
    JsonVariantConst device = json.as<JsonVariantConst>();
    if (json["device"].is<JsonObject>()) {
        device = json["device"];
    }
    const bool clear_ip = json["clear_static_ip"].is<bool>() && json["clear_static_ip"].as<bool>();
    if (g_tx != nullptr) {
        auto cfg = g_tx->config();
        if (const char* s = json_cstr(device["rx_mac"])) {
            cfg.rx_mac_valid = lumos::parse_mac(s, cfg.rx_mac);
        }
        if (device["channel"].is<int>()) {
            cfg.channel = static_cast<std::uint8_t>(std::clamp(device["channel"].as<int>(), 1, 13));
        }
        if (device["opto_pin"].is<int>()) {
            cfg.opto_pin = device["opto_pin"].as<int>();
        }
        if (device["active_low"].is<bool>()) {
            cfg.active_low = device["active_low"].as<bool>();
        }
        g_tx->apply_config(cfg);
        g_tx->save_nvs();
    }
    if (const char* s = json_cstr(device["wifi_ssid"])) {
        g_wifi.ssid = s;
    }
    if (const char* s = json_cstr(device["wifi_password"])) {
        g_wifi.password = s;
    }
    if (const char* s = json_cstr(device["hostname"]); s && s[0]) {
        g_wifi.hostname = s;
    }
    if (device["wifi_use_static"].is<bool>()) {
        g_wifi.use_static = device["wifi_use_static"].as<bool>();
    }
    if (const char* s = json_cstr(device["wifi_ip"])) {
        g_wifi.ip = s;
    }
    if (const char* s = json_cstr(device["wifi_gateway"])) {
        g_wifi.gateway = s;
    }
    if (const char* s = json_cstr(device["wifi_netmask"])) {
        g_wifi.netmask = s;
    }
    if (const char* s = json_cstr(device["wifi_dns1"])) {
        g_wifi.dns1 = s;
    }
    if (const char* s = json_cstr(device["wifi_dns2"])) {
        g_wifi.dns2 = s;
    }
    if (clear_ip) {
        g_wifi.use_static = false;
        g_wifi.ip.clear();
    }
    save_wifi_prefs();
    send_json(200, "{\"ok\":true,\"reboot\":true}");
    reboot_soon();
}

void handle_ota_done() {
    if (Update.hasError()) {
        server.send(500, "text/plain", "ota failed");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(500);
    ESP.restart();
}

void handle_ota_upload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        const size_t size = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(size)) {
            g_log.error("ota begin failed");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            g_log.error("ota write failed");
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
            g_log.error("ota end failed");
        }
    }
}

void start_http() {
    server.on("/", HTTP_GET, handle_index);
    server.on("/api", HTTP_GET, handle_api);
    server.on("/save", HTTP_POST, handle_save);
    server.on("/test", HTTP_POST, handle_test);
    server.on("/discover", HTTP_POST, handle_discover);
    server.on("/pair", HTTP_POST, handle_pair);
    server.on("/api/v1/wifi/scan", HTTP_GET, handle_wifi_scan);
    server.on("/api/v1/wifi", HTTP_POST, handle_wifi_post);
    server.on("/api/v1/wifi/retry", HTTP_POST, handle_wifi_retry);
    server.on("/api/v1/wifi/presence", HTTP_POST, handle_wifi_presence);
    server.on("/api/v1/config", HTTP_GET, handle_config_get);
    server.on("/api/v1/config", HTTP_POST, handle_config_post);
    server.on("/generate_204", HTTP_GET, []() { server.send(204, "text/plain", ""); });
    server.on("/hotspot-detect.html", HTTP_GET,
              []() { server.send(200, "text/html", lumos::kDoorbellAppleCaptiveHtml); });
    server.on("/canonical.html", HTTP_GET,
              []() { server.send(200, "text/html", lumos::kDoorbellAppleCaptiveHtml); });
    server.on("/ncsi.txt", HTTP_GET,
              []() { server.send(200, "text/plain", lumos::kDoorbellWindowsNcsi); });
    server.on("/api/v1/ota", HTTP_POST, handle_ota_done, handle_ota_upload);
    server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
    server.begin();
    g_log.info("HTTP on :80");
}

void start_wifi() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    load_wifi_prefs();
    apply_hostname();
    if (!g_wifi.ssid.empty()) {
        connect_sta();
    } else {
        start_ap_portal();
    }
}

void pump() {
    server.handleClient();
    if (g_mdns_started) {
        MDNS.update();
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(50);
    g_log.info("Booting doorbell transmitter (ESP8266, no LEDs)");
    EEPROM.begin(kEepromSize);
    WiFi.persistent(false);
    g_got_ip = WiFi.onStationModeGotIP(on_got_ip);
    g_disc = WiFi.onStationModeDisconnected(on_disconnected);

    g_tx_obj.load_nvs();
    start_wifi();
    g_tx = &g_tx_obj;
    lumos::dbplat::set_pump(&pump);
    start_http();
    auto started = g_tx_obj.start();
    if (!started) {
        g_log.error("transmitter start failed: %s", started.error().message.c_str());
    }
    if (!g_sta_ip.empty()) {
        g_tx_obj.set_sta_linked(true);
    }
    g_log.info("ready  this_mac=%s  setup=http://192.168.4.1/  lan=http://lumosos-bell.local",
             g_tx_obj.own_mac().c_str());
}

void loop() {
    pump();
    if (g_tx != nullptr) {
        g_tx->poll();
    }
}
