#include "lumos/doorbell/doorbell_receiver.hpp"
#include "lumos/doorbell/doorbell_packet.hpp"
#include "lumos/doorbell/doorbell_mac.hpp"
#include "lumos/core/board_pins.hpp"
#include "lumos/core/logger.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lumos {
namespace {

Logger log{"doorbell"};

constexpr std::uint16_t kMinPressMs = 100;
constexpr std::uint16_t kMaxPressMs = 4000;
constexpr std::uint64_t kHelloPeriodUs = 400 * 1000;
constexpr ledc_mode_t kToneMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kToneTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kToneChannel = LEDC_CHANNEL_0;
constexpr std::uint32_t kToneHz = 2500;
constexpr ledc_timer_bit_t kToneRes = LEDC_TIMER_10_BIT;
constexpr std::uint32_t kToneDutyOn = 512; // 50%

bool add_peer(const std::uint8_t mac[6], std::uint8_t channel, wifi_interface_t ifidx) {
    esp_now_del_peer(mac);
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = ifidx;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

} // namespace

DoorbellReceiver* DoorbellReceiver::instance_ = nullptr;

DoorbellReceiver::DoorbellReceiver(Preferences& preferences) : preferences_(preferences) {}

Result<void> DoorbellReceiver::start() {
    if (started_) {
        return Result<void>::ok();
    }
    instance_ = this;

    const esp_timer_create_args_t targs{
        .callback = &DoorbellReceiver::release_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "doorbell_rel",
        .skip_unhandled_events = true,
    };
    const esp_timer_create_args_t hello_args{
        .callback = &DoorbellReceiver::hello_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "doorbell_hi",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&targs, &release_timer_) != ESP_OK ||
        esp_timer_create(&hello_args, &hello_timer_) != ESP_OK) {
        return Result<void>::fail(ErrorCode::IoError, "doorbell timer create failed");
    }

    apply_settings();

    if (esp_now_init() != ESP_OK) {
        log.error("esp_now_init failed");
        return Result<void>::fail(ErrorCode::IoError, "esp_now_init failed");
    }
    if (esp_now_register_recv_cb(&DoorbellReceiver::recv_cb) != ESP_OK) {
        log.error("esp_now_register_recv_cb failed");
        return Result<void>::fail(ErrorCode::IoError, "esp_now recv cb failed");
    }
    espnow_ready_ = true;
    started_ = true;
    log.info("doorbell receiver ready (target=%s pin=%d)", kIdfTargetName,
             preferences_.device().doorbell.relay_pin);
    return Result<void>::ok();
}

void DoorbellReceiver::apply_settings() {
    auto& db = preferences_.device().doorbell;
    if (db.relay_pin == 0) {
        db.relay_pin = kDefaultRelayGpio;
    }
    if (!is_safe_output_gpio(db.relay_pin)) {
        log.warn("invalid relay pin %d on %s; using %d", db.relay_pin, kIdfTargetName,
                 kDefaultRelayGpio);
        db.relay_pin = kDefaultRelayGpio;
    }
    db.press_ms = static_cast<std::uint16_t>(
        std::clamp(static_cast<int>(db.press_ms), static_cast<int>(kMinPressMs),
                   static_cast<int>(kMaxPressMs)));

    paired_valid_ = parse_mac(db.paired_tx_mac, paired_mac_);
    if (!db.paired_tx_mac.empty() && !paired_valid_) {
        log.warn("paired_tx_mac invalid: %s", db.paired_tx_mac.c_str());
    }

    // Drop dedupe state when pairing changes.
    have_last_seq_ = false;

    if (relay_active_ && release_timer_ != nullptr) {
        esp_timer_stop(release_timer_);
        relay_active_ = false;
    }
    // Do not touch pads at boot/settings load. A shorted or JTAG pin
    // (this board died on GPIO 13) trips the interrupt WDT and bootloops.
    configured_pin_ = -1;
    tone_ready_ = false;
}

void DoorbellReceiver::test_pulse() {
    const auto& db = preferences_.device().doorbell;
    log.info("test pulse scheduled pin=%d tone=%d", db.relay_pin, db.tone ? 1 : 0);
    // Run off the HTTP task so the TCP response can flush, and off Wi-Fi's core.
    if (xTaskCreatePinnedToCore(&DoorbellReceiver::test_task, "db_buzz", 4096, this, 5, nullptr,
                                1) != pdPASS) {
        log.error("test task create failed — buzzing inline");
        run_test_buzz();
    }
}

