#include "lumos/hotkeys/hotkeys_service.hpp"
#include "lumos/core/logger.hpp"

#include "esp_http_client.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>

namespace lumos {
namespace {

Logger log{"hotkeys"};

void clamp_str(std::string& s, std::size_t max) {
    if (s.size() > max) {
        s.resize(max);
    }
}

void upper_method(std::string& m) {
    for (char& c : m) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
}

bool valid_method(const std::string& m) {
    return m == "GET" || m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE";
}

esp_http_client_method_t to_esp_method(const std::string& m) {
    if (m == "GET") {
        return HTTP_METHOD_GET;
    }
    if (m == "PUT") {
        return HTTP_METHOD_PUT;
    }
    if (m == "PATCH") {
        return HTTP_METHOD_PATCH;
    }
    if (m == "DELETE") {
        return HTTP_METHOD_DELETE;
    }
    return HTTP_METHOD_POST;
}

std::string trim_slash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

void trim_token(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) {
        s.pop_back();
    }
    if (s.size() >= 7 && (s.compare(0, 7, "Bearer ") == 0 || s.compare(0, 7, "bearer ") == 0)) {
        s.erase(0, 7);
    }
}

const cJSON* jget(const cJSON* obj, const char* key) {
    if (obj == nullptr) {
        return nullptr;
    }
    return cJSON_GetObjectItemCaseSensitive(const_cast<cJSON*>(obj), key);
}

void set_action_from_json(HotkeyAction& a, const cJSON* obj) {
    if (!cJSON_IsObject(const_cast<cJSON*>(obj))) {
        return;
    }
    if (const cJSON* v = jget(obj, "name"); cJSON_IsString(v) && v->valuestring) {
        a.name = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "type"); cJSON_IsString(v) && v->valuestring) {
        a.type = hotkey_type_from_str(v->valuestring);
    }
    if (const cJSON* v = jget(obj, "method"); cJSON_IsString(v) && v->valuestring) {
        a.method = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "url"); cJSON_IsString(v) && v->valuestring) {
        a.url = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "body"); cJSON_IsString(v) && v->valuestring) {
        a.body = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "service"); cJSON_IsString(v) && v->valuestring) {
        a.service = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "entity_id"); cJSON_IsString(v) && v->valuestring) {
        a.entity_id = v->valuestring;
    }
    if (const cJSON* v = jget(obj, "data"); cJSON_IsString(v) && v->valuestring) {
        a.data = v->valuestring;
    } else if (const cJSON* v = jget(obj, "data"); cJSON_IsObject(const_cast<cJSON*>(v))) {
        char* printed = cJSON_PrintUnformatted(const_cast<cJSON*>(v));
        if (printed) {
            a.data = printed;
            cJSON_free(printed);
        }
    }
    const cJSON* headers = jget(obj, "headers");
    if (cJSON_IsArray(const_cast<cJSON*>(headers))) {
        for (int i = 0; i < kHotkeyHeaderCount; ++i) {
            a.headers[i] = {};
            const cJSON* h = cJSON_GetArrayItem(const_cast<cJSON*>(headers), i);
            if (!cJSON_IsObject(const_cast<cJSON*>(h))) {
                continue;
            }
            if (const cJSON* n = jget(h, "name"); cJSON_IsString(n) && n->valuestring) {
                a.headers[i].name = n->valuestring;
            }
            if (const cJSON* val = jget(h, "value"); cJSON_IsString(val) && val->valuestring) {
                a.headers[i].value = val->valuestring;
            }
        }
    }
}

cJSON* action_to_json(const HotkeyAction& a, int id) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", id);
    cJSON_AddStringToObject(obj, "name", a.name.c_str());
    cJSON_AddStringToObject(obj, "type", hotkey_type_to_str(a.type));
    cJSON_AddStringToObject(obj, "method", a.method.c_str());
    cJSON_AddStringToObject(obj, "url", a.url.c_str());
    cJSON_AddStringToObject(obj, "body", a.body.c_str());
    cJSON_AddStringToObject(obj, "service", a.service.c_str());
    cJSON_AddStringToObject(obj, "entity_id", a.entity_id.c_str());
    cJSON_AddStringToObject(obj, "data", a.data.c_str());
    cJSON* headers = cJSON_AddArrayToObject(obj, "headers");
    for (const auto& h : a.headers) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", h.name.c_str());
        cJSON_AddStringToObject(item, "value", h.value.c_str());
        cJSON_AddItemToArray(headers, item);
    }
    return obj;
}

} // namespace

