// SPDX-License-Identifier: Apache-2.0
#include "cajui_application.h"
#include <cmath>

namespace cajui {
namespace {
constexpr uint32_t AckTransmitTimeoutMs = 3000;
constexpr float MinTemperature = -40, MaxTemperature = 80, MaxHumidity = 100;
constexpr float MilliScale = 1000;
Reading measurement(uint16_t metric, float value, float minimum, float maximum) {
    Reading reading{};
    reading.sensor = 1;
    reading.metric = metric;
    reading.unit = uint8_t(metric);
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        reading.status = Status::Error;
    } else {
        reading.milliValue = int32_t(std::lround(value * MilliScale));
    }
    return reading;
}
}
Data climateSample(float temperature, float humidity, uint32_t nextSeconds) {
    Data data{};
    data.nextSeconds = nextSeconds;
    data.count = 2;
    data.readings[0] = measurement(1, temperature, MinTemperature, MaxTemperature);
    data.readings[1] = measurement(2, humidity, 0, MaxHumidity);
    return data;
}
bool DuplicateAckLimiter::allow(uint64_t node, uint32_t now) {
    Slot* slot = nullptr;
    for (auto& candidate : slots_)
        if (candidate.node == node) slot = &candidate;
    if (!slot) { // Reuse a free or expired slot; with none, the node is not acknowledged.
        for (auto& candidate : slots_)
            if (!candidate.node || uint32_t(now - candidate.since) >= WindowMs) slot = &candidate;
        if (!slot) return false;
        *slot = Slot{};
        slot->node = node;
        slot->since = now;
    }
    if (uint32_t(now - slot->since) >= WindowMs) {
        slot->since = now;
        slot->count = 0;
    }
    if (slot->count >= PerWindow) return false;
    ++slot->count;
    return true;
}
bool ReceiverController::start() {
    if (state_ != ReceiverState::Stopped) return false;
    // A receiver without any binding may start: radio pairing creates the first one.
    if (!store_.healthy() || store_.role() != Role::Receiver || !radio_.listen()) {
        fail();
        return false;
    }
    state_ = ReceiverState::Listening;
    return true;
}
void ReceiverController::fail() {
    radio_.sleep();
    state_ = ReceiverState::Failed;
}
void ReceiverController::poll() {
    if (state_ == ReceiverState::Acknowledging) {
        uint32_t completed = 0;
        const auto status = radio_.transmitStatus(completed);
        // Completion wins over the watchdog: a poll that arrives late (the loop was busy)
        // after a successful transmission is not a radio fault.
        if (status == TransmitStatus::Complete) {
            // Adapter already rearmed RX at TX-done; do not clear a queued next packet.
            state_ = ReceiverState::Listening;
        } else if (status == TransmitStatus::Error ||
                   uint32_t(clock_.nowMs() - startedAt_) >= AckTransmitTimeoutMs) {
            fail();
        }
        return;
    }
    if (state_ != ReceiverState::Listening) return;
    Frame frame{};
    Link link{};
    const auto status = radio_.receiveMeasured(frame, link);
    if (status == ReceiveStatus::Error) {
        fail();
        return;
    }
    if (status == ReceiveStatus::Empty) return;
    const uint8_t type = untrustedType(frame);
    if (type >= FirstPairingType && type <= LastPairingType) {
        const int16_t rssi = link.known ? link.rssiDbm : int16_t(0);
        if (!pairing_ || !pairing_->handle(frame, rssi, ack_)) return;
        data_ = false;
        startedAt_ = clock_.nowMs();
        if (!radio_.startTransmit(ack_)) {
            fail();
            return;
        }
        state_ = ReceiverState::Acknowledging;
        return;
    }
    // A re-paired node has two active bindings until it uses the new one; the frame
    // authenticates under at most one of them.
    Binding bindings[2]{};
    const uint64_t node = untrustedDataNode(frame);
    const size_t count = node ? store_.bindings(node, bindings, 2) : 0;
    result_ = Result::Unauthorized;
    link_ = link;
    data_ = true;
    for (size_t i = 0; i < count; ++i) {
        result_ = cajui::receive(bindings[i], frame, store_, ack_, link_, power_);
        if (result_ != Result::CryptoError) break;
    }
    if (result_ == Result::StorageError) {
        fail();
        return;
    }
    if (result_ != Result::Ok && result_ != Result::Duplicate) return;
    if (result_ == Result::Duplicate && !duplicates_.allow(node, clock_.nowMs())) return;
    node_ = node;
    startedAt_ = clock_.nowMs();
    if (!radio_.startTransmit(ack_)) {
        fail();
        return;
    }
    state_ = ReceiverState::Acknowledging;
}
} // namespace cajui