void DoorbellReceiver::test_task(void* arg) {
    auto* self = static_cast<DoorbellReceiver*>(arg);
    vTaskDelay(pdMS_TO_TICKS(150));
    if (self != nullptr) {
        self->run_test_buzz();
    }
    vTaskDelete(nullptr);
}

void DoorbellReceiver::run_test_buzz() {
    auto& db = preferences_.device().doorbell;
    if (!is_safe_output_gpio(db.relay_pin)) {
        log.warn("test buzz skipped — pin %d not a safe output", db.relay_pin);
        return;
    }
    const gpio_num_t gpio = static_cast<gpio_num_t>(db.relay_pin);
    log.info("test buzz start pin=%d (DC LOW then HIGH, 800ms each)", db.relay_pin);

    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << db.relay_pin;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        log.error("test buzz gpio %d config failed", db.relay_pin);
        return;
    }
    (void)gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_2);
    configured_pin_ = db.relay_pin;

    // 3-pin active modules (GND/VCC/SIG) want a steady level, not PWM.
    // Try LOW first (common "low-level trigger"), then HIGH.
    log.info("test buzz SIG LOW 800ms");
    gpio_set_level(gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(800));
    log.info("test buzz SIG HIGH 800ms");
    gpio_set_level(gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(800));
    gpio_set_level(gpio, db.active_high ? 0 : 1);
    last_ring_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
    relay_active_ = false;
    log.info("test buzz done last_ring_ms=%u", static_cast<unsigned>(last_ring_ms_));
}

DoorbellStatus DoorbellReceiver::status() const {
    const auto& db = preferences_.device().doorbell;
    DoorbellStatus st;
    st.enabled = db.enabled;
    st.espnow_ready = espnow_ready_;
    st.paired = paired_valid_;
    st.relay_pin = db.relay_pin;
    st.active_high = db.active_high;
    st.tone = db.tone;
    st.press_ms = db.press_ms;
    st.paired_tx_mac = db.paired_tx_mac;
    std::uint8_t mac[6]{};
    own_mac_bytes(mac);
    st.own_mac = format_mac(mac);
    st.wifi_channel = current_channel();
    st.last_ring_ms = last_ring_ms_;
    st.last_seq = last_seq_;
    st.relay_active = relay_active_;
    st.pairing = pairing_active();
    if (st.pairing) {
        const auto now = static_cast<std::uint64_t>(esp_timer_get_time());
        st.pairing_ms = static_cast<std::uint32_t>((pairing_until_us_ - now) / 1000ULL);
    }
    st.peer_count = peer_count_;
    for (int i = 0; i < peer_count_ && i < kDoorbellMaxPeers; ++i) {
        st.peers[i] = peers_[i];
    }
    return st;
}

void DoorbellReceiver::start_pairing(std::uint32_t duration_ms) {
    if (!espnow_ready_) {
        return;
    }
    peer_count_ = 0;
    const auto ms = duration_ms == 0 ? kDoorbellPairDefaultMs : duration_ms;
    pairing_until_us_ = static_cast<std::uint64_t>(esp_timer_get_time()) +
                        static_cast<std::uint64_t>(ms) * 1000ULL;
    ensure_broadcast_peer();
    if (hello_timer_ != nullptr) {
        esp_timer_stop(hello_timer_);
        esp_timer_start_periodic(hello_timer_, kHelloPeriodUs);
    }
    send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
    log.info("doorbell pairing for %u ms on ch=%u", static_cast<unsigned>(ms),
             static_cast<unsigned>(current_channel()));
}

void DoorbellReceiver::stop_pairing() {
    pairing_until_us_ = 0;
    if (hello_timer_ != nullptr) {
        esp_timer_stop(hello_timer_);
    }
}

bool DoorbellReceiver::select_peer(const std::uint8_t mac[6]) {
    if (mac == nullptr) {
        return false;
    }
    preferences_.device().doorbell.paired_tx_mac = format_mac(mac);
    preferences_.device().doorbell.enabled = true;
    preferences_.save();
    apply_settings();
    add_peer(mac, 0, WIFI_IF_STA);
    send_pair(DOORBELL_PAIR_CLAIM, mac);
    stop_pairing();
    log.info("paired TX %s", format_mac(mac).c_str());
    return true;
}

void DoorbellReceiver::recv_cb(const esp_now_recv_info_t* info, const std::uint8_t* data, int len) {
    if (instance_ == nullptr || info == nullptr || data == nullptr) {
        return;
    }
    int rssi = 0;
    if (info->rx_ctrl != nullptr) {
        rssi = info->rx_ctrl->rssi;
    }
    instance_->on_packet(info->src_addr, data, len, rssi);
}

