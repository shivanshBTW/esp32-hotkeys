#include "hotkeys_service.hpp"
#include "lumos/core/logger.hpp"
#include "lumos/core/result.hpp"
#include "lumos/doorbell/doorbell_mac.hpp"
#include "lumos/doorbell/doorbell_receiver.hpp"
#include "lumos/ota/ota_service.hpp"
#include "lumos/preferences/preferences.hpp"
#include "lumos/webui/web_ui.hpp"
#include "lumos/wifi/neighbor_info.hpp"
#include "lumos/wifi/wifi_service.hpp"

#include <cJSON.h>

#include "esp_http_server.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

lumos::Logger log{"hotkeys_main"};

inline const char* kAppName = "Hotkeys";
inline const char* kAppVersion = "0.1.0";
inline const char* kApiVersion = "0.3";

struct State {
    lumos::Preferences* preferences{nullptr};
    lumos::WifiService* wifi{nullptr};
    lumos::DoorbellReceiver* doorbell{nullptr};
};

State* g_state = nullptr;

static httpd_handle_t start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 48;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        log.error("Failed to start HTTP server");
        return nullptr;
    }
    return server;
}

static const char* http_status_phrase(int status) {
    switch (status) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 404:
        return "404 Not Found";
    default:
        return "500 Internal Server Error";
    }
}

static State* from_req(httpd_req_t* req) {
    if (g_state != nullptr && g_state->wifi != nullptr) {
        g_state->wifi->note_ui_activity();
    }
    (void)req;
    return g_state;
}

