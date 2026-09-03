#include "lumos/doorbell/doorbell_transmitter.hpp"
#include "lumos/doorbell/doorbell_mac.hpp"
#include "lumos/doorbell/doorbell_packet.hpp"
#include "lumos/doorbell/doorbell_platform.hpp"
#include "lumos/core/logger.hpp"

#if defined(ESP_PLATFORM) && !defined(ARDUINO)
#include "esp_attr.h"
#endif

#include <algorithm>
#include <cstring>

namespace lumos {
namespace {

Logger log{"doorbell_tx"};

} // namespace

DoorbellTransmitter* DoorbellTransmitter::instance_ = nullptr;

Result<void> DoorbellTransmitter::start() {
    if (started_) {
        return Result<void>::ok();
    }
    instance_ = this;
    load_nvs();

    if (!dbplat::timer_create(dbplat::TDebounce, &DoorbellTransmitter::debounce_timer_cb, this) ||
        !dbplat::timer_create(dbplat::TRetry, &DoorbellTransmitter::retry_timer_cb, this) ||
        !dbplat::timer_create(dbplat::THello, &DoorbellTransmitter::hello_timer_cb, this)) {
        return Result<void>::fail(ErrorCode::IoError, "doorbell tx timer create failed");
    }

    if (!dbplat::espnow_start(&DoorbellTransmitter::recv_fn)) {
        log.error("esp_now_init failed");
        return Result<void>::fail(ErrorCode::IoError, "esp_now_init failed");
    }
    espnow_ready_ = true;
    configure_wifi_channel();
    ensure_broadcast_peer();
    add_peer();
    configure_gpio();
    started_ = true;
    log.info("doorbell tx ready pin=%d ch=%u rx=%s", cfg_.opto_pin,
             static_cast<unsigned>(cfg_.channel),
             cfg_.rx_mac_valid ? format_mac(cfg_.rx_mac).c_str() : "(unpaired)");
    return Result<void>::ok();
}

void DoorbellTransmitter::apply_config(const DoorbellTxConfig& cfg) {
    cfg_ = cfg;
    if (!is_safe_input_gpio(cfg_.opto_pin)) {
        log.warn("invalid opto pin %d; using %d", cfg_.opto_pin, kDefaultOptoGpio);
        cfg_.opto_pin = kDefaultOptoGpio;
    }
    cfg_.channel = static_cast<std::uint8_t>(std::clamp(static_cast<int>(cfg_.channel), 1, 13));
    if (started_) {
        if (!sta_linked_) {
            configure_wifi_channel();
        }
        add_peer();
        configure_gpio();
    }
}

void DoorbellTransmitter::load_nvs() {
    std::int32_t pin = cfg_.opto_pin;
    std::uint8_t ch = cfg_.channel;
    std::uint8_t al = cfg_.active_low ? 1 : 0;
    char mac[32]{};
    if (!dbplat::nvs_load(&pin, &ch, &al, &cfg_.tx_id, mac, sizeof(mac))) {
        return;
    }
    cfg_.opto_pin = static_cast<int>(pin);
    cfg_.channel = ch;
    cfg_.active_low = al != 0;
    if (mac[0] != '\0') {
        cfg_.rx_mac_valid = parse_mac(mac, cfg_.rx_mac);
    }
    if (!is_safe_input_gpio(cfg_.opto_pin)) {
        cfg_.opto_pin = kDefaultOptoGpio;
    }
    cfg_.channel = static_cast<std::uint8_t>(std::clamp(static_cast<int>(cfg_.channel), 1, 13));
}

void DoorbellTransmitter::save_nvs() {
    if (!dbplat::nvs_save(cfg_.opto_pin, cfg_.channel, cfg_.active_low ? 1 : 0, cfg_.tx_id,
                          cfg_.rx_mac_valid ? format_mac(cfg_.rx_mac).c_str() : "")) {
        log.error("nvs_open dbtx failed");
    }
}

void DoorbellTransmitter::test_send() {
    send_press(true);
}

DoorbellTxStatus DoorbellTransmitter::status() const {
    DoorbellTxStatus st;
    st.cfg = cfg_;
    st.own_mac = own_mac();
    st.espnow_ready = espnow_ready_;
    st.paired = cfg_.rx_mac_valid;
    st.last_seq = seq_;
    st.last_send_ms = last_send_ms_;
    st.pairing = pairing_active();
    st.scanning = scanning_;
    if (st.pairing) {
        const auto now = dbplat::now_us();
        st.pairing_ms = static_cast<std::uint32_t>((pairing_until_us_ - now) / 1000ULL);
    }
    st.peer_count = peer_count_;
    for (int i = 0; i < peer_count_ && i < kDoorbellMaxPeers; ++i) {
        st.peers[i] = peers_[i];
    }
    if (configured_pin_ >= 0) {
        st.opto_level = dbplat::gpio_read(configured_pin_);
    }
    return st;
}

void DoorbellTransmitter::start_pairing(std::uint32_t duration_ms) {
    if (!espnow_ready_ || scanning_) {
        return;
    }
    peer_count_ = 0;
    const auto ms = duration_ms == 0 ? kDoorbellPairDefaultMs : duration_ms;
    pairing_until_us_ = dbplat::now_us() + static_cast<std::uint64_t>(ms) * 1000ULL;
    ensure_broadcast_peer();
    dbplat::timer_stop(dbplat::THello);
    dbplat::timer_periodic(dbplat::THello, static_cast<std::uint32_t>(kDoorbellTxHelloPeriodUs));
    scanning_ = true;
    dbplat::spawn(&DoorbellTransmitter::scan_task, this);
    log.info("doorbell TX pairing / scan");
}

void DoorbellTransmitter::stop_pairing() {
    pairing_until_us_ = 0;
    dbplat::timer_stop(dbplat::THello);
}

bool DoorbellTransmitter::select_peer(const std::uint8_t mac[6]) {
    if (mac == nullptr) {
        return false;
    }
    std::uint8_t channel = sta_linked_ ? radio_channel() : cfg_.channel;
    for (int i = 0; i < peer_count_; ++i) {
        if (mac_equal(peers_[i].mac, mac)) {
            if (peers_[i].channel >= 1 && peers_[i].channel <= 13) {
                channel = peers_[i].channel;
            }
            break;
        }
    }
    auto cfg = cfg_;
    std::memcpy(cfg.rx_mac, mac, 6);
    cfg.rx_mac_valid = true;
    cfg.channel = channel;
    apply_config(cfg);
    save_nvs();
    dbplat::espnow_add_peer(mac, sta_linked_ ? 0 : channel, sta_linked_);
    send_pair(DOORBELL_PAIR_CLAIM, mac);
    stop_pairing();
    log.info("paired RX %s ch=%u", format_mac(mac).c_str(), static_cast<unsigned>(channel));
    return true;
}

void DoorbellTransmitter::set_sta_linked(bool linked) {
    sta_linked_ = linked;
    if (!started_ || !espnow_ready_) {
        return;
    }
    dbplat::espnow_on_wifi_change();
    const auto ch = radio_channel();
    if (ch >= 1 && ch <= 13) {
        cfg_.channel = ch;
    }
    ensure_broadcast_peer();
    add_peer();
    log.info("ESP-NOW if=%s ch=%u mac=%s", sta_linked_ ? "STA" : "AP",
             static_cast<unsigned>(cfg_.channel), own_mac().c_str());
}

std::string DoorbellTransmitter::own_mac() const {
    std::uint8_t mac[6]{};
    own_mac_bytes(mac);
    return format_mac(mac);
}

void DoorbellTransmitter::poll() {
    dbplat::poll();
}

#if defined(ESP_PLATFORM) && !defined(ARDUINO)
void IRAM_ATTR DoorbellTransmitter::gpio_isr(void* arg) {
#else
void DoorbellTransmitter::gpio_isr(void* arg) {
#endif
    auto* self = static_cast<DoorbellTransmitter*>(arg);
    if (self == nullptr) {
        return;
    }
#if defined(ARDUINO_ARCH_ESP8266)
    // Already deferred to loop(); Ticker debounce often misses short/AC opto pulses.
    self->maybe_fire();
#else
    dbplat::timer_stop(dbplat::TDebounce);
    dbplat::timer_once(dbplat::TDebounce, kDoorbellTxDebounceUs);
#endif
}

void DoorbellTransmitter::debounce_timer_cb(void* arg) {
    auto* self = static_cast<DoorbellTransmitter*>(arg);
    if (self != nullptr) {
        self->maybe_fire();
    }
}

void DoorbellTransmitter::retry_timer_cb(void* arg) {
    auto* self = static_cast<DoorbellTransmitter*>(arg);
    if (self == nullptr) {
        return;
    }
    if (self->retries_left_ <= 0) {
        return;
    }
    self->retries_left_--;
    self->send_press(false);
    if (self->retries_left_ > 0) {
        dbplat::timer_once(dbplat::TRetry, kDoorbellTxRetryUs);
    }
}

void DoorbellTransmitter::hello_timer_cb(void* arg) {
    auto* self = static_cast<DoorbellTransmitter*>(arg);
    if (self == nullptr) {
        return;
    }
    if (!self->pairing_active()) {
        self->stop_pairing();
        return;
    }
    if (self->scanning_) {
        return;
    }
    self->send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
}

void DoorbellTransmitter::recv_fn(const std::uint8_t mac[6], const std::uint8_t* data, int len,
                                 int rssi) {
    if (instance_ == nullptr || mac == nullptr || data == nullptr) {
        return;
    }
    instance_->on_packet(mac, data, len, rssi);
}

void DoorbellTransmitter::scan_task(void* arg) {
    auto* self = static_cast<DoorbellTransmitter*>(arg);
    if (self == nullptr) {
        dbplat::task_exit();
        return;
    }
    dbplat::delay_ms(350);
    self->ensure_broadcast_peer();
    if (self->sta_linked_) {
        for (int i = 0; i < 10 && self->pairing_active(); ++i) {
            self->send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
            dbplat::delay_ms(250);
        }
    } else {
        const auto home = self->cfg_.channel;
        for (std::uint8_t ch = 1; ch <= 13 && self->pairing_active(); ++ch) {
            self->set_ap_channel(ch);
            dbplat::espnow_add_peer(kDoorbellBroadcastMac, ch, self->sta_linked_);
            dbplat::delay_ms(40);
            self->send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
            dbplat::delay_ms(80);
            self->send_pair(DOORBELL_PAIR_HELLO, kDoorbellBroadcastMac);
            dbplat::delay_ms(200);
        }
        self->set_ap_channel(home);
        dbplat::espnow_add_peer(kDoorbellBroadcastMac, home, self->sta_linked_);
    }
    self->scanning_ = false;
    log.info("doorbell TX scan done, %d peer(s)", self->peer_count_);
    dbplat::task_exit();
}

void DoorbellTransmitter::maybe_fire() {
    // Edge is enough. Requiring the pin still active after debounce drops short
    // doorbell / AC optocoupler pulses.
    const auto now = static_cast<std::uint32_t>(dbplat::now_us() / 1000ULL);
    if (last_send_ms_ != 0 && (now - last_send_ms_) < kDoorbellTxCooldownMs) {
        return;
    }
    send_press(true);
    retries_left_ = kDoorbellTxRetries;
    dbplat::timer_stop(dbplat::TRetry);
    dbplat::timer_once(dbplat::TRetry, kDoorbellTxRetryUs);
}

void DoorbellTransmitter::send_press(bool bump_seq) {
    if (!espnow_ready_ || !cfg_.rx_mac_valid) {
        log.warn("doorbell press ignored (unpaired or esp-now down)");
        return;
    }
    if (bump_seq) {
        seq_++;
        if (seq_ == 0) {
            seq_ = 1;
        }
    }
    DoorbellPacket pkt{};
    pkt.magic = kDoorbellMagic;
    pkt.version = kDoorbellVersion;
    pkt.type = DOORBELL_PRESS;
    pkt.seq = seq_;
    pkt.reserved = 0;
    pkt.tx_id = cfg_.tx_id;
    last_send_ms_ = static_cast<std::uint32_t>(dbplat::now_us() / 1000ULL);
    add_peer();
    ensure_broadcast_peer();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&pkt);
    const int len = static_cast<int>(sizeof(pkt));
    // ESP32 STA accepts ESP32 unicast, but drops ESP8266 unicast to the STA MAC.
    // Pairing already uses broadcast and is heard; send press the same way.
    const bool uni = dbplat::espnow_send(cfg_.rx_mac, bytes, len);
    const bool bcast = dbplat::espnow_send(kDoorbellBroadcastMac, bytes, len);
    if (!uni && !bcast) {
        log.warn("esp_now_send failed seq=%u", static_cast<unsigned>(seq_));
    } else {
        log.info("sent DOORBELL_PRESS seq=%u to %s%s", static_cast<unsigned>(seq_),
                 format_mac(cfg_.rx_mac).c_str(), bcast ? " +bcast" : "");
    }
}

void DoorbellTransmitter::configure_gpio() {
    if (!is_safe_input_gpio(cfg_.opto_pin)) {
        return;
    }
    if (configured_pin_ >= 0 && configured_pin_ != cfg_.opto_pin) {
        dbplat::gpio_unhook(configured_pin_);
    }
    if (!dbplat::gpio_setup_input(cfg_.opto_pin, cfg_.active_low, &DoorbellTransmitter::gpio_isr,
                                  this)) {
        log.error("gpio setup failed");
        return;
    }
    configured_pin_ = cfg_.opto_pin;
}

void DoorbellTransmitter::configure_wifi_channel() {
    if (sta_linked_) {
        return;
    }
    set_ap_channel(cfg_.channel);
}

void DoorbellTransmitter::set_ap_channel(std::uint8_t channel) {
    dbplat::wifi_set_ap_channel(channel);
}

void DoorbellTransmitter::add_peer() {
    if (!espnow_ready_ || !cfg_.rx_mac_valid) {
        return;
    }
    if (!dbplat::espnow_add_peer(cfg_.rx_mac, sta_linked_ ? 0 : cfg_.channel, sta_linked_)) {
        log.warn("esp_now_add_peer failed for %s", format_mac(cfg_.rx_mac).c_str());
    }
}

void DoorbellTransmitter::ensure_broadcast_peer() {
    dbplat::espnow_add_peer(kDoorbellBroadcastMac, 0, sta_linked_);
}

void DoorbellTransmitter::send_pair(std::uint8_t type, const std::uint8_t* dest) {
    if (!espnow_ready_ || dest == nullptr) {
        return;
    }
    std::uint8_t mac[6]{};
    own_mac_bytes(mac);
    DoorbellPairHello pkt{};
    fill_pair_hello(pkt, type, DOORBELL_ROLE_TX, radio_channel(), mac, "LumosOS-Bell", cfg_.tx_id);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&pkt);
    const int len = static_cast<int>(sizeof(pkt));
    (void)dbplat::espnow_send(dest, bytes, len);
    if (!mac_equal(dest, kDoorbellBroadcastMac)) {
        (void)dbplat::espnow_send(kDoorbellBroadcastMac, bytes, len);
    }
}

