// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
// Receiver image: listens for enrolled transmitters, queues their samples durably, forwards
// them to MQTT and serves the setup page. See docs/radio-applications.md.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include "cajui_application.h"
#include "cajui_command.h"
#include "cajui_device.h"
#include "cajui_manage.h"
#include "cajui_nvs.h"
#include "cajui_pairing.h"
#include "cajui_provisioning.h"
#include "cajui_uplink.h"
#include "board/admin_console.h"
#include "board/common.h"
#include "board/mqtt_uplink.h"
#include "board/ota.h"
#include "board/setup_portal.h"
#include "board/sx1262_radio.h"

namespace {
using namespace board;
constexpr uint32_t ConfirmAfterMs = 60000, UpdateRestartDelayMs = 1500, BindingCheckMs = 1000;
constexpr char Model[] = "heltec-wifi-lora-32-v3";
uint32_t uptimeSeconds() {
    return uint32_t(esp_timer_get_time() / 1000000);
}

class ReceiverApp final : public ReceiverControl, public cajui::StateSource {
public:
    ReceiverApp() : store_(records_, cajui::Role::Receiver, deviceId()) {}
    void setup();
    void loop();
    bool applyUplink(const cajui::UplinkConfig&) override;
    void restartForUpdate() override { updateRestart_.store(true); }
    void receiverStatus(cajui::ReceiverStatus&) override;
    size_t nodes(uint64_t* output, size_t capacity) override;
    bool nodeStatus(uint64_t node, cajui::NodeStatus&) override;

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
    std::atomic<bool> listening_{false}, updateRestart_{false};
    cajui::ReceiverController controller_{radio_, clock_, store_};
    cajui::PairingHost pairing_{store_, entropy_, clock_};
    SetupPortal portal_{store_, uplink_, uplinkBlob_, *this, lock_, entropy_};
    std::unique_ptr<cajui::Forwarder> forwarder_;
    std::unique_ptr<cajui::StateReporter> reporter_;
    cajui::CommandRunner commandRunner_{pairing_, store_, clock_};
    // Command results wait in a small outbox: one command can produce up to three (a
    // superseded or closed accept plus its own), and each publication can wait for the
    // client's lock, so the loop publishes at most one per pass.
    class Results final : public cajui::ResultSink {
    public:
        explicit Results(MqttUplink& uplink) : uplink_(uplink) {}
        void result(uint64_t device, const char* id, cajui::CommandStatus status,
                    cajui::CommandReason reason) override {
            Pending& slot = outbox_[(head_ + count_) % Capacity];
            size_t size = 0;
            if (count_ == Capacity || !cajui::formatResult(id, status, reason, slot.payload,
                                                           sizeof(slot.payload), size)) {
                Serial.printf("CJAPP COMMAND result_dropped id=%s\n", id);
                return;
            }
            slot.device = device;
            slot.generation = generation;
            ++count_;
            Serial.printf("CJAPP COMMAND id=%s status=%u reason=%u\n", id, unsigned(status),
                          unsigned(reason));
        }
        bool empty() const { return count_ == 0; }
        // Publishes the oldest result; a refused one waits for a later pass.
        void flush() {
            const Pending& next = outbox_[head_];
            if (!uplink_.publishResult(next.device, next.payload, next.generation) &&
                next.generation == uplink_.generation() && uplink_.ready())
                return;
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
        uint32_t generation = 0; // Of the command being run.

    private:
        static constexpr size_t Capacity = 4;
        struct Pending {
            uint64_t device = 0;
            uint32_t generation = 0;
            char payload[cajui::ResultCapacity]{};
        } outbox_[Capacity]{};
        size_t head_ = 0, count_ = 0;
        MqttUplink& uplink_;
    } results_{uplink_};
    void runCommands();
    // The last frame accepted from each node since this start, for its management state.
    struct LastFrame {
        uint64_t node = 0, counter = 0;
        cajui::Link link{};
        uint32_t uptimeS = 0;
    } frames_[cajui::BindingCapacity]{};
    cajui::EnrollmentInfo enrollments_[cajui::BindingCapacity]{};
    size_t enrollmentCount_ = 0;
    uint32_t bindingsCheckedAt_ = 0;
    int8_t power_ = cajui::DefaultPowerDbm;
    // Reading the update state maps flash: read at start and after a confirmation only.
    const char* slot_ = nullptr;
    const char* firmwareState_ = nullptr;
    uint32_t offlineAt_ = 0;
    bool offlineAnnounced_ = false;
    std::unique_ptr<cajui::Provisioning> commands_;
    std::unique_ptr<Console> console_;
    bool adminMode_ = false, running_ = false, restartPending_ = false, healthy_ = false,
         confirmed_ = false, setupRunning_ = false;
    uint32_t confirmAttemptAt_ = 0;
    uint32_t startedAt_ = 0, retryAt_ = 0, reportedForwards_ = 0, reportedRetries_ = 0;
    bool reportedOnline_ = false;
    void startForwarding();
    void reportForwarding();
    void recordFrame();
    void checkBindings();
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
        reporter_.reset(new cajui::StateReporter(uplink_, *this, clock_, settings.username));
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
        if (reporter_) reporter_->pause();
    }
    const bool started = uplink_.startMqtt(settings, store_.device());
    Locked held(lock_);
    if (reporter_)
        reporter_->setSource(settings.username);
    else if (started)
        reporter_.reset(new cajui::StateReporter(uplink_, *this, clock_, settings.username));
    if (forwarder_) return forwarder_->setSource(settings.username) && started;
    if (!started) return false;
    forwarder_.reset(new cajui::Forwarder(uplink_, clock_, store_, settings.username));
    return forwarder_->state() != cajui::ForwardState::Failed;
}
void ReceiverApp::receiverStatus(cajui::ReceiverStatus& status) {
    status.device = store_.device();
    status.model = Model;
    status.firmwareVersion = FirmwareVersion;
    status.slot = slot_;
    status.firmwareState = firmwareState_;
    status.profile = store_.profile();
    status.powerDbm = power_;
    status.uptimeS = uptimeSeconds();
    status.resetReason = resetReasonName();
    status.wifiKnown = MqttUplink::wifiConnected();
    status.wifiRssiDbm = status.wifiKnown ? int16_t(WiFi.RSSI()) : int16_t(0);
    status.queued = store_.queued();
    status.queueCapacity = cajui::QueueCapacity;
    status.published = forwarder_ ? forwarder_->forwarded() : 0;
    status.retries = forwarder_ ? forwarder_->retries() : 0;
    status.pairingOpen = pairing_.state() != cajui::HostState::Closed;
    status.pairingRemainingS = pairing_.remainingMs() / 1000;
    status.requests = pairing_.candidates();
    status.requestCount = pairing_.candidateCount();
    status.commands = true;
}
size_t ReceiverApp::nodes(uint64_t* output, size_t capacity) {
    cajui::EnrollmentInfo list[cajui::BindingCapacity]{};
    const size_t listed = store_.list(list, cajui::BindingCapacity);
    size_t count = 0;
    for (size_t i = 0; i < listed; ++i) {
        bool seen = false;
        for (size_t j = 0; j < count && !seen; ++j) seen = output[j] == list[i].node;
        if (!seen && count < capacity) output[count++] = list[i].node;
    }
    return count;
}
bool ReceiverApp::nodeStatus(uint64_t node, cajui::NodeStatus& status) {
    cajui::EnrollmentInfo list[cajui::BindingCapacity]{};
    const size_t listed = store_.list(list, cajui::BindingCapacity);
    bool found = false, active = false, prepared = false;
    for (size_t i = 0; i < listed; ++i) {
        if (list[i].node != node) continue;
        found = true;
        active = active || list[i].state == cajui::Enrollment::Active;
        prepared = prepared || list[i].state == cajui::Enrollment::Prepared;
    }
    if (!found) return false;
    status.node = node;
    status.receiver = store_.device();
    status.binding = active     ? cajui::NodeBinding::Active
                     : prepared ? cajui::NodeBinding::Pending
                                : cajui::NodeBinding::Revoked;
    for (const auto& frame : frames_) {
        if (frame.node != node) continue;
        status.frameKnown = true;
        status.counter = frame.counter;
        status.link = frame.link;
        status.receiverUptimeS = frame.uptimeS;
    }
    return true;
}
// Called under the lock while listening: revocation writes flash. One command per pass
// keeps the loop's work bounded; a pending accept is resolved as the pairing progresses.
void ReceiverApp::runCommands() {
    // Earlier results go out first, so new commands never pile up behind a full outbox.
    if (!results_.empty()) return;
    MqttUplink::Incoming incoming{};
    if (uplink_.nextCommand(incoming)) {
        incoming.payload[incoming.size] = 0;
        results_.generation = incoming.generation;
        commandRunner_.execute(incoming.device, incoming.payload, incoming.size,
                               incoming.receivedAt, results_);
    }
    results_.generation = uplink_.generation();
    commandRunner_.poll(results_);
}
// Called under the lock right after an acknowledged DATA frame.
void ReceiverApp::recordFrame() {
    const uint64_t node = controller_.lastNode();
    cajui::EnrollmentInfo list[cajui::BindingCapacity]{};
    const size_t count = store_.list(list, cajui::BindingCapacity);
    uint64_t counter = 0;
    // A re-paired node's old generation is revoked by its first frame under the new one, so
    // the active enrollment with the highest counter is the one that just received.
    for (size_t i = 0; i < count; ++i)
        if (list[i].node == node && list[i].state == cajui::Enrollment::Active &&
            list[i].received > counter)
            counter = list[i].received;
    // Its own slot, else a free one, else one of a node whose slot was retired since.
    uint64_t listed[cajui::BindingCapacity]{};
    const size_t known = nodes(listed, cajui::BindingCapacity);
    LastFrame* slot = nullptr;
    for (auto& frame : frames_)
        if (frame.node == node) slot = &frame;
    for (auto& frame : frames_)
        if (!slot && frame.node == 0) slot = &frame;
    for (auto& frame : frames_)
        if (!slot && std::find(listed, listed + known, frame.node) == listed + known) slot = &frame;
    if (!slot) return; // Unreachable: the node holds one of the BindingCapacity slots.
    slot->node = node;
    slot->counter = counter;
    slot->link = controller_.lastLink();
    slot->uptimeS = uptimeSeconds();
    if (reporter_) reporter_->nodeChanged(node);
}
// Pairing, the setup page and the USB console change bindings; compare once a second.
void ReceiverApp::checkBindings() {
    if (uint32_t(millis() - bindingsCheckedAt_) < BindingCheckMs) return;
    bindingsCheckedAt_ = millis();
    cajui::EnrollmentInfo list[cajui::BindingCapacity]{};
    const size_t count = store_.list(list, cajui::BindingCapacity);
    bool changed = count != enrollmentCount_;
    for (size_t i = 0; i < count && !changed; ++i)
        changed = list[i].node != enrollments_[i].node ||
                  list[i].generation != enrollments_[i].generation ||
                  list[i].state != enrollments_[i].state;
    if (!changed) return;
    if (reporter_) {
        for (size_t i = 0; i < enrollmentCount_; ++i) reporter_->nodeChanged(enrollments_[i].node);
        for (size_t i = 0; i < count; ++i) reporter_->nodeChanged(list[i].node);
    }
    std::copy(list, list + count, enrollments_);
    enrollmentCount_ = count;
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
    reportFirmware();
    slot_ = firmwareSlot();
    firmwareState_ = firmwareState();
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
    if (cajui::loadPower(radioBlob_, power_) == cajui::ReadResult::Error) {
        Serial.println("CJAPP POWER config_error"); // The bench default applies.
        power_ = cajui::DefaultPowerDbm;
    }
    if (!radio_.begin(power_)) return fault("RADIO_INIT");
    controller_.setPairing(&pairing_);
    if (!controller_.start()) return fault("RECEIVE_START");
    listening_.store(true);
    Serial.printf("CJAPP RECEIVER queued=%u power=%d\n", unsigned(store_.queued()), int(power_));
    enrollmentCount_ = store_.list(enrollments_, cajui::BindingCapacity);
    startForwarding();
    portal_.setPairing(&pairing_);
    setupRunning_ = portal_.start(listening_);
    if (!setupRunning_) Serial.println("CJAPP SETUP unavailable");
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
    bool listening = false, published = false;
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
            recordFrame();
        }
        listening = controller_.state() == cajui::ReceiverState::Listening;
        listening_.store(listening);
        if (controller_.state() == cajui::ReceiverState::Failed) {
            failure = "RECEIVER";
        } else if (forwarder_) {
            // Only while listening: forwarding writes flash and must not delay an ACK.
            const auto before = forwarder_->state();
            forwarder_->poll(listening);
            published = before != cajui::ForwardState::Waiting &&
                        forwarder_->state() == cajui::ForwardState::Waiting;
            reportForwarding();
            if (forwarder_->state() == cajui::ForwardState::Failed) failure = "FORWARDER";
        }
        pairing_.poll();
        checkBindings();
        // State lives in RAM and goes to the client's outbox, never flash. Each enqueue can
        // wait for the client's lock up to its network timeout: at most one per loop pass,
        // so a pass that published a sample leaves state for the next one.
        if (forwarder_ && listening && !published) {
            runCommands();
            if (!results_.empty()) {
                results_.flush(); // This pass's one wait for the client lock.
                published = true;
            }
        }
        if (reporter_ && listening && !published) reporter_->poll();
    }
    if (failure) return fault(failure);
    if (restartPending_ && listening) restartFor(*commands_);
    // Give the page a moment to deliver its response, and the client time to send
    // "offline", before restarting into the update. Waiting across passes keeps each
    // pass's blocking within the loop watchdog.
    if (updateRestart_.load() && listening) {
        if (!offlineAnnounced_) {
            offlineAnnounced_ = true;
            offlineAt_ = millis();
            uplink_.announceOffline();
        } else if (uint32_t(millis() - offlineAt_) >= UpdateRestartDelayMs) {
            restartInto(cajui::BootRequest::None);
        }
    }
    // A minute of healthy operation confirms a freshly updated image; until then any
    // restart returns to the previous one.
    // The setup page is the receiver's only update channel without a cable: an image
    // without it is never kept.
    if (!confirmed_ && setupRunning_ && uint32_t(millis() - startedAt_) >= ConfirmAfterMs &&
        uint32_t(millis() - confirmAttemptAt_) >= ConfirmAfterMs) {
        confirmAttemptAt_ = millis();
        confirmed_ = confirmFirmware();
        if (confirmed_) {
            Locked held(lock_); // The reporter reads it under the lock.
            firmwareState_ = firmwareState();
        }
    }
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
