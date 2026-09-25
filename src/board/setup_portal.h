// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_pairing.h"
#include "cajui_setup.h"
#include "common.h"
#include "mqtt_uplink.h"
#include <DNSServer.h>
#include <WebServer.h>
#include <atomic>

namespace board {
// PRG button on the Heltec board. Final hardware can wire an external button to any free
// GPIO with a pull-up and set this constant.
constexpr uint8_t SetupButton = 0;
constexpr uint32_t SetupHoldMs = 3000;

// What the setup page asks of the receiver application.
class ReceiverControl {
public:
    virtual ~ReceiverControl() = default;
    // Replaces the broker connection with these settings. Called without the lock held.
    virtual bool applyUplink(const cajui::UplinkConfig&) = 0;
};

// Receiver administration page on a temporary access point, opened by the button.
// It runs in its own task, so a slow or idle HTTP client never delays the radio loop.
// The store, pairing host and uplink record are shared with that loop and are used only
// with the application lock held; storage writes also wait until the radio is listening.
// The page answers only on the access point interface, only for its own address, and
// every form carries the session token (docs/radio-applications.md).
// TODO(security): the access point is open. Anyone nearby who joins it while it is open
// can change the Wi-Fi/broker settings or revoke transmitters. Add a per-device password
// (label/QR).
class SetupPortal {
public:
    SetupPortal(cajui::PersistentStore& store, MqttUplink& uplink, cajui::AtomicBlob& uplinkBlob,
                ReceiverControl& control, AppLock& lock, cajui::Entropy& entropy)
        : store_(store), uplink_(uplink), blob_(uplinkBlob), control_(control), lock_(lock),
          entropy_(entropy) {}
    SetupPortal(const SetupPortal&) = delete;
    SetupPortal& operator=(const SetupPortal&) = delete;
    // Optional: enables adding transmitters by radio pairing from the page.
    void setPairing(cajui::PairingHost* pairing) { pairing_ = pairing; }
    // Starts the task that watches the button and serves the page. `radioIdle` is true
    // while the receiver listens; it is written by the loop with the lock held.
    bool start(const std::atomic<bool>& radioIdle);

private:
    cajui::PersistentStore& store_;
    MqttUplink& uplink_;
    cajui::AtomicBlob& blob_;
    ReceiverControl& control_;
    AppLock& lock_;
    cajui::Entropy& entropy_;
    const std::atomic<bool>* radioIdle_ = nullptr;
    cajui::PairingHost* pairing_ = nullptr;
    WebServer server_{80};
    DNSServer dns_;
    cajui::SetupSession session_;
    char ssid_[cajui::SetupSsidCapacity]{};
    char address_[sizeof("255.255.255.255")]{};
    // Staged settings, and the Wi-Fi credentials the station was last started with.
    cajui::UplinkConfig pending_{}, running_{};
    bool active_ = false, routed_ = false, stored_ = false, savedCurrent_ = false;
    bool trial_ = false, trialFailed_ = false, reconnect_ = false;
    bool mdns_ = false, mdnsEndPending_ = false;
    bool closing_ = false, scanning_ = false, settling_ = false, scanPending_ = false,
         discoverPending_ = false, scanRetryDue_ = false;
    cajui::WifiState wifi_ = cajui::WifiState::Idle;
    uint32_t wifiSince_ = 0, closeAt_ = 0, settleAt_ = 0, scanRetryAt_ = 0, scanStartedAt_ = 0;
    uint8_t scanFailures_ = 0;
    cajui::NetworkView networks_[cajui::MaxNetworks]{};
    size_t networkCount_ = 0;
    // Written by the discovery task, read by the portal task after searching_ turns false.
    cajui::BrokerView brokers_[cajui::MaxBrokers]{};
    std::atomic<size_t> brokerCount_{0};
    std::atomic<bool> searching_{false};
    cajui::EnrollmentInfo transmitters_[cajui::BindingCapacity]{};
    char page_[cajui::PageCapacity]{};
    static void task(void* self);
    void run();
    void open();
    void close();
    void poll();
    void route();
    bool authorize(bool post);
    void redirect(cajui::Notice);
    void home();
    void transmitters();
    void fillPairing(cajui::PairingView&) const;
    void fillTransmitters(cajui::SetupView&);
    bool verified() const;
    bool save();
    void restoreStoredWifi();
    void scan();
    void discover();
    void startMdns();
    void collectScan();
    void trackWifi();
    static void discoveryTask(void* self);
    // Runs `action` with the lock held once the radio is listening (or after a bound).
    template <typename Action> auto whileRadioIdle(Action action) -> decltype(action());
};
} // namespace board
