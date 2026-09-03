#include "lumos/doorbell/doorbell_platform.hpp"
#include "lumos/doorbell/doorbell_tx_types.hpp"

#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <cstring>

namespace lumos::dbplat {
namespace {

esp_timer_handle_t timers[3]{};
RecvFn recv_cb{nullptr};

void idf_recv(const esp_now_recv_info_t* info, const std::uint8_t* data, int len) {
    if (recv_cb == nullptr || info == nullptr || data == nullptr) {
        return;
    }
    int rssi = 0;
    if (info->rx_ctrl != nullptr) {
        rssi = info->rx_ctrl->rssi;
    }
    recv_cb(info->src_addr, data, len, rssi);
}

} // namespace

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(esp_timer_get_time());
}

bool nvs_load(std::int32_t* pin, std::uint8_t* ch, std::uint8_t* alow, std::uint32_t* txid, char* rxmac,
              std::size_t rxmac_sz) {
    nvs_handle_t h{};
    if (nvs_open(kDoorbellTxNvsNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    if (pin != nullptr) {
        nvs_get_i32(h, "pin", pin);
    }
    if (ch != nullptr) {
        nvs_get_u8(h, "ch", ch);
    }
    if (alow != nullptr) {
        nvs_get_u8(h, "alow", alow);
    }
    if (txid != nullptr) {
        nvs_get_u32(h, "txid", txid);
    }
    if (rxmac != nullptr && rxmac_sz > 0) {
        std::size_t len = rxmac_sz;
        if (nvs_get_str(h, "rxmac", rxmac, &len) != ESP_OK) {
            rxmac[0] = '\0';
        }
    }
    nvs_close(h);
    return true;
}

bool nvs_save(std::int32_t pin, std::uint8_t ch, std::uint8_t alow, std::uint32_t txid,
              const char* rxmac) {
    nvs_handle_t h{};
    if (nvs_open(kDoorbellTxNvsNs, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_i32(h, "pin", pin);
    nvs_set_u8(h, "ch", ch);
    nvs_set_u8(h, "alow", alow);
    nvs_set_u32(h, "txid", txid);
    nvs_set_str(h, "rxmac", rxmac != nullptr ? rxmac : "");
    nvs_commit(h);
    nvs_close(h);
    return true;
}

void gpio_unhook(int pin) {
    gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
}

bool gpio_setup_input(int pin, bool pull_up, void (*isr)(void*), void* arg) {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    if (gpio_config(&io) != ESP_OK) {
        return false;
    }
    static bool isr_installed = false;
    if (!isr_installed) {
        const esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return false;
        }
        isr_installed = true;
    }
    gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
    return gpio_isr_handler_add(static_cast<gpio_num_t>(pin), isr, arg) == ESP_OK;
}

int gpio_read(int pin) {
    return gpio_get_level(static_cast<gpio_num_t>(pin));
}

bool espnow_start(RecvFn cb) {
    recv_cb = cb;
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    return esp_now_register_recv_cb(&idf_recv) == ESP_OK;
}

bool espnow_add_peer(const std::uint8_t mac[6], std::uint8_t channel, bool sta_if) {
    if (mac == nullptr) {
        return false;
    }
    esp_now_del_peer(mac);
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = sta_if ? WIFI_IF_STA : WIFI_IF_AP;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

bool espnow_send(const std::uint8_t mac[6], const std::uint8_t* data, int len) {
    return esp_now_send(mac, data, len) == ESP_OK;
}

void espnow_on_wifi_change() {}

void wifi_set_ap_channel(std::uint8_t channel) {
    wifi_config_t ap{};
    if (esp_wifi_get_config(WIFI_IF_AP, &ap) == ESP_OK) {
        ap.ap.channel = channel;
        (void)esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
}

std::uint8_t wifi_radio_channel(std::uint8_t fallback) {
    std::uint8_t primary = fallback;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary >= 1 && primary <= 13) {
        return primary;
    }
    return fallback;
}

void wifi_own_mac(std::uint8_t out[6], bool sta) {
    esp_read_mac(out, sta ? ESP_MAC_WIFI_STA : ESP_MAC_WIFI_SOFTAP);
}

bool timer_create(TimerId id, TimerFn cb, void* arg) {
    if (id < 0 || id > THello) {
        return false;
    }
    const esp_timer_create_args_t args{
        .callback = cb,
        .arg = arg,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dbtx_t",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&args, &timers[id]) == ESP_OK;
}

void LUMOS_DBPLAT_IRAM timer_stop(TimerId id) {
    if (id >= 0 && id <= THello && timers[id] != nullptr) {
        esp_timer_stop(timers[id]);
    }
}

void LUMOS_DBPLAT_IRAM timer_once(TimerId id, std::uint32_t us) {
    if (id >= 0 && id <= THello && timers[id] != nullptr) {
        esp_timer_start_once(timers[id], us);
    }
}

void timer_periodic(TimerId id, std::uint32_t us) {
    if (id >= 0 && id <= THello && timers[id] != nullptr) {
        esp_timer_start_periodic(timers[id], us);
    }
}

bool spawn(TaskFn fn, void* arg) {
    return xTaskCreate(fn, "dbtx_scan", 4096, arg, 5, nullptr) == pdPASS;
}

void delay_ms(std::uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void task_exit() {
    vTaskDelete(nullptr);
}

void set_pump(void (*)()) {}

void poll() {}

} // namespace lumos::dbplat