void clamp_hotkeys_settings(HotkeysSettings& s) {
    clamp_str(s.ha_base_url, kHotkeyUrlMax);
    trim_token(s.ha_token);
    clamp_str(s.ha_token, kHotkeyTokenMax);
    s.ha_base_url = trim_slash(s.ha_base_url);
    s.keypad_enabled = false;
    for (int& pin : s.row_pins) {
        if (pin < 0 || pin > 39) {
            pin = 0;
        }
    }
    for (int& pin : s.col_pins) {
        if (pin < 0 || pin > 39) {
            pin = 0;
        }
    }
    for (auto& a : s.actions) {
        clamp_str(a.name, kHotkeyNameMax);
        clamp_str(a.url, kHotkeyUrlMax);
        clamp_str(a.body, kHotkeyBodyMax);
        clamp_str(a.service, kHotkeyServiceMax);
        clamp_str(a.entity_id, kHotkeyServiceMax);
        clamp_str(a.data, kHotkeyBodyMax);
        upper_method(a.method);
        if (!valid_method(a.method)) {
            a.method = "POST";
        }
        for (auto& h : a.headers) {
            clamp_str(h.name, 32);
            clamp_str(h.value, kHotkeyHeaderMax);
        }
    }
}

HotkeysService::HotkeysService(Preferences& preferences, WifiService& wifi)
    : preferences_(preferences), wifi_(wifi) {}

Result<void> HotkeysService::start() {
    if (started_) {
        return Result<void>::ok();
    }
    apply_settings();
    started_ = true;
    log.info("hotkeys ready slots=%d", action_count());
    return Result<void>::ok();
}

void HotkeysService::apply_settings() {
    settings_ = {};
    const std::string& blob = preferences_.hotkeys_blob();
    if (blob.empty()) {
        return;
    }
    cJSON* json = cJSON_Parse(blob.c_str());
    if (json == nullptr) {
        log.warn("hotkeys blob invalid — using defaults");
        return;
    }
    bool token_provided = false;
    (void)merge_json(json, settings_, token_provided);
    clamp_hotkeys_settings(settings_);
    cJSON_Delete(json);
}

HotkeysStatus HotkeysService::status() const {
    HotkeysStatus st;
    st.enabled = started_;
    st.action_count = action_count();
    st.last_id = last_id_;
    st.last_http_status = last_http_status_;
    st.last_error = last_error_;
    st.last_fire_ms = last_fire_ms_;
    return st;
}

Result<void> HotkeysService::update(const HotkeysSettings& next, bool token_provided) {
    const std::string keep_token = settings_.ha_token;
    settings_.ha_base_url = next.ha_base_url;
    if (token_provided) {
        settings_.ha_token = next.ha_token;
    } else {
        settings_.ha_token = keep_token;
    }
    settings_.keypad_enabled = false;
    settings_.row_pins = next.row_pins;
    settings_.col_pins = next.col_pins;
    settings_.actions = next.actions;
    clamp_hotkeys_settings(settings_);
    persist();
    return Result<void>::ok();
}

cJSON* HotkeysService::to_json(bool include_secrets) const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", started_);
    cJSON_AddNumberToObject(root, "action_count", action_count());
    cJSON_AddNumberToObject(root, "last_id", last_id_);
    cJSON_AddNumberToObject(root, "last_http_status", last_http_status_);
    cJSON_AddStringToObject(root, "last_error", last_error_.c_str());
    cJSON_AddNumberToObject(root, "last_fire_ms", last_fire_ms_);

    cJSON* ha = cJSON_AddObjectToObject(root, "ha");
    cJSON_AddStringToObject(ha, "base_url", settings_.ha_base_url.c_str());
    cJSON_AddBoolToObject(ha, "token_set", !settings_.ha_token.empty());
    if (include_secrets) {
        cJSON_AddStringToObject(ha, "token", settings_.ha_token.c_str());
    }

    cJSON* kp = cJSON_AddObjectToObject(root, "keypad");
    cJSON_AddBoolToObject(kp, "enabled", settings_.keypad_enabled);
    cJSON* rows = cJSON_AddArrayToObject(kp, "row_pins");
    cJSON* cols = cJSON_AddArrayToObject(kp, "col_pins");
    for (int i = 0; i < 4; ++i) {
        cJSON_AddItemToArray(rows, cJSON_CreateNumber(settings_.row_pins[i]));
        cJSON_AddItemToArray(cols, cJSON_CreateNumber(settings_.col_pins[i]));
    }

    cJSON* actions = cJSON_AddArrayToObject(root, "actions");
    for (int i = 0; i < kHotkeySlotCount; ++i) {
        cJSON_AddItemToArray(actions, action_to_json(settings_.actions[i], i));
    }
    return root;
}

