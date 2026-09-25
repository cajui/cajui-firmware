// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
// Receiver image: listens for enrolled transmitters, queues their samples durably, forwards
// them to MQTT and serves the setup page. See docs/radio-applications.md.
#include <Arduino.h>
#include <esp_random.h>
#include <atomic>
#include <cstdlib>
#include <memory>
#include "cajui_application.h"
#include "cajui_device.h"
#include "cajui_nvs.h"
#include "cajui_pairing.h"
#include "cajui_provisioning.h"
#include "cajui_uplink.h"
#include "board/admin_console.h"
#include "board/common.h"
#include "board/mqtt_uplink.h"
#include "board/setup_portal.h"
#include "board/sx1262_radio.h"

namespace {
using namespace board;

class ReceiverApp final : public ReceiverControl {
public:
    ReceiverApp() : store_(records_, cajui::Role::Receiver, deviceId()) {}
    void setup();
    void loop();
    bool applyUplink(const cajui::UplinkConfig&) override;

private:
    BoardClock clock_;
    BoardEntropy entropy_{true}; // Pairing and the setup page run with Wi-Fi on.
    Sx1262Radio radio_;
    cajui::NvsRecords records_;
    cajui::RecordBlob uplinkBlob_{records_, "uplink", cajui::MinUplinkSize,
                                  cajui::UplinkBlobCapacity};
    cajui::RecordBlob radioBlob_{records_, "radio", cajui::RadioRecordSize, cajui::RadioRecordSize};
    cajui::PersistentStore store_;
    MqttUplink uplink_;
    AppLock lock_;
    std::atomic<bool> listening_{false};
    cajui::ReceiverController controller_{radio_, clock_, store_};
    cajui::PairingHost pairing_{store_, entropy_, clock_};
    SetupPortal portal_{store_, uplink_, uplinkBlob_, *this, lock_, entropy_};
    std::unique_ptr<cajui::Forwarder> forwarder_;
    std::unique_ptr<cajui::Provisioning> commands_;
    std::unique_ptr<Console> console_;
    bool adminMode_ = false, running_ = false, restartPending_ = false, healthy_ = false;
    uint32_t startedAt_ = 0, retryAt_ = 0, reportedForwards_ = 0, reportedRetries_ = 0;
    bool reportedOnline_ = false;
    void startForwarding();
    void reportForwarding();
    void fault(const char* reason);
};

// Forwarding is optional: without stored settings the receiver keeps queueing.
void ReceiverApp::startForwarding() {
    cajui::UplinkConfig settings{};
    const auto loaded = cajui::loadUplink(uplinkBlob_, settings);
    if (loaded != cajui::ReadResult::Ok) {
        Serial.println(loaded == cajui::ReadResult::Missing ? "CJAPP UPLINK disabled"
                                                            : "CJAPP UPLINK config_error");
        return;
    }
    if (uplink_.begin(settings, store_.device())) {
        forwarder_.reset(new cajui::Forwarder(uplink_, clock_, store_, settings.username));
        Serial.printf("CJAPP UPLINK started host=%s port=%u source=%s\n", settings.host,
                      unsigned(settings.port), settings.username);
    } else {
        Serial.println("CJAPP UPLINK start_failed");
    }
    cajui::wipe(settings);
}
// Called by the setup page after saving. Publishing pauses while the client is replaced,
// so no sample is published with the old connection under the new source.
bool ReceiverApp::applyUplink(const cajui::UplinkConfig& settings) {
    {
        Locked held(lock_);
        if (forwarder_) forwarder_->pause();
    }
    const bool started = uplink_.startMqtt(settings, store_.device());
    Locked held(lock_);
    if (forwarder_) return forwarder_->setSource(settings.username) && started;
    if (!started) return false;
    forwarder_.reset(new cajui::Forwarder(uplink_, clock_, store_, settings.username));
    return forwarder_->state() != cajui::ForwardState::Failed;
}
void ReceiverApp::reportForwarding() {
    if (uplink_.connected() != reportedOnline_) {
        reportedOnline_ = uplink_.connected();
        Serial.printf("CJAPP UPLINK %s wifi=%u\n", reportedOnline_ ? "online" : "offline",
                      unsigned(MqttUplink::wifiConnected()));
    }
    if (forwarder_->forwarded() != reportedForwards_) {
        reportedForwards_ = forwarder_->forwarded();
        Serial.printf("CJAPP FORWARD puback total=%u queued=%u\n", unsigned(reportedForwards_),
                      unsigned(store_.queued()));
    }
    if (forwarder_->retries() != reportedRetries_) {
        reportedRetries_ = forwarder_->retries();
        Serial.printf("CJAPP FORWARD retry total=%u\n", unsigned(reportedRetries_));
    }
}
// The receiver stops its radio and restarts after a growing delay; the USB console stays
// available meanwhile. Restarting remounts storage, the documented recovery.
void ReceiverApp::fault(const char* reason) {
    const uint32_t faults = recordFault();
    const uint32_t delay = cajui::retryDelayMs(faults);
    Serial.printf("CJAPP STOP %s faults=%u restart_s=%u\n", reason, unsigned(faults),
                  unsigned(delay / 1000));
    radio_.sleep();
    output(board::RadioReset, LOW);
    output(board::Vext, HIGH);
    running_ = false;
    listening_.store(false);
    retryAt_ = millis() + delay;
}
void ReceiverApp::setup() {
    startBoard();
    const bool ready = lock_.begin() && uplink_.prepare();
    const cajui::BootRequest request = takeBootRequest();
    const bool opened = records_.begin();
    const bool mounted = opened && store_.mount();
    const auto decision =
        cajui::decideBoot(store_, mounted, RadioProfile, request, /*pairHeld=*/false);
    adminMode_ = decision.mode == cajui::BootMode::Admin;
    commands_.reset(new cajui::Provisioning(store_, esp_random(), opened ? &uplinkBlob_ : nullptr,
                                            adminMode_ ? cajui::ConsoleMode::Admin
                                                       : cajui::ConsoleMode::Operation,
                                            &radioBlob_)); // Reports STORAGE if unopened.
    console_.reset(new Console(*commands_));
    if (adminMode_) {
        Serial.printf("CJAPP ADMIN reason=%s\n", cajui::reasonName(decision.reason));
        return;
    }
    if (!ready) return fault("STARTUP");
    int8_t power = cajui::DefaultPowerDbm;
    if (cajui::loadPower(radioBlob_, power) == cajui::ReadResult::Error)
        Serial.println("CJAPP POWER config_error"); // The bench default applies.
    if (!radio_.begin(power)) return fault("RADIO_INIT");
    controller_.setPairing(&pairing_);
    if (!controller_.start()) return fault("RECEIVE_START");
    listening_.store(true);
    Serial.printf("CJAPP RECEIVER queued=%u power=%d\n", unsigned(store_.queued()), int(power));
    startForwarding();
    portal_.setPairing(&pairing_);
    if (!portal_.start(listening_)) Serial.println("CJAPP SETUP unavailable");
    running_ = true;
    startedAt_ = millis();
    enableLoopWDT(); // The loop never blocks: a hang restarts the receiver.
}
void ReceiverApp::loop() {
    {
        Locked held(lock_); // INFO reads the store the setup page may be changing.
        if (console_ && console_->poll()) restartPending_ = true;
    }
    if (adminMode_) {
        if (restartPending_) restartFor(*commands_);
        delay(1);
        return;
    }
    if (!running_) {
        if (restartPending_) restartFor(*commands_);
        if (int32_t(millis() - retryAt_) >= 0) restartInto(cajui::BootRequest::None);
        delay(1);
        return;
    }
    const char* failure = nullptr;
    bool listening = false;
    {
        Locked held(lock_);
        const auto before = controller_.state();
        controller_.poll();
        if (before == cajui::ReceiverState::Listening &&
            controller_.state() == cajui::ReceiverState::Acknowledging &&
            controller_.acknowledgedData()) { // Pairing replies log their own lines.
            const cajui::Link& link = controller_.lastLink();
            if (link.known)
                Serial.printf("CJAPP ACCEPT result=%u queued=%u rssi=%d snr=%s%d.%d\n",
                              unsigned(controller_.lastResult()), unsigned(store_.queued()),
                              int(link.rssiDbm), link.snrTenthsDb < 0 ? "-" : "",
                              std::abs(link.snrTenthsDb) / 10, std::abs(link.snrTenthsDb) % 10);
            else
                Serial.printf("CJAPP ACCEPT result=%u queued=%u rssi=unknown snr=unknown\n",
                              unsigned(controller_.lastResult()), unsigned(store_.queued()));
        }
        listening = controller_.state() == cajui::ReceiverState::Listening;
        listening_.store(listening);
        if (controller_.state() == cajui::ReceiverState::Failed) {
            failure = "RECEIVER";
        } else if (forwarder_) {
            // Only while listening: forwarding writes flash and must not delay an ACK.
            forwarder_->poll(listening);
            reportForwarding();
            if (forwarder_->state() == cajui::ForwardState::Failed) failure = "FORWARDER";
        }
        pairing_.poll();
    }
    if (failure) return fault(failure);
    if (restartPending_ && listening) restartFor(*commands_);
    if (!healthy_ && uint32_t(millis() - startedAt_) >= cajui::HealthyRunMs) {
        healthy_ = true;
        clearFaults();
    }
    delay(1);
}
ReceiverApp* app = nullptr;
} // namespace

void setup() {
    static ReceiverApp instance;
    app = &instance;
    app->setup();
}
void loop() {
    app->loop();
}
#endif
