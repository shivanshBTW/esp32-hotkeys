#pragma once

#include "lumos/core/result.hpp"

#include <esp_http_server.h>

namespace lumos {

class WebUi {
public:
    Result<void> start(httpd_handle_t server);

private:
    static esp_err_t get_index(httpd_req_t* req);
    static esp_err_t get_doorbell(httpd_req_t* req);
    static esp_err_t get_android_probe(httpd_req_t* req);
    static esp_err_t get_apple_probe(httpd_req_t* req);
    static esp_err_t get_windows_probe(httpd_req_t* req);
};

} // namespace lumos
