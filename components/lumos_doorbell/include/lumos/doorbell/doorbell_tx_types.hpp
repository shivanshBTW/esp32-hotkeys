#pragma once

#include "lumos/core/board_pins.hpp"
#include "lumos/doorbell/doorbell_packet.hpp"

#include <cstdint>
#include <string>

namespace lumos {

struct DoorbellTxConfig {
    int opto_pin{kDefaultOptoGpio};
    bool active_low{true}; // typical optocoupler collector to GPIO, emitter to GND
    std::uint8_t channel{1};
    std::uint8_t rx_mac[6]{};
    bool rx_mac_valid{false};
    std::uint32_t tx_id{1};
};

struct DoorbellTxStatus {
    DoorbellTxConfig cfg{};
    std::string own_mac;
    bool espnow_ready{false};
    bool paired{false};
    std::uint8_t last_seq{0};
    std::uint32_t last_send_ms{0};
    bool pairing{false};
    bool scanning{false};
    std::uint32_t pairing_ms{0};
    int peer_count{0};
    DoorbellDiscoveredPeer peers[kDoorbellMaxPeers]{};
    int opto_level{-1}; // 0/1 after GPIO is configured
};

constexpr const char* kDoorbellTxNvsNs = "dbtx";
constexpr std::uint32_t kDoorbellTxDebounceUs = 40 * 1000;
constexpr std::uint32_t kDoorbellTxRetryUs = 20 * 1000;
constexpr int kDoorbellTxRetries = 2; // first send + 2 = 3 total
constexpr std::uint32_t kDoorbellTxCooldownMs = 1200;
constexpr std::uint64_t kDoorbellTxHelloPeriodUs = 400 * 1000;

} // namespace lumos