void DoorbellTransmitter::on_packet(const std::uint8_t mac[6], const std::uint8_t* data, int len,
                                   int rssi) {
    DoorbellPairHello hello{};
    if (!parse_pair_hello(data, len, hello) || hello.role != DOORBELL_ROLE_RX) {
        return;
    }
    std::uint8_t self_mac[6]{};
    own_mac_bytes(self_mac);
    if (mac_equal(mac, self_mac)) {
        return;
    }
    if (!pairing_active() && !scanning_) {
        return;
    }
    note_peer(hello.mac, hello, rssi);
    if (hello.type == DOORBELL_PAIR_CLAIM) {
        select_peer(hello.mac);
    }
}

void DoorbellTransmitter::note_peer(const std::uint8_t mac[6], const DoorbellPairHello& hello,
                                   int rssi) {
    std::uint8_t zeros[6]{};
    const std::uint8_t* use = mac;
    if (mac_equal(mac, zeros)) {
        return;
    }
    for (int i = 0; i < peer_count_; ++i) {
        if (mac_equal(peers_[i].mac, use)) {
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
    std::memcpy(p.mac, use, 6);
    p.channel = hello.channel;
    p.role = hello.role;
    p.rssi = static_cast<std::int8_t>(rssi);
    std::memcpy(p.name, hello.name, sizeof(p.name));
}

void DoorbellTransmitter::own_mac_bytes(std::uint8_t out[6]) const {
    dbplat::wifi_own_mac(out, sta_linked_);
}

std::uint8_t DoorbellTransmitter::radio_channel() const {
    return dbplat::wifi_radio_channel(cfg_.channel);
}

bool DoorbellTransmitter::pairing_active() const {
    if (pairing_until_us_ == 0) {
        return false;
    }
    return dbplat::now_us() < pairing_until_us_;
}

} // namespace lumos
