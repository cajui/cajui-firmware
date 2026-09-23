#pragma once
#include "cajui_protocol.h"

namespace cajui {

class Clock {
public:
    virtual ~Clock() = default;
    // Monotonic milliseconds modulo 2^32; the same timebase as Radio timestamps.
    virtual uint32_t nowMs() const = 0;
};

class Jitter {
public:
    virtual ~Jitter() = default;
    // Inclusive bounds. Scheduling randomness only, NEVER a source of keys/nonces.
    virtual bool between(uint32_t minimum, uint32_t maximum, uint32_t& value) = 0;
};

enum class ChannelStatus { Pending, Clear, Busy, Error };
enum class TransmitStatus { Pending, Complete, Error };
enum class ReceiveStatus { Empty, Received, Error };

// Single owner, nonblocking adapter. No methods may call back into the controller.
class Radio {
public:
    virtual ~Radio() = default;
    virtual bool startChannelCheck() = 0;
    virtual ChannelStatus channelStatus() = 0;
    // Frame remains valid until sleep(). Clear stale events on each new operation.
    virtual bool startTransmit(const Frame&) = 0;
    // At actual TX completion, immediately arm continuous RX (before polling).
    // Complete returns the hardware completion time, not the time of this poll.
    virtual TransmitStatus transmitStatus(uint32_t& completedAtMs) = 0;
    // Pop at most one frame. Enforce the buffer capacity even for oversized RF input.
    // CRC failures are dropped by the adapter; software authentication still applies.
    virtual ReceiveStatus receive(Frame&) = 0;
    // Cancel CAD/TX/RX, clear all events and release frame references. Idempotent.
    // False means radio quiescence is unknown; the controller refuses a new cycle.
    virtual bool sleep() = 0;
};

struct SendPolicy {
    uint32_t initialJitterMs = 500;
    uint32_t ackTimeoutMs = 1500;
    uint32_t firstBackoffMinMs = 100;
    uint32_t firstBackoffMaxMs = 500;
    uint32_t secondBackoffMinMs = 200;
    uint32_t secondBackoffMaxMs = 1000;
    uint32_t channelTimeoutMs = 1000;
    uint32_t transmitTimeoutMs = 3000;
    uint32_t cycleTimeoutMs = 10000;
};

enum class SendState {
    Idle,
    Starting,
    Waiting,
    CheckingChannel,
    Transmitting,
    AwaitingAck,
    Finished
};
enum class StartResult { Started, Busy, InvalidPolicy, ProtocolRejected, RadioUnavailable };
enum class Completion {
    None,
    Acknowledged,
    AttemptsExhausted,
    Deadline,
    RadioError,
    RadioTimeout,
    RandomError,
    Cancelled
};

struct SendReport {
    Completion completion = Completion::None;
    uint8_t attempts = 0;
    uint32_t rejectedAcks = 0;
    bool radioSleeping = false;
};

// One sample per cycle. This schedules delivery, not measurement or MCU deep sleep.
// Call poll() frequently from one task. Pause enrollment/key rotation during a cycle.
class SendController final {
public:
    SendController(Radio&, Clock&, Jitter&, const SendPolicy& = SendPolicy{});
    SendController(const SendController&) = delete;
    SendController& operator=(const SendController&) = delete;
    StartResult start(const Binding&, const Data&, CounterStore&);
    void poll();
    void cancel();
    bool active() const;
    SendState state() const { return state_; }
    const SendReport& report() const { return report_; }
    // Result of the last Sender::begin; meaningful when start returns ProtocolRejected.
    Result protocolResult() const { return protocolResult_; }

private:
    Radio& radio_;
    Clock& clock_;
    Jitter& jitter_;
    const SendPolicy policy_;
    Sender sender_{};
    SendState state_ = SendState::Idle;
    SendReport report_{};
    Result protocolResult_ = Result::Ok;
    uint32_t cycleStart_ = 0, phaseStart_ = 0, waitMs_ = 0;
    bool radioAvailable_ = true;

    bool validPolicy() const;
    void wait(uint32_t minimum, uint32_t maximum);
    void backoff();
    void finish(Completion);
};

} // namespace cajui
