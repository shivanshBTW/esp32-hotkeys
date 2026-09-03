#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lumos {

struct NeighborInfo {
    std::string hostname;
    std::string ip;
    std::uint16_t port{80};
    std::string version;
    std::string api;
    std::string leds;
    std::string chipset;
    std::string path{"/"};
};

// Pure helper (host-testable) — no HTML escaping; values are device-controlled TXT/mDNS.
inline std::string neighbors_to_json(const std::vector<NeighborInfo>& neighbors) {
    std::string out = "{\"neighbors\":[";
    bool first = true;
    for (const auto& n : neighbors) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        char port_buf[16];
        std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(n.port));
        out += "{\"hostname\":\"";
        out += n.hostname;
        out += "\",\"ip\":\"";
        out += n.ip;
        out += "\",\"port\":";
        out += port_buf;
        out += ",\"version\":\"";
        out += n.version;
        out += "\",\"api\":\"";
        out += n.api;
        out += "\",\"leds\":\"";
        out += n.leds;
        out += "\",\"chipset\":\"";
        out += n.chipset;
        out += "\",\"path\":\"";
        out += (n.path.empty() ? "/" : n.path);
        out += "\"}";
    }
    out += "]}";
    return out;
}

} // namespace lumos