static esp_err_t send_json(httpd_req_t* req, const char* json, int status = 200) {
    httpd_resp_set_status(req, http_status_phrase(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_printed(httpd_req_t* req, cJSON* root, int status = 200) {
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const esp_err_t err = send_json(req, printed ? printed : "{}", printed ? status : 500);
    if (printed) {
        cJSON_free(printed);
    }
    return err;
}

static esp_err_t read_body(httpd_req_t* req, std::string& out) {
    const int total = req->content_len;
    if (total <= 0 || total > 16384) {
        out.clear();
        return ESP_OK;
    }
    out.resize(static_cast<std::size_t>(total));
    int received = 0;
    while (received < total) {
        const int r = httpd_req_recv(req, out.data() + received, total - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    return ESP_OK;
}

static const cJSON* json_get(const cJSON* obj, const char* key) {
    if (obj == nullptr) {
        return nullptr;
    }
    return cJSON_GetObjectItemCaseSensitive(const_cast<cJSON*>(obj), key);
}

static bool json_get_bool(const cJSON* obj, const char* key, bool default_value) {
    const auto* v = json_get(obj, key);
    if (v == nullptr || !cJSON_IsBool(v)) {
        return default_value;
    }
    return cJSON_IsTrue(v);
}

static std::string json_get_str(const cJSON* obj, const char* key, const char* def = "") {
    const auto* v = json_get(obj, key);
    if (v == nullptr || !cJSON_IsString(v) || v->valuestring == nullptr) {
        return def ? std::string(def) : std::string();
    }
    return v->valuestring ? std::string(v->valuestring) : std::string();
}

static int json_get_int(const cJSON* obj, const char* key, int def) {
    const auto* v = json_get(obj, key);
    if (v == nullptr || !cJSON_IsNumber(v)) {
        return def;
    }
    return v->valueint;
}

// --- REST handlers (same shapes as LumosOS for Wi-Fi / OTA / doorbell / config) ---

static cJSON* wifi_status_json(const lumos::WifiStatus& w) {
    cJSON* wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", w.connected);
    cJSON_AddBoolToObject(wifi, "use_static", w.use_static);
    cJSON_AddStringToObject(wifi, "ip", w.ip.c_str());
    cJSON_AddStringToObject(wifi, "gateway", w.gateway.c_str());
    cJSON_AddStringToObject(wifi, "netmask", w.netmask.c_str());
    cJSON_AddStringToObject(wifi, "dns1", w.dns1.c_str());
    cJSON_AddStringToObject(wifi, "dns2", w.dns2.c_str());
    cJSON_AddStringToObject(wifi, "ssid", w.ssid.c_str());
    cJSON_AddStringToObject(wifi, "mac", w.mac.c_str());
    cJSON_AddNumberToObject(wifi, "mode", static_cast<int>(w.mode));
    cJSON_AddBoolToObject(wifi, "has_saved_wifi", w.has_saved_wifi);
    cJSON_AddBoolToObject(wifi, "setup_mode", w.setup_mode);
    cJSON_AddBoolToObject(wifi, "auto_retry", w.auto_retry);
    return wifi;
}

static cJSON* device_settings_json(const lumos::DeviceSettings& d, bool include_secrets) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", d.hostname.c_str());
    cJSON_AddStringToObject(root, "wifi_ssid", d.wifi_ssid.c_str());
    if (include_secrets) {
        cJSON_AddStringToObject(root, "wifi_password", d.wifi_password.c_str());
    }
    cJSON_AddBoolToObject(root, "wifi_use_static", d.wifi_use_static);
    cJSON_AddStringToObject(root, "wifi_ip", d.wifi_ip.c_str());
    cJSON_AddStringToObject(root, "wifi_gateway", d.wifi_gateway.c_str());
    cJSON_AddStringToObject(root, "wifi_netmask", d.wifi_netmask.c_str());
    cJSON_AddStringToObject(root, "wifi_dns1", d.wifi_dns1.c_str());
    cJSON_AddStringToObject(root, "wifi_dns2", d.wifi_dns2.c_str());
    cJSON* db = cJSON_AddObjectToObject(root, "doorbell");
    cJSON_AddBoolToObject(db, "enabled", d.doorbell.enabled);
    cJSON_AddNumberToObject(db, "relay_pin", d.doorbell.relay_pin);
    cJSON_AddBoolToObject(db, "active_high", d.doorbell.active_high);
    cJSON_AddBoolToObject(db, "tone", d.doorbell.tone);
    cJSON_AddNumberToObject(db, "press_ms", d.doorbell.press_ms);
    cJSON_AddStringToObject(db, "paired_tx_mac", d.doorbell.paired_tx_mac.c_str());
    return root;
}

static esp_err_t get_status(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st || !st->wifi || !st->doorbell) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    const auto w = st->wifi->status();
    const auto d = st->doorbell->status();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", kAppName);
    cJSON_AddStringToObject(root, "version", kAppVersion);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddItemToObject(root, "wifi", wifi_status_json(w));

    cJSON* doorbell = cJSON_AddObjectToObject(root, "doorbell");
    cJSON_AddBoolToObject(doorbell, "enabled", d.enabled);
    cJSON_AddBoolToObject(doorbell, "espnow_ready", d.espnow_ready);
    cJSON_AddBoolToObject(doorbell, "paired", d.paired);
    cJSON_AddBoolToObject(doorbell, "pairing", d.pairing);
    cJSON_AddStringToObject(doorbell, "paired_tx_mac", d.paired_tx_mac.c_str());
    cJSON_AddStringToObject(doorbell, "own_mac", d.own_mac.c_str());
    cJSON_AddNumberToObject(doorbell, "relay_pin", d.relay_pin);
    cJSON_AddNumberToObject(doorbell, "last_ring_ms", d.last_ring_ms);

    cJSON* hotkeys = cJSON_AddObjectToObject(root, "hotkeys");
    cJSON_AddBoolToObject(hotkeys, "enabled", false);
    return send_printed(req, root);
}

static esp_err_t get_neighbors(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    const bool refresh = std::strstr(req->uri, "refresh=1") != nullptr;
    const auto json = lumos::neighbors_to_json(st->wifi->neighbors(refresh));
    return send_json(req, json.c_str());
}

static esp_err_t get_settings(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    return send_printed(req, device_settings_json(st->preferences->device(), false));
}

static void set_wifi_fields(lumos::DeviceSettings& d, const cJSON* device_obj);
static void set_doorbell_fields(lumos::DoorbellSettings& db, const cJSON* doorbell_obj);

static esp_err_t post_settings(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad body\"}", 400);
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_json(req, "{\"error\":\"invalid json\"}", 400);
    }
    auto& d = st->preferences->device();
    bool hostname_changed = false;
    if (const cJSON* hv = json_get(json, "hostname"); cJSON_IsString(hv) && hv->valuestring) {
        hostname_changed = d.hostname != hv->valuestring;
        d.hostname = hv->valuestring;
    }
    set_wifi_fields(d, json);
    const bool doorbell_touched = cJSON_IsObject(cJSON_GetObjectItem(json, "doorbell"));
    set_doorbell_fields(d.doorbell, json_get(json, "doorbell"));
    st->preferences->save();
    if (doorbell_touched) {
        st->doorbell->apply_settings();
    }
    if (hostname_changed) {
        st->wifi->apply_hostname();
        st->wifi->start_mdns();
    }
    cJSON_Delete(json);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t get_wifi(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    return send_printed(req, wifi_status_json(st->wifi->status()));
}

static esp_err_t post_wifi(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad body\"}", 400);
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_json(req, "{\"error\":\"invalid json\"}", 400);
    }
    auto& d = st->preferences->device();
    if (cJSON_IsTrue(cJSON_GetObjectItem(json, "forget"))) {
        cJSON_Delete(json);
        auto result = st->wifi->forget_wifi();
        if (!result) {
            return send_json(req, "{\"error\":\"forget failed\"}", 400);
        }
        return send_json(req, "{\"ok\":true}");
    }
    const cJSON* ssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON* pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(json);
        return send_json(req, "{\"error\":\"ssid required\"}", 400);
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "use_static"); cJSON_IsBool(v)) {
        d.wifi_use_static = cJSON_IsTrue(v);
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "ip"); cJSON_IsString(v)) {
        d.wifi_ip = v->valuestring;
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "gateway"); cJSON_IsString(v)) {
        d.wifi_gateway = v->valuestring;
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "netmask"); cJSON_IsString(v)) {
        d.wifi_netmask = v->valuestring;
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "dns1"); cJSON_IsString(v)) {
        d.wifi_dns1 = v->valuestring;
    }
    if (const cJSON* v = cJSON_GetObjectItem(json, "dns2"); cJSON_IsString(v)) {
        d.wifi_dns2 = v->valuestring;
    }
    if (d.wifi_use_static && (d.wifi_ip.empty() || d.wifi_gateway.empty())) {
        cJSON_Delete(json);
        return send_json(req, "{\"error\":\"static IP requires ip and gateway\"}", 400);
    }
    const char* password = cJSON_IsString(pass) ? pass->valuestring : "";
    auto result = st->wifi->connect_sta(ssid->valuestring, password);
    cJSON_Delete(json);
    if (!result) {
        return send_json(req, "{\"error\":\"connect failed\"}", 400);
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t get_wifi_scan(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::vector<lumos::WifiNetwork> nets;
    auto res = st->wifi->scan();
    if (!res) {
        return send_json(req, "{\"error\":\"scan failed\"}", 400);
    }
    nets = res.value();

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "networks");
    for (const auto& n : nets) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(it, "rssi", n.rssi);
        cJSON_AddNumberToObject(it, "channel", n.channel);
        cJSON_AddBoolToObject(it, "secure", n.secure);
        cJSON_AddItemToArray(arr, it);
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const esp_err_t err = send_json(req, printed ? printed : "{}", printed ? 200 : 500);
    if (printed) {
        cJSON_free(printed);
    }
    return err;
}

