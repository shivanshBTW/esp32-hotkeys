#include "lumos/ota/ota_service.hpp"
#include "lumos/core/logger.hpp"

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstddef>
#include <cstring>

namespace lumos {
namespace {
Logger log{"ota"};

bool content_type_is_multipart(httpd_req_t* req) {
    char ctype[128]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK) {
        return false;
    }
    return std::strstr(ctype, "multipart/") != nullptr;
}

bool recv_until_double_crlf(httpd_req_t* req, int& remaining) {
    int match = 0;
    while (remaining > 0 && match < 4) {
        char c = 0;
        const int n = httpd_req_recv(req, &c, 1);
        if (n <= 0) {
            return false;
        }
        remaining -= n;
        if ((match == 0 || match == 2) && c == '\r') {
            ++match;
        } else if ((match == 1 || match == 3) && c == '\n') {
            ++match;
        } else {
            match = (c == '\r') ? 1 : 0;
        }
    }
    return match == 4;
}

esp_err_t write_payload(esp_ota_handle_t ota_handle, const char* data, int len) {
    return esp_ota_write(ota_handle, data, static_cast<std::size_t>(len));
}
} // namespace

esp_err_t OtaService::post_ota(httpd_req_t* req) {
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return err;
    }

    char buf[1024];
    int remaining = req->content_len;
    const bool multipart = content_type_is_multipart(req);
    log.info("OTA upload starting (%d bytes%s) -> %s", remaining, multipart ? ", multipart" : "",
             update_partition->label);

    if (multipart && !recv_until_double_crlf(req, remaining)) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "multipart headers");
        return ESP_FAIL;
    }

    char tail[256]{};
    int tail_len = 0;
    while (remaining > 0) {
        const int to_read = remaining > static_cast<int>(sizeof(buf))
                                ? static_cast<int>(sizeof(buf))
                                : remaining;
        const int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        remaining -= received;
        if (!multipart) {
            err = write_payload(ota_handle, buf, received);
        } else {
            const int keep = 128;
            if (tail_len + received <= static_cast<int>(sizeof(tail))) {
                std::memcpy(tail + tail_len, buf, static_cast<std::size_t>(received));
                tail_len += received;
            } else {
                const int flush = tail_len + received - keep;
                if (flush <= tail_len) {
                    err = write_payload(ota_handle, tail, flush);
                    std::memmove(tail, tail + flush, static_cast<std::size_t>(tail_len - flush));
                    tail_len -= flush;
                    std::memcpy(tail + tail_len, buf, static_cast<std::size_t>(received));
                    tail_len += received;
                } else {
                    err = write_payload(ota_handle, tail, tail_len);
                    if (err == ESP_OK) {
                        const int from_buf = received - keep;
                        err = write_payload(ota_handle, buf, from_buf);
                    }
                    std::memcpy(tail, buf + (received - keep), static_cast<std::size_t>(keep));
                    tail_len = keep;
                }
            }
        }
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return err;
        }
    }
    if (multipart) {
        int cut = tail_len;
        for (int i = 0; i + 3 < tail_len; ++i) {
            if (tail[i] == '\r' && tail[i + 1] == '\n' && tail[i + 2] == '-' && tail[i + 3] == '-') {
                cut = i;
                break;
            }
        }
        if (cut > 0) {
            err = write_payload(ota_handle, tail, cut);
            if (err != ESP_OK) {
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
                return err;
            }
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return err;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    log.info("OTA success — rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

Result<void> OtaService::start(httpd_handle_t server) {
    httpd_uri_t uri = {
        .uri = "/api/v1/ota",
        .method = HTTP_POST,
        .handler = post_ota,
        .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        return Result<void>::fail(ErrorCode::IoError, "failed to register OTA route");
    }
    return Result<void>::ok();
}

} // namespace lumos
