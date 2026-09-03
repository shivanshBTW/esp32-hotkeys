#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lumos {

inline bool parse_mac(const char* s, std::uint8_t out[6]) {
    if (s == nullptr || s[0] == '\0') {
        return false;
    }
    unsigned b[6]{};
    if (std::sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4],
                    &b[5]) == 6 ||
        std::sscanf(s, "%02x%02x%02x%02x%02x%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) ==
            6) {
        for (int i = 0; i < 6; ++i) {
            out[i] = static_cast<std::uint8_t>(b[i]);
        }
        return true;
    }
    return false;
}

inline bool parse_mac(const std::string& s, std::uint8_t out[6]) {
    return parse_mac(s.c_str(), out);
}

inline std::string format_mac(const std::uint8_t mac[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    return buf;
}

inline bool mac_equal(const std::uint8_t a[6], const std::uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

} // namespace lumos
