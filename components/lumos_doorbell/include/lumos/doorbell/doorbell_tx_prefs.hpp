#pragma once

#include <cstdint>
#include <string>

namespace lumos {

constexpr const char* kDoorbellApSsid = "LumosOS-Bell";
constexpr const char* kDoorbellWifiNvsNs = "dbwifi";
constexpr int kDoorbellStaFailLimit = 5;
constexpr std::uint64_t kDoorbellBackgroundRetryUs = 30ULL * 1000ULL * 1000ULL;
constexpr std::int64_t kDoorbellUiPresenceHoldMs = 10000;
constexpr const char* kDoorbellConfigProduct = "doorbell_tx";
constexpr const char* kDoorbellApIp = "192.168.4.1";

constexpr const char* kDoorbellAppleCaptiveHtml =
    "<!DOCTYPE HTML PUBLIC \"-//Apple//DTD HTML 3.2//EN\">"
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
constexpr const char* kDoorbellWindowsNcsi = "Microsoft NCSI";

struct WifiPrefs {
    std::string ssid;
    std::string password;
    std::string hostname{"LumosOS-Bell"};
    bool use_static{false};
    std::string ip;
    std::string gateway;
    std::string netmask{"255.255.255.0"};
    std::string dns1;
    std::string dns2;
};

} // namespace lumos
