#include "cajui_runtime.h"

namespace cajui {

SendController::SendController(Radio& radio, Clock& clock, Jitter& jitter, const SendPolicy& policy)
    : radio_(radio), clock_(clock), jitter_(jitter), policy_(policy) {}

bool SendController::active() const {
    return state_ != SendState::Idle && state_ != SendState::Finished;
}

bool SendController::validPolicy() const {
    // Bound all intervals below the unsigned half-range for wrap-safe subtraction.
    const auto cycle = policy_.cycleTimeoutMs;
    return cycle && cycle <= INT32_MAX && policy_.initialJitterMs < cycle &&
           policy_.ackTimeoutMs && policy_.ackTimeoutMs < cycle &&
           policy_.channelTimeoutMs && policy_.channelTimeoutMs < cycle &&
           policy_.transmitTimeoutMs && policy_.transmitTimeoutMs < cycle &&
           policy_.firstBackoffMinMs && policy_.firstBackoffMinMs <= policy_.firstBackoffMaxMs &&
           policy_.firstBackoffMaxMs < cycle && policy_.secondBackoffMinMs &&
           policy_.secondBackoffMinMs <= policy_.secondBackoffMaxMs &&
           policy_.secondBackoffMaxMs < cycle;
}

StartResult SendController::start(const Binding& binding, const Data& data, CounterStore& store) {
    if (active()) return StartResult::Busy;
    if (!radioAvailable_) return StartResult::RadioUnavailable;
    if (!validPolicy()) return StartResult::InvalidPolicy;
    const auto started = clock_.nowMs();
    protocolResult_ = sender_.begin(binding, data, store);
    if (protocolResult_ != Result::Ok) return StartResult::ProtocolRejected;
    // The cycle includes time spent reserving a durable counter.
    cycleStart_ = started;
    report_ = SendReport{};
    state_ = SendState::Starting;
    return StartResult::Started;
}

void SendController::finish(Completion completion) {
    // Stop the driver BEFORE releasing the frame it may still reference.
    radioAvailable_ = radioAvailable_ && radio_.sleep();
    report_.radioSleeping = radioAvailable_;
    report_.attempts = sender_.attempts();
    report_.completion = completion;
    state_ = SendState::Finished;
    // If stop failed, retain the frame and prohibit reuse until driver recovery and
    // reconstruction of this controller. The application must keep it alive.
    if (radioAvailable_) sender_.abandon();
}

void SendController::wait(uint32_t minimum, uint32_t maximum) {
    if (!radio_.sleep()) {
        // Latch an uncertain stop; never retry a failed shutdown implicitly.
        radioAvailable_ = false;
        finish(Completion::RadioError);
        return;
    }
    uint32_t delay = 0;
    if (!jitter_.between(minimum, maximum, delay) || delay < minimum || delay > maximum) {
        finish(Completion::RandomError);
        return;
    }
    waitMs_ = delay;
    phaseStart_ = clock_.nowMs();
    state_ = SendState::Waiting;
}

void SendController::backoff() {
    if (sender_.attempts() < 2) wait(policy_.firstBackoffMinMs, policy_.firstBackoffMaxMs);
    else wait(policy_.secondBackoffMinMs, policy_.secondBackoffMaxMs);
}

void SendController::poll() {
    if (!active()) return;
    const auto now = clock_.nowMs();
    if (uint32_t(now - cycleStart_) >= policy_.cycleTimeoutMs) {
        finish(Completion::Deadline);
        return;
    }
    switch (state_) {
    case SendState::Starting:
        wait(0, policy_.initialJitterMs);
        break;
    case SendState::Waiting:
        if (uint32_t(now - phaseStart_) < waitMs_) break;
        if (!radio_.startChannelCheck()) { finish(Completion::RadioError); break; }
        phaseStart_ = now;
        state_ = SendState::CheckingChannel;
        break;
    case SendState::CheckingChannel: {
        if (uint32_t(now - phaseStart_) >= policy_.channelTimeoutMs) {
            finish(Completion::RadioTimeout); break;
        }
        const auto status = radio_.channelStatus();
        if (status == ChannelStatus::Pending) break;
        if (status == ChannelStatus::Busy) { backoff(); break; }
        if (status != ChannelStatus::Clear) { finish(Completion::RadioError); break; }
        const auto* frame = sender_.nextAttempt();
        if (!frame || !radio_.startTransmit(*frame)) { finish(Completion::RadioError); break; }
        phaseStart_ = now;
        state_ = SendState::Transmitting;
        break;
    }
    case SendState::Transmitting: {
        if (uint32_t(now - phaseStart_) >= policy_.transmitTimeoutMs) {
            finish(Completion::RadioTimeout); break;
        }
        uint32_t completedAt = 0;
        const auto status = radio_.transmitStatus(completedAt);
        if (status == TransmitStatus::Pending) break;
        if (status != TransmitStatus::Complete ||
            uint32_t(completedAt - phaseStart_) > uint32_t(now - phaseStart_)) {
            finish(Completion::RadioError); break;
        }
        phaseStart_ = completedAt;
        state_ = SendState::AwaitingAck;
        break;
    }
    case SendState::AwaitingAck: {
        // Deadlines precede packet processing: even a stream of junk cannot extend RX.
        if (uint32_t(now - phaseStart_) >= policy_.ackTimeoutMs) {
            if (sender_.attempts() >= Sender::MaxAttempts) finish(Completion::AttemptsExhausted);
            else backoff();
            break;
        }
        Frame frame{};
        const auto status = radio_.receive(frame);
        if (status == ReceiveStatus::Empty) break;
        if (status != ReceiveStatus::Received) { finish(Completion::RadioError); break; }
        if (sender_.acknowledge(frame) == Result::Ok) finish(Completion::Acknowledged);
        else if (report_.rejectedAcks != UINT32_MAX) ++report_.rejectedAcks;
        break;
    }
    case SendState::Idle:
    case SendState::Finished:
        break; // Filtered by active(); listed so -Wswitch flags any new state.
    }
}

void SendController::cancel() {
    if (active()) finish(Completion::Cancelled);
}

} // namespace cajui
