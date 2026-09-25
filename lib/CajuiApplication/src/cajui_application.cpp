#include "cajui_application.h"
#include <cmath>

namespace cajui {
namespace {
constexpr uint32_t AckTransmitTimeoutMs = 3000;
constexpr uint8_t PairingFirstType = 3, PairingLastType = 6; // docs/radio-pairing.md
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
    const auto status = radio_.receive(frame);
    if (status == ReceiveStatus::Error) {
        fail();
        return;
    }
    if (status == ReceiveStatus::Empty) return;
    const uint8_t type = untrustedType(frame);
    if (type >= PairingFirstType && type <= PairingLastType) {
        if (!pairing_ || !pairing_->handle(frame, radio_.lastRssi(), ack_)) return;
        startedAt_ = clock_.nowMs();
        if (!radio_.startTransmit(ack_)) {
            fail();
            return;
        }
        state_ = ReceiverState::Acknowledging;
        return;
    }
    Binding binding{};
    const uint64_t node = untrustedDataNode(frame);
    if (!node || !store_.binding(node, binding)) {
        result_ = Result::Unauthorized;
        return;
    }
    result_ = cajui::receive(binding, frame, store_, ack_);
    if (result_ == Result::StorageError) {
        fail();
        return;
    }
    if (result_ != Result::Ok && result_ != Result::Duplicate) return;
    startedAt_ = clock_.nowMs();
    if (!radio_.startTransmit(ack_)) {
        fail();
        return;
    }
    state_ = ReceiverState::Acknowledging;
}
} // namespace cajui
