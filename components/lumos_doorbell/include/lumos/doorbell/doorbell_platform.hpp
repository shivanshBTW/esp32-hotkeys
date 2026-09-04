#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ESP_PLATFORM) && !defined(ARDUINO)
#include "esp_attr.h"
#define LUMOS_DBPLAT_IRAM IRAM_ATTR
#else
#define LUMOS_DBPLAT_IRAM
#endif

namespace lumos::dbplat {

using RecvFn = void (*)(const std::uint8_t mac[6], const std::uint8_t* data, int len, int rssi);
using TimerFn = void (*)(void* arg);
using TaskFn = void (*)(void* arg);

enum TimerId : int {
    TDebounce = 0,
    TRetry = 1,
    THello = 2,
};

std::uint64_t now_us();

bool nvs_load(std::int32_t* pin, std::uint8_t* ch, std::uint8_t* alow, std::uint32_t* txid, char* rxmac,
              std::size_t rxmac_sz, char* rxlan = nullptr, std::size_t rxlan_sz = 0);
bool nvs_save(std::int32_t pin, std::uint8_t ch, std::uint8_t alow, std::uint32_t txid, const char* rxmac,
              const char* rxlan = nullptr);
bool lan_ring(const char* ip);

void gpio_unhook(int pin);
bool gpio_setup_input(int pin, bool pull_up, void (*isr)(void*), void* arg);
int gpio_read(int pin);

struct GpioWatch {
    int pin{-1};
    int level{-1};
    std::uint32_t edges{0};
};
inline constexpr int kGpioWatchMax = 5;
int gpio_watch(GpioWatch* out, int max);

bool espnow_start(RecvFn cb);
bool espnow_add_peer(const std::uint8_t mac[6], std::uint8_t channel, bool sta_if);
bool espnow_send(const std::uint8_t mac[6], const std::uint8_t* data, int len);
// ESP8266 ESP-NOW dies across WiFi.mode(); ESP-IDF no-op.
void espnow_on_wifi_change();

void wifi_set_ap_channel(std::uint8_t channel);
std::uint8_t wifi_radio_channel(std::uint8_t fallback);
void wifi_own_mac(std::uint8_t out[6], bool sta);

bool timer_create(TimerId id, TimerFn cb, void* arg);
void LUMOS_DBPLAT_IRAM timer_stop(TimerId id);
void LUMOS_DBPLAT_IRAM timer_once(TimerId id, std::uint32_t us);
void timer_periodic(TimerId id, std::uint32_t us);

bool spawn(TaskFn fn, void* arg);
void delay_ms(std::uint32_t ms);
void task_exit();
void set_pump(void (*fn)());
void poll();

} // namespace lumos::dbplat
