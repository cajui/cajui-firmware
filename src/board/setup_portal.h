#pragma once
#include "cajui_setup.h"
#include "mqtt_uplink.h"
#include <DNSServer.h>
#include <WebServer.h>
#include <atomic>

namespace board {
// PRG button on the Heltec board. Final hardware can wire an external button to any free
// GPIO with a pull-up and set this constant.
constexpr uint8_t SetupButton = 0;
constexpr uint32_t SetupHoldMs = 3000;

// Receiver administration page on a temporary access point, opened by the button.
// TODO(security): the access point is open. Anyone nearby while it is open can change
// the Wi-Fi/broker settings or revoke transmitters. Add a per-device password (label/QR).
class SetupPortal {
public:
    using Apply = bool (*)(const cajui::UplinkConfig&);
    SetupPortal(cajui::PersistentStore& store, MqttUplink& uplink, cajui::AtomicBlob& uplinkBlob,
                Apply apply)
        : store_(store), uplink_(uplink), blob_(uplinkBlob), apply_(apply) {}
    SetupPortal(const SetupPortal&) = delete;
    SetupPortal& operator=(const SetupPortal&) = delete;
    void open();
    void close();
    bool active() const { return active_; }
    // quiet=false defers HTTP handling, which may write flash, while the radio is busy.
    void poll(bool quiet);

private:
    cajui::PersistentStore& store_;
    MqttUplink& uplink_;
    cajui::AtomicBlob& blob_;
    Apply apply_;
    WebServer server_{80};
    DNSServer dns_;
    bool active_ = false, routed_ = false, mdns_ = false, saved_ = false;
    char ssid_[cajui::SetupSsidCapacity]{};
    cajui::UplinkConfig pending_{};
    cajui::WifiState wifi_ = cajui::WifiState::Idle;
    uint32_t wifiSince_ = 0, lastActivity_ = 0, closeAt_ = 0, settleAt_ = 0, scanRetryAt_ = 0,
             scanStartedAt_ = 0;
    uint8_t scanFailures_ = 0;
    bool closing_ = false, scanning_ = false, settling_ = false, scanPending_ = false,
         discoverPending_ = false, scanRetryDue_ = false, reconnect_ = false;
    cajui::NetworkView networks_[cajui::MaxNetworks]{};
    size_t networkCount_ = 0;
    // Written by the discovery task, read by the loop after searching_ turns false.
    cajui::BrokerView brokers_[cajui::MaxBrokers]{};
    std::atomic<size_t> brokerCount_{0};
    std::atomic<bool> searching_{false};
    cajui::EnrollmentInfo transmitters_[cajui::BindingCapacity]{};
    const char* notice_ = nullptr;
    char page_[cajui::PageCapacity]{};
    void route();
    void touch() { lastActivity_ = millis(); }
    void redirect(const char* notice);
    void home();
    void save();
    void scan();
    void discover();
    void startMdns();
    void collectScan();
    void trackWifi();
    static void discoveryTask(void* self);
};
} // namespace board