bool HotkeysService::merge_json(const cJSON* obj, HotkeysSettings& next, bool& token_provided) const {
    if (!cJSON_IsObject(const_cast<cJSON*>(obj))) {
        return false;
    }
    token_provided = false;

    const cJSON* ha = jget(obj, "ha");
    if (cJSON_IsObject(const_cast<cJSON*>(ha))) {
        if (const cJSON* v = jget(ha, "base_url"); cJSON_IsString(v) && v->valuestring) {
            next.ha_base_url = v->valuestring;
        }
        if (const cJSON* v = jget(ha, "token"); cJSON_IsString(v) && v->valuestring) {
            token_provided = true;
            next.ha_token = v->valuestring;
        }
    }

    const cJSON* kp = jget(obj, "keypad");
    if (cJSON_IsObject(const_cast<cJSON*>(kp))) {
        const cJSON* rows = jget(kp, "row_pins");
        const cJSON* cols = jget(kp, "col_pins");
        if (cJSON_IsArray(const_cast<cJSON*>(rows))) {
            for (int i = 0; i < 4; ++i) {
                const cJSON* n = cJSON_GetArrayItem(const_cast<cJSON*>(rows), i);
                if (cJSON_IsNumber(n)) {
                    next.row_pins[i] = n->valueint;
                }
            }
        }
        if (cJSON_IsArray(const_cast<cJSON*>(cols))) {
            for (int i = 0; i < 4; ++i) {
                const cJSON* n = cJSON_GetArrayItem(const_cast<cJSON*>(cols), i);
                if (cJSON_IsNumber(n)) {
                    next.col_pins[i] = n->valueint;
                }
            }
        }
    }

    const cJSON* actions = jget(obj, "actions");
    if (cJSON_IsArray(const_cast<cJSON*>(actions))) {
        const int n = cJSON_GetArraySize(const_cast<cJSON*>(actions));
        for (int i = 0; i < n; ++i) {
            const cJSON* item = cJSON_GetArrayItem(const_cast<cJSON*>(actions), i);
            int id = i;
            if (const cJSON* v = jget(item, "id"); cJSON_IsNumber(v)) {
                id = v->valueint;
            }
            if (id < 0 || id >= kHotkeySlotCount) {
                continue;
            }
            set_action_from_json(next.actions[id], item);
        }
    } else if (cJSON_IsObject(const_cast<cJSON*>(actions))) {
        int id = 0;
        if (const cJSON* v = jget(actions, "id"); cJSON_IsNumber(v)) {
            id = v->valueint;
        }
        if (id >= 0 && id < kHotkeySlotCount) {
            set_action_from_json(next.actions[id], actions);
        }
    }
    return true;
}

Result<void> HotkeysService::apply_json(const cJSON* obj) {
    bool token_provided = false;
    const std::string keep_token = settings_.ha_token;
    if (!merge_json(obj, settings_, token_provided)) {
        return Result<void>::fail(ErrorCode::InvalidArgument, "hotkeys object required");
    }
    if (!token_provided) {
        settings_.ha_token = keep_token;
    }
    clamp_hotkeys_settings(settings_);
    persist();
    return Result<void>::ok();
}

void HotkeysService::test_fire(int id) {
    if (id < 0 || id >= kHotkeySlotCount) {
        last_id_ = id;
        last_http_status_ = 0;
        last_error_ = "invalid slot";
        return;
    }
    if (busy_.exchange(true)) {
        last_error_ = "busy";
        log.warn("hotkey fire dropped — already in flight");
        return;
    }
    pending_id_ = id;
    if (xTaskCreatePinnedToCore(&HotkeysService::fire_task, "hk_fire", 8192, this, 5, nullptr, 1) !=
        pdPASS) {
        busy_.store(false);
        last_error_ = "task create failed";
        log.error("hotkey fire task create failed");
    }
}

void HotkeysService::fire_task(void* arg) {
    auto* self = static_cast<HotkeysService*>(arg);
    if (self != nullptr) {
        self->run_fire(self->pending_id_);
        self->busy_.store(false);
    }
    vTaskDelete(nullptr);
}