void DoorbellReceiver::release_timer_cb(void* arg) {
    auto* self = static_cast<DoorbellReceiver*>(arg);
    if (self != nullptr) {
        self->set_relay(false);
    }
}

void DoorbellReceiver::hello_timer_cb(void* arg) {
    auto* self = static_cast<DoorbellReceiver*>(arg);
    if (self == nullptr) {
        return;
    }
    if (!self->pairing_active()) {
        self->stop_pairing();
        return;
    }
    self->send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
}

void DoorbellReceiver::on_packet(const std::uint8_t mac[6], const std::uint8_t* data, int len,
                                int rssi) {
    if (!espnow_ready_ || len < static_cast<int>(sizeof(DoorbellPacket))) {
        return;
    }

    DoorbellPairHello hello{};
    if (parse_pair_hello(data, len, hello)) {
        handle_pair(mac, hello, rssi);
        return;
    }

    const auto& db = preferences_.device().doorbell;
    if (!db.enabled || !paired_valid_) {
        return;
    }
    if (!mac_equal(mac, paired_mac_)) {
        return;
    }

    DoorbellPacket pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));
    if (pkt.magic != kDoorbellMagic || pkt.version != kDoorbellVersion) {
        return;
    }
    if (pkt.type != DOORBELL_PRESS) {
        return;
    }
    if (have_last_seq_ && pkt.seq == last_seq_) {
        log.debug("duplicate seq %u ignored", static_cast<unsigned>(pkt.seq));
        return;
    }

    have_last_seq_ = true;
    last_seq_ = pkt.seq;
    log.info("doorbell press from %s seq=%u", format_mac(mac).c_str(),
             static_cast<unsigned>(pkt.seq));
    pulse_relay();
}

void DoorbellReceiver::handle_pair(const std::uint8_t src[6], const DoorbellPairHello& hello,
                                  int rssi) {
    std::uint8_t self_mac[6]{};
    own_mac_bytes(self_mac);
    if (mac_equal(src, self_mac)) {
        return;
    }
    if (hello.role != DOORBELL_ROLE_TX) {
        return;
    }
    if (!pairing_active()) {
        return;
    }
    note_peer(src, hello, rssi);
    if (hello.type == DOORBELL_PAIR_CLAIM) {
        select_peer(src);
    }
}

void DoorbellReceiver::note_peer(const std::uint8_t mac[6], const DoorbellPairHello& hello,
                                int rssi) {
    for (int i = 0; i < peer_count_; ++i) {
        if (mac_equal(peers_[i].mac, mac)) {
            peers_[i].channel = hello.channel;
            peers_[i].role = hello.role;
            peers_[i].rssi = static_cast<std::int8_t>(rssi);
            std::memcpy(peers_[i].name, hello.name, sizeof(peers_[i].name));
            return;
        }
    }
    if (peer_count_ >= kDoorbellMaxPeers) {
        return;
    }
    auto& p = peers_[peer_count_++];
    std::memcpy(p.mac, mac, 6);
    p.channel = hello.channel;
    p.role = hello.role;
    p.rssi = static_cast<std::int8_t>(rssi);
    std::memcpy(p.name, hello.name, sizeof(p.name));
}

void DoorbellReceiver::pulse_relay(std::uint16_t override_ms) {
    const auto& db = preferences_.device().doorbell;
    set_relay(true);
    last_ring_ms_ = static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
    if (release_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(release_timer_);
    const std::uint16_t ms =
        override_ms > 0 ? override_ms
                        : static_cast<std::uint16_t>(std::max<std::uint16_t>(db.press_ms, kMinPressMs));
    const std::uint64_t us = static_cast<std::uint64_t>(ms) * 1000ULL;
    if (esp_timer_start_once(release_timer_, us) != ESP_OK) {
        log.error("failed to start relay release timer");
        set_relay(false);
    }
}

void DoorbellReceiver::set_relay(bool active) {
    const auto& db = preferences_.device().doorbell;
    if (configured_pin_ != db.relay_pin) {
        configure_gpio();
    }
    const int pin = configured_pin_ >= 0 ? configured_pin_ : db.relay_pin;
    if (!is_safe_output_gpio(pin)) {
        return;
    }
    if (db.tone) {
        if (active) {
            start_tone();
        } else {
            stop_tone();
        }
    } else {
        const int level = db.active_high ? (active ? 1 : 0) : (active ? 0 : 1);
        if (gpio_set_level(static_cast<gpio_num_t>(pin), level) != ESP_OK) {
            log.error("relay gpio %d set_level(%d) failed", pin, level);
        }
    }
    relay_active_ = active;
}

void DoorbellReceiver::ensure_tone(int pin) {
    ledc_timer_config_t timer{};
    timer.speed_mode = kToneMode;
    timer.duty_resolution = kToneRes;
    timer.timer_num = kToneTimer;
    timer.freq_hz = kToneHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        log.error("buzzer LEDC timer failed");
        tone_ready_ = false;
        return;
    }

    ledc_channel_config_t ch{};
    ch.gpio_num = pin;
    ch.speed_mode = kToneMode;
    ch.channel = kToneChannel;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = kToneTimer;
    ch.duty = 0;
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) {
        log.error("buzzer LEDC channel failed");
        tone_ready_ = false;
        return;
    }
    tone_ready_ = true;
}

