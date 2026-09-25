// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_runtime.h"
#include "cajui_storage.h"

namespace cajui {
class ReceiverRadio : public Radio {
public:
    virtual bool listen() = 0;
    // Like receive(), also returning the signal quality of that same frame, taken together
    // so it cannot belong to a frame that arrived later. Unknown when not measured.
    virtual ReceiveStatus receiveMeasured(Frame& frame, Link& link) {
        link = Link{};
        return receive(frame);
    }
};
// Receives radio pairing frames (types 3-6). A true result starts transmitting reply.
class PairingPort {
public:
    virtual ~PairingPort() = default;
    virtual bool handle(const Frame& frame, int16_t rssi, Frame& reply) = 0;
};
enum class ReceiverState { Stopped, Listening, Acknowledging, Failed };
// Bounds how often one node's repeated DATA frame is acknowledged again. A genuine node
// repeats a sample at most twice per cycle when its ACK is lost; a replayed frame must
// not make the receiver transmit on demand.
class DuplicateAckLimiter {
public:
    static constexpr uint32_t WindowMs = 60000;
    static constexpr uint8_t PerWindow = 3;
    bool allow(uint64_t node, uint32_t now);

private:
    struct Slot {
        uint64_t node = 0;
        uint32_t since = 0;
        uint8_t count = 0;
    };
    Slot slots_[BindingCapacity]{};
};
// Serialized application owner. No provisioning mutations while running.
// Frames remain members until the adapter is quiescent, including on failure.
class ReceiverController final {
public:
    ReceiverController(ReceiverRadio& radio, Clock& clock, PersistentStore& store)
        : radio_(radio), clock_(clock), store_(store) {}
    ReceiverController(const ReceiverController&) = delete;
    ReceiverController& operator=(const ReceiverController&) = delete;
    bool start();
    void poll();
    // Optional; pairing frames are ignored without it.
    void setPairing(PairingPort* pairing) { pairing_ = pairing; }
    ReceiverState state() const { return state_; }
    Result lastResult() const { return result_; }
    // Signal quality of the last DATA frame the controller processed.
    const Link& lastLink() const { return link_; }
    // True when the current or last ACK answered DATA, false for a pairing reply.
    bool acknowledgedData() const { return data_; }
    // Power command sent in v2 ACKs; KeepPower until a policy sets one (docs/protocol-v1.md).
    void setPowerCommand(int8_t powerDbm) { power_ = powerDbm; }

private:
    ReceiverRadio& radio_;
    Clock& clock_;
    PersistentStore& store_;
    PairingPort* pairing_ = nullptr;
    Frame ack_{};
    ReceiverState state_ = ReceiverState::Stopped;
    Result result_ = Result::NotFound;
    Link link_{};
    int8_t power_ = KeepPower;
    bool data_ = false;
    uint32_t startedAt_ = 0;
    DuplicateAckLimiter duplicates_{};
    void fail();
};
// Local metric registry v1: sensor 1, temperature 1/Celsius 1, humidity 2/percent 2.
// Non-finite/out-of-range DHT22 values become Error with zero payload, not zero readings.
Data climateSample(float temperatureC, float humidityPercent, uint32_t nextSeconds);
} // namespace cajui
