#pragma once

#include "lumos/core/result.hpp"
#include "lumos/doorbell/doorbell_tx_types.hpp"

#include <cstdint>
#include <string>

namespace lumos {

class DoorbellTransmitter {
public:
    Result<void> start();
    void apply_config(const DoorbellTxConfig& cfg);
    void load_nvs();
    void save_nvs();
    void test_send();
    DoorbellTxConfig config() const { return cfg_; }
    DoorbellTxStatus status() const;
    std::uint8_t last_seq() const { return seq_; }
    std::uint32_t last_send_ms() const { return last_send_ms_; }
    bool espnow_ready() const { return espnow_ready_; }
    std::string own_mac() const;

    void start_pairing(std::uint32_t duration_ms = kDoorbellPairDefaultMs);
    void stop_pairing();
    bool select_peer(const std::uint8_t mac[6]);
    void set_sta_linked(bool linked);
    bool sta_linked() const { return sta_linked_; }

    // ESP8266 cooperative loop; no-op on ESP-IDF (scan runs in a task).
    void poll();

private:
    static void gpio_isr(void* arg);
    static void debounce_timer_cb(void* arg);
    static void retry_timer_cb(void* arg);
    static void hello_timer_cb(void* arg);
    static void scan_task(void* arg);
    static void recv_fn(const std::uint8_t mac[6], const std::uint8_t* data, int len, int rssi);

    void maybe_fire();
    void send_press(bool bump_seq);
    void configure_gpio();
    void configure_wifi_channel();
    void add_peer();
    void ensure_broadcast_peer();
    void send_pair(std::uint8_t type, const std::uint8_t* dest);
    void on_packet(const std::uint8_t mac[6], const std::uint8_t* data, int len, int rssi);
    void note_peer(const std::uint8_t mac[6], const DoorbellPairHello& hello, int rssi);
    void own_mac_bytes(std::uint8_t out[6]) const;
    bool pairing_active() const;
    void set_ap_channel(std::uint8_t channel);
    std::uint8_t radio_channel() const;

    DoorbellTxConfig cfg_{};
    bool started_{false};
    bool espnow_ready_{false};
    int configured_pin_{-1};
    volatile std::uint32_t opto_edges_{0};
    volatile std::uint32_t last_edge_ms_{0};
    std::uint8_t seq_{0};
    std::uint32_t last_send_ms_{0};
    int retries_left_{0};
    std::uint64_t pairing_until_us_{0};
    bool scanning_{false};
    bool sta_linked_{false};
    int peer_count_{0};
    DoorbellDiscoveredPeer peers_[kDoorbellMaxPeers]{};

    static DoorbellTransmitter* instance_;
};

} // namespace lumos