static esp_err_t post_wifi_retry(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    auto result = st->wifi->retry_saved();
    if (!result) {
        return send_json(req, "{\"error\":\"no saved wifi\"}", 400);
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t post_wifi_presence(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    st->wifi->note_ui_activity();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t get_doorbell(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    const auto d = st->doorbell->status();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", d.enabled);
    cJSON_AddBoolToObject(root, "espnow_ready", d.espnow_ready);
    cJSON_AddBoolToObject(root, "paired", d.paired);
    cJSON_AddBoolToObject(root, "relay_active", d.relay_active);
    cJSON_AddBoolToObject(root, "pairing", d.pairing);
    cJSON_AddNumberToObject(root, "press_ms", d.press_ms);
    cJSON_AddNumberToObject(root, "relay_pin", d.relay_pin);
    cJSON_AddBoolToObject(root, "active_high", d.active_high);
    cJSON_AddBoolToObject(root, "tone", d.tone);
    cJSON_AddStringToObject(root, "paired_tx_mac", d.paired_tx_mac.c_str());
    cJSON_AddStringToObject(root, "own_mac", d.own_mac.c_str());
    cJSON_AddNumberToObject(root, "wifi_channel", d.wifi_channel);
    cJSON_AddNumberToObject(root, "last_ring_ms", d.last_ring_ms);
    cJSON_AddNumberToObject(root, "last_seq", d.last_seq);
    cJSON_AddNumberToObject(root, "pairing_ms", d.pairing_ms);

    cJSON* peers = cJSON_AddArrayToObject(root, "peers");
    for (int i = 0; i < d.peer_count; ++i) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "mac", lumos::format_mac(d.peers[i].mac).c_str());
        cJSON_AddStringToObject(p, "name", d.peers[i].name);
        cJSON_AddNumberToObject(p, "channel", d.peers[i].channel);
        cJSON_AddNumberToObject(p, "rssi", static_cast<int>(d.peers[i].rssi));
        cJSON_AddItemToArray(peers, p);
    }

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const esp_err_t err = send_json(req, printed ? printed : "{}", printed ? 200 : 500);
    if (printed) {
        cJSON_free(printed);
    }
    return err;
}

static esp_err_t post_doorbell_pair_start(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    st->doorbell->start_pairing();
    return send_json(req, "{\"ok\":true,\"pairing\":true}");
}

static esp_err_t post_doorbell_test(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    // Finish the HTTP response first. Driving the relay can brown out the
    // chip; if we pulse first the browser just sees a dropped connection.
    const esp_err_t sent = send_json(req, "{\"ok\":true}");
    st->doorbell->test_pulse();
    return sent;
}

static esp_err_t post_doorbell_pair(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad body\"}", 400);
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_json(req, "{\"error\":\"invalid json\"}", 400);
    }
    const std::string mac = json_get_str(json, "mac", "");
    std::uint8_t parsed[6]{};
    bool ok = lumos::parse_mac(mac, parsed);
    cJSON_Delete(json);
    if (!ok) {
        return send_json(req, "{\"error\":\"mac invalid\"}", 400);
    }
    if (!st->doorbell->select_peer(parsed)) {
        return send_json(req, "{\"error\":\"pair failed\"}", 400);
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t post_doorbell(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad body\"}", 400);
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_json(req, "{\"error\":\"invalid json\"}", 400);
    }

    const cJSON* root = json;
    const cJSON* db = json_get(root, "doorbell");
    const cJSON* obj = db && cJSON_IsObject(const_cast<cJSON*>(db)) ? db : root;
    if (!cJSON_IsObject(const_cast<cJSON*>(obj))) {
        cJSON_Delete(json);
        return send_json(req, "{\"error\":\"missing doorbell object\"}", 400);
    }

    auto& d = st->preferences->device().doorbell;
    d.enabled = json_get_bool(obj, "enabled", d.enabled);
    d.relay_pin = json_get_int(obj, "relay_pin", d.relay_pin);
    d.active_high = json_get_bool(obj, "active_high", d.active_high);
    d.tone = json_get_bool(obj, "tone", d.tone);
    d.press_ms = static_cast<std::uint16_t>(json_get_int(obj, "press_ms", d.press_ms));
    d.paired_tx_mac = json_get_str(obj, "paired_tx_mac", d.paired_tx_mac.c_str());

    st->preferences->save();
    st->doorbell->apply_settings();
    cJSON_Delete(json);
    return send_json(req, "{\"ok\":true}");
}