void DoorbellReceiver::start_tone() {
    const int pin = configured_pin_ >= 0 ? configured_pin_ : preferences_.device().doorbell.relay_pin;
    if (!tone_ready_) {
        ensure_tone(pin);
    }
    if (!tone_ready_) {
        return;
    }
    ledc_set_duty(kToneMode, kToneChannel, kToneDutyOn);
    ledc_update_duty(kToneMode, kToneChannel);
}

void DoorbellReceiver::stop_tone() {
    if (!tone_ready_) {
        return;
    }
    const int idle = preferences_.device().doorbell.active_high ? 0 : 1;
    ledc_stop(kToneMode, kToneChannel, idle);
}

void DoorbellReceiver::configure_gpio() {
    const auto& db = preferences_.device().doorbell;
    const int pin = db.relay_pin;
    if (!is_safe_output_gpio(pin)) {
        return;
    }

    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);

    if (configured_pin_ >= 0 && configured_pin_ != pin) {
        if (tone_ready_) {
            ledc_stop(kToneMode, kToneChannel, 0);
            tone_ready_ = false;
        }
        gpio_set_direction(static_cast<gpio_num_t>(configured_pin_), GPIO_MODE_DISABLE);
    }

    configured_pin_ = pin;

    if (db.tone) {
        ensure_tone(pin);
        stop_tone();
        relay_active_ = false;
        return;
    }

    if (tone_ready_) {
        ledc_stop(kToneMode, kToneChannel, 0);
        tone_ready_ = false;
    }

    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        log.error("relay gpio %d config failed", pin);
        return;
    }
    // Weaker drive: a GPIO shorted to GND or a raw coil will brown out USB power
    // less violently than the default ~20–40 mA pad.
    (void)gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_1);
    const int idle = db.active_high ? 0 : 1;
    (void)gpio_set_level(gpio, idle);
    relay_active_ = false;
}

void DoorbellReceiver::send_pair(std::uint8_t type, const std::uint8_t* dest) {
    if (!espnow_ready_ || dest == nullptr) {
        return;
    }
    std::uint8_t mac[6]{};
    own_mac_bytes(mac);
    DoorbellPairHello pkt{};
    std::string name = preferences_.device().hostname;
    if (name.empty()) {
        name = "LumosOS";
    }
    fill_pair_hello(pkt, type, DOORBELL_ROLE_RX, current_channel(), mac, name.c_str(), 1);
    (void)esp_now_send(dest, reinterpret_cast<const std::uint8_t*>(&pkt), sizeof(pkt));
}

void DoorbellReceiver::ensure_broadcast_peer() {
    add_peer(kDoorbellBroadcastMac, 0, WIFI_IF_STA);
}

bool DoorbellReceiver::pairing_active() const {
    if (pairing_until_us_ == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(esp_timer_get_time()) < pairing_until_us_;
}

std::uint8_t DoorbellReceiver::current_channel() const {
    std::uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return 0;
    }
    return primary;
}

void DoorbellReceiver::own_mac_bytes(std::uint8_t out[6]) const {
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}

bool DoorbellReceiver::parse_paired_mac(std::uint8_t out[6]) const {
    return parse_mac(preferences_.device().doorbell.paired_tx_mac, out);
}

std::string DoorbellReceiver::format_mac(const std::uint8_t mac[6]) {
    return lumos::format_mac(mac);
}

bool DoorbellReceiver::mac_equal(const std::uint8_t a[6], const std::uint8_t b[6]) {
    return lumos::mac_equal(a, b);
}

} // namespace lumos
