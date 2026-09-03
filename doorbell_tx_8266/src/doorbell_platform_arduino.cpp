#include "lumos/doorbell/doorbell_platform.hpp"
#include "lumos/doorbell/doorbell_tx_types.hpp"

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <espnow.h>

extern "C" {
#include "user_interface.h"
}

#include <cstring>

namespace lumos::dbplat {
namespace {

constexpr int kEepromSize = 512;
constexpr int kTxOff = 0;

struct TxBlob {
    char magic[4];
    std::uint8_t ver;
    std::int32_t pin;
    std::uint8_t ch;
    std::uint8_t alow;
    std::uint32_t txid;
    char rxmac[32];
};

struct TimerSlot {
    Ticker ticker;
    TimerFn cb{nullptr};
    void* arg{nullptr};
};

TimerSlot timers[3]{};
RecvFn recv_cb{nullptr};
void (*pump_fn)(){nullptr};
TaskFn queued_fn{nullptr};
void* queued_arg{nullptr};
bool queued_{false};
volatile bool gpio_edge_{false};
void (*gpio_isr_fn)(void*){nullptr};
void* gpio_isr_arg{nullptr};
int gpio_pin_{-1};
int gpio_last_level_{-1};
bool eeprom_ready_{false};

void ensure_eeprom() {
    if (!eeprom_ready_) {
        EEPROM.begin(kEepromSize);
        eeprom_ready_ = true;
    }
}

void fire0() {
    if (timers[0].cb != nullptr) {
        timers[0].cb(timers[0].arg);
    }
}
void fire1() {
    if (timers[1].cb != nullptr) {
        timers[1].cb(timers[1].arg);
    }
}
void fire2() {
    if (timers[2].cb != nullptr) {
        timers[2].cb(timers[2].arg);
    }
}

void (*fires[3])() = {&fire0, &fire1, &fire2};

void ICACHE_RAM_ATTR raw_gpio_isr() {
    gpio_edge_ = true;
}

void arduino_recv(uint8_t* mac, uint8_t* data, uint8_t len) {
    if (recv_cb == nullptr || mac == nullptr || data == nullptr) {
        return;
    }
    recv_cb(mac, data, static_cast<int>(len), 0);
}

void arduino_send(uint8_t* mac, uint8_t status) {
    (void)mac;
    if (status != 0) {
        Serial.printf("[doorbell_tx] esp_now_send status=%u\n", static_cast<unsigned>(status));
    }
}

bool bind_espnow() {
    if (esp_now_init() != 0) {
        return false;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(&arduino_recv);
    esp_now_register_send_cb(&arduino_send);
    return true;
}

} // namespace

std::uint64_t now_us() {
    return micros64();
}

bool nvs_load(std::int32_t* pin, std::uint8_t* ch, std::uint8_t* alow, std::uint32_t* txid, char* rxmac,
              std::size_t rxmac_sz) {
    ensure_eeprom();
    TxBlob blob{};
    EEPROM.get(kTxOff, blob);
    if (std::memcmp(blob.magic, "DBTX", 4) != 0 || blob.ver != 1) {
        return false;
    }
    if (pin != nullptr) {
        *pin = blob.pin;
    }
    if (ch != nullptr) {
        *ch = blob.ch;
    }
    if (alow != nullptr) {
        *alow = blob.alow;
    }
    if (txid != nullptr) {
        *txid = blob.txid;
    }
    if (rxmac != nullptr && rxmac_sz > 0) {
        std::strncpy(rxmac, blob.rxmac, rxmac_sz - 1);
        rxmac[rxmac_sz - 1] = '\0';
    }
    return true;
}

bool nvs_save(std::int32_t pin, std::uint8_t ch, std::uint8_t alow, std::uint32_t txid,
              const char* rxmac) {
    ensure_eeprom();
    TxBlob blob{};
    std::memcpy(blob.magic, "DBTX", 4);
    blob.ver = 1;
    blob.pin = pin;
    blob.ch = ch;
    blob.alow = alow;
    blob.txid = txid;
    if (rxmac != nullptr) {
        std::strncpy(blob.rxmac, rxmac, sizeof(blob.rxmac) - 1);
    }
    EEPROM.put(kTxOff, blob);
    return EEPROM.commit();
}

void gpio_unhook(int pin) {
    detachInterrupt(digitalPinToInterrupt(pin));
}

bool gpio_setup_input(int pin, bool pull_up, void (*isr)(void*), void* arg) {
    gpio_isr_fn = isr;
    gpio_isr_arg = arg;
    gpio_pin_ = pin;
    pinMode(pin, pull_up ? INPUT_PULLUP : INPUT);
    gpio_last_level_ = digitalRead(pin);
    attachInterrupt(digitalPinToInterrupt(pin), &raw_gpio_isr, CHANGE);
    return true;
}

int gpio_read(int pin) {
    return digitalRead(pin);
}

bool espnow_start(RecvFn cb) {
    recv_cb = cb;
    (void)esp_now_deinit();
    return bind_espnow();
}

void espnow_on_wifi_change() {
    if (recv_cb == nullptr) {
        return;
    }
    (void)esp_now_deinit();
    (void)bind_espnow();
}

bool espnow_add_peer(const std::uint8_t mac[6], std::uint8_t channel, bool sta_if) {
    (void)sta_if;
    if (mac == nullptr) {
        return false;
    }
    std::uint8_t m[6];
    std::memcpy(m, mac, 6);
    std::uint8_t ch = channel;
    if (ch < 1 || ch > 13) {
        ch = wifi_get_channel();
    }
    if (ch < 1 || ch > 13) {
        ch = 1;
    }
    esp_now_del_peer(m);
    // COMBO: ESP32 peers have no ESP8266 role; SLAVE often makes unicast fail.
    return esp_now_add_peer(m, ESP_NOW_ROLE_COMBO, ch, nullptr, 0) == 0;
}

bool espnow_send(const std::uint8_t mac[6], const std::uint8_t* data, int len) {
    return esp_now_send(const_cast<std::uint8_t*>(mac), const_cast<std::uint8_t*>(data),
                        static_cast<std::uint8_t>(len)) == 0;
}

void wifi_set_ap_channel(std::uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return;
    }
    wifi_set_channel(channel);
    struct softap_config conf {};
    if (wifi_softap_get_config(&conf)) {
        conf.channel = channel;
        wifi_softap_set_config_current(&conf);
    }
}

std::uint8_t wifi_radio_channel(std::uint8_t fallback) {
    const std::uint8_t ch = wifi_get_channel();
    if (ch >= 1 && ch <= 13) {
        return ch;
    }
    return fallback;
}

void wifi_own_mac(std::uint8_t out[6], bool sta) {
    wifi_get_macaddr(sta ? STATION_IF : SOFTAP_IF, out);
}

bool timer_create(TimerId id, TimerFn cb, void* arg) {
    if (id < 0 || id > THello) {
        return false;
    }
    timers[id].cb = cb;
    timers[id].arg = arg;
    return true;
}

void LUMOS_DBPLAT_IRAM timer_stop(TimerId id) {
    if (id >= 0 && id <= THello) {
        timers[id].ticker.detach();
    }
}

void LUMOS_DBPLAT_IRAM timer_once(TimerId id, std::uint32_t us) {
    if (id < 0 || id > THello) {
        return;
    }
    timers[id].ticker.once_ms((us + 999) / 1000, fires[id]);
}

void timer_periodic(TimerId id, std::uint32_t us) {
    if (id < 0 || id > THello) {
        return;
    }
    timers[id].ticker.attach_ms((us + 999) / 1000, fires[id]);
}

bool spawn(TaskFn fn, void* arg) {
    if (queued_) {
        return false;
    }
    queued_fn = fn;
    queued_arg = arg;
    queued_ = true;
    return true;
}

void delay_ms(std::uint32_t ms) {
    const std::uint32_t start = millis();
    while (millis() - start < ms) {
        if (pump_fn != nullptr) {
            pump_fn();
        }
        yield();
    }
}

void task_exit() {}

void set_pump(void (*fn)()) {
    pump_fn = fn;
}

void poll() {
    if (gpio_pin_ >= 0) {
        const int level = digitalRead(gpio_pin_);
        if (gpio_last_level_ >= 0 && level != gpio_last_level_) {
            gpio_edge_ = true;
        }
        gpio_last_level_ = level;
    }
    if (gpio_edge_) {
        gpio_edge_ = false;
        if (gpio_isr_fn != nullptr) {
            gpio_isr_fn(gpio_isr_arg);
        }
    }
    if (queued_) {
        const TaskFn fn = queued_fn;
        void* arg = queued_arg;
        queued_ = false;
        if (fn != nullptr) {
            fn(arg);
        }
    }
}

} // namespace lumos::dbplat