static void set_wifi_fields(lumos::DeviceSettings& d, const cJSON* device_obj) {
    if (!device_obj || !cJSON_IsObject(const_cast<cJSON*>(device_obj))) {
        return;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_ssid"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_ssid = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_password"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_password = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_use_static"); cJSON_IsBool(v)) {
        d.wifi_use_static = cJSON_IsTrue(v);
    }
    if (const cJSON* v = json_get(device_obj, "wifi_ip"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_ip = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_gateway"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_gateway = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_netmask"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_netmask = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_dns1"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_dns1 = v->valuestring;
    }
    if (const cJSON* v = json_get(device_obj, "wifi_dns2"); cJSON_IsString(v) && v->valuestring) {
        d.wifi_dns2 = v->valuestring;
    }
}

static void set_doorbell_fields(lumos::DoorbellSettings& db, const cJSON* doorbell_obj) {
    if (!doorbell_obj || !cJSON_IsObject(const_cast<cJSON*>(doorbell_obj))) {
        return;
    }
    if (const cJSON* v = json_get(doorbell_obj, "enabled"); cJSON_IsBool(v)) {
        db.enabled = cJSON_IsTrue(v);
    }
    if (const cJSON* v = json_get(doorbell_obj, "relay_pin"); cJSON_IsNumber(v)) {
        db.relay_pin = v->valueint;
    }
    if (const cJSON* v = json_get(doorbell_obj, "active_high"); cJSON_IsBool(v)) {
        db.active_high = cJSON_IsTrue(v);
    }
    if (const cJSON* v = json_get(doorbell_obj, "tone"); cJSON_IsBool(v)) {
        db.tone = cJSON_IsTrue(v);
    }
    if (const cJSON* v = json_get(doorbell_obj, "press_ms"); cJSON_IsNumber(v)) {
        db.press_ms = static_cast<std::uint16_t>(v->valueint);
    }
    if (const cJSON* v = json_get(doorbell_obj, "paired_tx_mac"); cJSON_IsString(v) && v->valuestring) {
        db.paired_tx_mac = v->valuestring;
    }
}

static esp_err_t get_config(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    bool include_secrets = false;
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "secrets", val, sizeof(val)) == ESP_OK) {
            include_secrets = (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0);
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "hotkeys.config.v1");
    cJSON_AddStringToObject(root, "app", kAppName);
    cJSON_AddStringToObject(root, "version", kAppVersion);
    cJSON_AddStringToObject(root, "api", kApiVersion);
    cJSON_AddItemToObject(root, "device",
                          device_settings_json(st->preferences->device(), include_secrets));
    cJSON* hotkeys = cJSON_AddObjectToObject(root, "hotkeys");
    cJSON_AddBoolToObject(hotkeys, "enabled", false);

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"hotkeys-config.json\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const esp_err_t err =
        httpd_resp_send(req, printed, printed != nullptr ? HTTPD_RESP_USE_STRLEN : 0);
    if (printed) {
        cJSON_free(printed);
    }
    return err;
}

