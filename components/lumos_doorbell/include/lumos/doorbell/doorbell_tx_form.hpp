#pragma once

#include <cctype>
#include <cstdio>
#include <string>

namespace lumos {

inline std::string doorbell_form_value(const std::string& body, const char* key) {
    const std::string prefix = std::string(key) + "=";
    auto pos = body.find(prefix);
    if (pos == std::string::npos) {
        return {};
    }
    pos += prefix.size();
    auto end = body.find('&', pos);
    std::string raw = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '+') {
            out.push_back(' ');
        } else if (raw[i] == '%' && i + 2 < raw.size()) {
            unsigned v = 0;
            if (std::sscanf(raw.c_str() + i + 1, "%02x", &v) == 1) {
                out.push_back(static_cast<char>(v));
                i += 2;
            }
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

inline std::string doorbell_mdns_label(std::string host) {
    std::string out;
    for (unsigned char c : host) {
        if (std::isalnum(c) || c == '-') {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out.empty() ? "lumosos-bell" : out;
}

} // namespace lumos
