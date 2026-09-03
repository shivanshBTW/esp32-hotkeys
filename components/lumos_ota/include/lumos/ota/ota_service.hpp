#pragma once

#include "lumos/core/result.hpp"

#include <esp_http_server.h>

namespace lumos {

class OtaService {
public:
    Result<void> start(httpd_handle_t server);

private:
    static esp_err_t post_ota(httpd_req_t* req);
};

} // namespace lumos