static esp_err_t post_config(httpd_req_t* req) {
    auto* st = from_req(req);
    if (!st) {
        return send_json(req, "{\"error\":\"not ready\"}", 500);
    }
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        return send_json(req, "{\"error\":\"bad body\"}", 400);
    }
    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return send_json(req, "{\"error\":\"invalid json\"}", 400);
    }

    cJSON* device_obj = cJSON_GetObjectItem(json, "device");
    if (!cJSON_IsObject(device_obj)) {
        if (cJSON_GetObjectItem(json, "wifi_ssid") != nullptr ||
            cJSON_GetObjectItem(json, "hostname") != nullptr) {
            device_obj = json;
        } else {
            cJSON_Delete(json);
            return send_json(req, "{\"error\":\"missing device object\"}", 400);
        }
    }

    const cJSON* schema = cJSON_GetObjectItem(json, "schema");
    if (cJSON_IsString(schema) && std::strcmp(schema->valuestring, "hotkeys.config.v1") != 0 &&
        std::strcmp(schema->valuestring, "lumosos.config.v1") != 0) {
        cJSON_Delete(json);
        return send_json(req, "{\"error\":\"unsupported config schema\"}", 400);
    }

    auto& d = st->preferences->device();
    bool hostname_changed = false;
    if (const cJSON* hv = json_get(device_obj, "hostname"); cJSON_IsString(hv) && hv->valuestring) {
        hostname_changed = d.hostname != hv->valuestring;
        d.hostname = hv->valuestring;
    }
    set_wifi_fields(d, device_obj);
    if (const cJSON* v = cJSON_GetObjectItem(json, "clear_static_ip"); cJSON_IsBool(v) &&
        cJSON_IsTrue(v)) {
        d.wifi_use_static = false;
        d.wifi_ip.clear();
    }
    const bool doorbell_touched = cJSON_IsObject(cJSON_GetObjectItem(device_obj, "doorbell"));
    set_doorbell_fields(d.doorbell, json_get(device_obj, "doorbell"));

    st->preferences->save();
    if (doorbell_touched) {
        st->doorbell->apply_settings();
    }
    if (hostname_changed) {
        st->wifi->apply_hostname();
        st->wifi->start_mdns();
    }
    cJSON_Delete(json);
    return send_json(req, "{\"ok\":true,\"reboot\":false}");
}

} // namespace

