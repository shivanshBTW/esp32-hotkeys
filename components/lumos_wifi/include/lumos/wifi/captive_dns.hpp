#pragma once

#include <atomic>
#include <cstdint>

namespace lumos {

// Minimal DNS responder that answers every A query with the SoftAP IP (captive portal).
class CaptiveDns {
public:
    bool start(std::uint32_t ap_ip_be);
    void stop();

private:
    static void task(void* arg);

    int sock_{-1};
    std::uint32_t ap_ip_be_{0};
    std::atomic<bool> running_{false};
    void* task_handle_{nullptr};
};

} // namespace lumos
