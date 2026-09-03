#pragma once

#include <cstdint>
#include <cstring>

namespace lumos {

// Shared ESP-NOW wire format for doorbell TX (#1) and RX (#2).
constexpr std::uint32_t kDoorbellMagic = 0x4C444242u; // 'LDBB'
constexpr std::uint8_t kDoorbellVersion = 1;
constexpr std::uint32_t kDoorbellPairDefaultMs = 60 * 1000;
constexpr int kDoorbellMaxPeers = 8;

inline constexpr std::uint8_t kDoorbellBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum DoorbellEventType : std::uint8_t {
    DOORBELL_PRESS = 1,
    DOORBELL_PAIR_HELLO = 2, // advertise during pairing
    DOORBELL_PAIR_CLAIM = 3, // "save my MAC"
};

enum DoorbellRole : std::uint8_t {
    DOORBELL_ROLE_TX = 1,
    DOORBELL_ROLE_RX = 2,
};

struct DoorbellPacket {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t type;
    std::uint8_t seq;
    std::uint8_t reserved;
    std::uint32_t tx_id;
} __attribute__((packed));

static_assert(sizeof(DoorbellPacket) == 12, "DoorbellPacket size");

struct DoorbellPairHello {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t type;
    std::uint8_t seq;
    std::uint8_t role;
    std::uint32_t tx_id;
    std::uint8_t channel;
    std::uint8_t mac[6];
    char name[16];
} __attribute__((packed));

static_assert(sizeof(DoorbellPairHello) == 35, "DoorbellPairHello size");

struct DoorbellDiscoveredPeer {
    std::uint8_t mac[6]{};
    std::uint8_t channel{0};
    std::uint8_t role{0};
    std::int8_t rssi{0};
    char name[16]{};
};

inline void fill_pair_hello(DoorbellPairHello& pkt, std::uint8_t type, std::uint8_t role,
                            std::uint8_t channel, const std::uint8_t mac[6], const char* name,
                            std::uint32_t id) {
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.magic = kDoorbellMagic;
    pkt.version = kDoorbellVersion;
    pkt.type = type;
    pkt.role = role;
    pkt.tx_id = id;
    pkt.channel = channel;
    if (mac != nullptr) {
        std::memcpy(pkt.mac, mac, 6);
    }
    if (name != nullptr) {
        std::strncpy(pkt.name, name, sizeof(pkt.name) - 1);
    }
}

inline bool parse_pair_hello(const std::uint8_t* data, int len, DoorbellPairHello& out) {
    if (data == nullptr || len < static_cast<int>(sizeof(DoorbellPairHello))) {
        return false;
    }
    std::memcpy(&out, data, sizeof(out));
    out.name[sizeof(out.name) - 1] = '\0';
    return out.magic == kDoorbellMagic && out.version == kDoorbellVersion &&
           (out.type == DOORBELL_PAIR_HELLO || out.type == DOORBELL_PAIR_CLAIM);
}

} // namespace lumos