extern "C" void app_main() {
    log.info("Booting hotkeys app");

    auto preferences = std::unique_ptr<lumos::Preferences>(new lumos::Preferences());
    if (!preferences->init()) {
        log.error("Preferences init failed");
        return;
    }

    auto& device = preferences->device();
    (void)device; // unused for now

    auto doorbell = std::unique_ptr<lumos::DoorbellReceiver>(new lumos::DoorbellReceiver(*preferences));

    auto wifi = std::unique_ptr<lumos::WifiService>(new lumos::WifiService(*preferences));
    if (!wifi->start()) {
        log.warn("WiFi start failed (device may still be usable in AP setup)");
    }

    if (!doorbell->start()) {
        log.warn("Doorbell receiver failed to start (hotkeys firmware continues)");
    }

    hotkeys::HotkeysService hotkeys_stub;
    hotkeys_stub.start();

    httpd_handle_t server = start_http_server();
    if (server == nullptr) {
        return;
    }

    static State s_state;
    s_state.preferences = preferences.get();
    s_state.wifi = wifi.get();
    s_state.doorbell = doorbell.get();
    g_state = &s_state;

    auto ota = std::unique_ptr<lumos::OtaService>(new lumos::OtaService());
    (void)ota->start(server);

    auto webui = std::unique_ptr<lumos::WebUi>(new lumos::WebUi());
    if (!webui->start(server)) {
        log.error("Web UI failed to start");
    }

    httpd_uri_t uri{};

    uri = {.uri = "/api/v1/status",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_status(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/settings",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_settings(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/settings",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_settings(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/wifi",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_wifi(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/wifi",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_wifi(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/wifi/scan",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_wifi_scan(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/wifi/retry",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_wifi_retry(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/wifi/presence",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_wifi_presence(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/neighbors",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_neighbors(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/doorbell",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_doorbell(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/doorbell",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_doorbell(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/doorbell/test",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_doorbell_test(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/doorbell/pair/start",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_doorbell_pair_start(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/doorbell/pair",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_doorbell_pair(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/config",
           .method = HTTP_GET,
           .handler = [](httpd_req_t* r) -> esp_err_t { return get_config(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    uri = {.uri = "/api/v1/config",
           .method = HTTP_POST,
           .handler = [](httpd_req_t* r) -> esp_err_t { return post_config(r); },
           .user_ctx = nullptr};
    (void)httpd_register_uri_handler(server, &uri);

    // Keep services alive.
    static auto s_preferences = std::move(preferences);
    static auto s_wifi = std::move(wifi);
    static auto s_doorbell = std::move(doorbell);
    static auto s_ota = std::move(ota);
    static auto s_webui = std::move(webui);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

