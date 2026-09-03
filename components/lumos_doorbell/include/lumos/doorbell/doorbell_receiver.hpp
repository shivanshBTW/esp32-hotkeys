#pragma once

#include "lumos/core/result.hpp"
#include "lumos/doorbell/doorbell_packet.hpp"
#include "lumos/preferences/preferences.hpp"

#include "esp_now.h"
#include "esp_timer.h"

#include <cstdint>
#include <string>

namespace lumos {

struct DoorbellStatus {
    bool enabled{false};
    bool espnow_ready{false};
    bool paired{false};
    int relay_pin{kDefaultRelayGpio};
    bool active_high{true};
    bool tone{false};
    std::uint16_t press_ms{400};
    std::string paired_tx_mac;
    std::string own_mac;
    std::uint8_t wifi_channel{0};
    std::uint32_t last_ring_ms{0}; // ms since boot; 0 = never
    std::uint8_t last_seq{0};
    bool relay_active{false};
    bool pairing{false};
    std::uint32_t pairing_ms{0};
    int peer_count{0};
    DoorbellDiscoveredPeer peers[kDoorbellMaxPeers]{};
};

class DoorbellReceiver {
public:
    explicit DoorbellReceiver(Preferences& preferences);

    Result<void> start();
    void apply_settings();
    void test_pulse();
    DoorbellStatus status() const;

    void start_pairing(std::uint32_t duration_ms = kDoorbellPairDefaultMs);
    void stop_pairing();
    bool select_peer(const std::uint8_t mac[6]);

private:
    static void recv_cb(const esp_now_recv_info_t* info, const std::uint8_t* data, int len);
    static void release_timer_cb(void* arg);
    static void hello_timer_cb(void* arg);

    void on_packet(const std::uint8_t mac[6], const std::uint8_t* data, int len, int rssi);
    void handle_pair(const std::uint8_t src[6], const DoorbellPairHello& hello, int rssi);
    void note_peer(const std::uint8_t mac[6], const DoorbellPairHello& hello, int rssi);
    void pulse_relay(std::uint16_t override_ms = 0);
    void set_relay(bool active);
    void configure_gpio();
    void ensure_tone(int pin);
    void start_tone();
    void stop_tone();
    void run_test_buzz();
    static void test_task(void* arg);
    void send_pair(std::uint8_t type, const std::uint8_t* dest);
    void ensure_broadcast_peer();
    bool pairing_active() const;
    std::uint8_t current_channel() const;
    void own_mac_bytes(std::uint8_t out[6]) const;
    bool parse_paired_mac(std::uint8_t out[6]) const;
    static std::string format_mac(const std::uint8_t mac[6]);
    static bool mac_equal(const std::uint8_t a[6], const std::uint8_t b[6]);

    Preferences& preferences_;
    bool started_{false};
    bool espnow_ready_{false};
    esp_timer_handle_t release_timer_{nullptr};
    esp_timer_handle_t hello_timer_{nullptr};
    int configured_pin_{-1};
    bool relay_active_{false};
    bool tone_ready_{false};
    bool have_last_seq_{false};
    std::uint8_t last_seq_{0};
    std::uint32_t last_ring_ms_{0};
    std::uint8_t paired_mac_[6]{};
    bool paired_valid_{false};
    std::uint64_t pairing_until_us_{0};
    int peer_count_{0};
    DoorbellDiscoveredPeer peers_[kDoorbellMaxPeers]{};

    static DoorbellReceiver* instance_;
};

} // namespace lumos