void HotkeysService::run_fire(int id) {
    last_id_ = id;
    last_http_status_ = 0;
    last_error_.clear();
    last_fire_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);

    if (!wifi_.status().connected) {
        last_error_ = "wifi not connected";
        log.warn("hotkey %d skipped — wifi not connected", id);
        return;
    }
    if (id < 0 || id >= kHotkeySlotCount || hotkey_slot_empty(settings_.actions[id])) {
        last_error_ = "empty slot";
        return;
    }

    std::string method;
    std::string url;
    std::string body;
    std::array<HotkeyHeader, kHotkeyHeaderCount + 1> headers{};
    int header_n = 0;
    std::string err;
    if (!build_request(id, method, url, body, headers, header_n, err)) {
        last_error_ = err;
        log.warn("hotkey %d build failed", id);
        return;
    }

    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.method = to_esp_method(method);
    cfg.timeout_ms = 5000;
    cfg.disable_auto_redirect = true;
    cfg.crt_bundle_attach = nullptr;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        last_error_ = "http init failed";
        return;
    }
    for (int i = 0; i < header_n; ++i) {
        if (!headers[i].name.empty()) {
            (void)esp_http_client_set_header(client, headers[i].name.c_str(),
                                             headers[i].value.c_str());
        }
    }
    if (!body.empty() && method != "GET") {
        (void)esp_http_client_set_header(client, "Content-Type", "application/json");
        (void)esp_http_client_set_post_field(client, body.c_str(),
                                             static_cast<int>(body.size()));
    }

    const esp_err_t rc = esp_http_client_perform(client);
    if (rc == ESP_OK) {
        last_http_status_ = esp_http_client_get_status_code(client);
        if (last_http_status_ == 401) {
            last_error_ = "HTTP 401 — HA rejected the token; paste it again";
        } else if (last_http_status_ >= 400) {
            last_error_ = "HTTP " + std::to_string(last_http_status_);
        }
    } else {
        last_error_ = esp_err_to_name(rc);
    }
    esp_http_client_cleanup(client);
    log.info("hotkey %d done status=%d", id, last_http_status_);
}

bool HotkeysService::build_request(int id, std::string& method, std::string& url, std::string& body,
                                   std::array<HotkeyHeader, kHotkeyHeaderCount + 1>& headers,
                                   int& header_n, std::string& err) const {
    const HotkeyAction& a = settings_.actions[id];
    header_n = 0;
    if (a.type == HotkeyType::HomeAssistant) {
        if (settings_.ha_base_url.empty() || settings_.ha_token.empty()) {
            err = "HA base URL or token missing";
            return false;
        }
        const auto dot = a.service.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= a.service.size()) {
            err = "invalid HA service";
            return false;
        }
        method = "POST";
        url = trim_slash(settings_.ha_base_url) + "/api/services/" + a.service.substr(0, dot) + "/" +
              a.service.substr(dot + 1);
        headers[header_n++] = HotkeyHeader{"Authorization", "Bearer " + settings_.ha_token};

        cJSON* json = nullptr;
        if (!a.data.empty()) {
            json = cJSON_Parse(a.data.c_str());
        }
        if (json == nullptr || !cJSON_IsObject(json)) {
            if (json) {
                cJSON_Delete(json);
            }
            json = cJSON_CreateObject();
        }
        if (!a.entity_id.empty()) {
            cJSON_DeleteItemFromObject(json, "entity_id");
            cJSON_AddStringToObject(json, "entity_id", a.entity_id.c_str());
        }
        char* printed = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        if (printed) {
            body = printed;
            cJSON_free(printed);
        } else {
            body = "{}";
        }
        return true;
    }

    if (a.url.empty()) {
        err = "empty url";
        return false;
    }
    method = a.method.empty() ? "POST" : a.method;
    url = a.url;
    body = a.body;
    for (const auto& h : a.headers) {
        if (!h.name.empty()) {
            headers[header_n++] = h;
        }
    }
    return true;
}

void HotkeysService::persist() {
    cJSON* root = cJSON_CreateObject();
    cJSON* ha = cJSON_AddObjectToObject(root, "ha");
    cJSON_AddStringToObject(ha, "base_url", settings_.ha_base_url.c_str());
    cJSON_AddStringToObject(ha, "token", settings_.ha_token.c_str());
    cJSON* kp = cJSON_AddObjectToObject(root, "keypad");
    cJSON_AddBoolToObject(kp, "enabled", false);
    cJSON* rows = cJSON_AddArrayToObject(kp, "row_pins");
    cJSON* cols = cJSON_AddArrayToObject(kp, "col_pins");
    for (int i = 0; i < 4; ++i) {
        cJSON_AddItemToArray(rows, cJSON_CreateNumber(settings_.row_pins[i]));
        cJSON_AddItemToArray(cols, cJSON_CreateNumber(settings_.col_pins[i]));
    }
    cJSON* actions = cJSON_AddArrayToObject(root, "actions");
    for (int i = 0; i < kHotkeySlotCount; ++i) {
        cJSON_AddItemToArray(actions, action_to_json(settings_.actions[i], i));
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed) {
        preferences_.set_hotkeys_blob(printed);
        cJSON_free(printed);
    }
    preferences_.save();
}

int HotkeysService::action_count() const {
    int n = 0;
    for (const auto& a : settings_.actions) {
        if (!hotkey_slot_empty(a)) {
            ++n;
        }
    }
    return n;
}

} // namespace lumos
