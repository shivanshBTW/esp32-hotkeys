#include "lumos/wifi/captive_dns.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstring>

namespace lumos {

bool CaptiveDns::start(std::uint32_t ap_ip_be) {
    stop();
    ap_ip_be_ = ap_ip_be;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        return false;
    }

    int yes = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    running_ = true;
    xTaskCreate(&CaptiveDns::task, "captive_dns", 3072, this, 5, reinterpret_cast<TaskHandle_t*>(&task_handle_));
    return true;
}

void CaptiveDns::stop() {
    running_ = false;
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
    if (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
        task_handle_ = nullptr;
    }
}

void CaptiveDns::task(void* arg) {
    auto* self = static_cast<CaptiveDns*>(arg);
    std::uint8_t buf[512];

    while (self->running_) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int n = ::recvfrom(self->sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                                 &from_len);
        if (n < 12) {
            continue;
        }

        // Build a minimal DNS response: copy query, set response flags, append A record.
        std::uint8_t response[512];
        if (static_cast<std::size_t>(n) + 16 > sizeof(response)) {
            continue;
        }
        std::memcpy(response, buf, static_cast<std::size_t>(n));
        response[2] = 0x81; // response + recursion available
        response[3] = 0x80;
        response[6] = 0x00;
        response[7] = 0x01; // ANCOUNT = 1

        std::size_t pos = static_cast<std::size_t>(n);
        // Name pointer to offset 12
        response[pos++] = 0xC0;
        response[pos++] = 0x0C;
        response[pos++] = 0x00;
        response[pos++] = 0x01; // Type A
        response[pos++] = 0x00;
        response[pos++] = 0x01; // Class IN
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x3C; // TTL 60
        response[pos++] = 0x00;
        response[pos++] = 0x04; // RDLENGTH
        std::memcpy(response + pos, &self->ap_ip_be_, 4);
        pos += 4;

        ::sendto(self->sock_, response, pos, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }

    vTaskDelete(nullptr);
}

} // namespace lumos
