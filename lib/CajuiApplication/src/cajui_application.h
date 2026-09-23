#pragma once
#include "cajui_runtime.h"
#include "cajui_storage.h"

namespace cajui {
class ReceiverRadio : public Radio {
public:
    virtual bool listen() = 0;
};
enum class ReceiverState { Stopped, Listening, Acknowledging, Failed };
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
    ReceiverState state() const { return state_; }
    Result lastResult() const { return result_; }

private:
    ReceiverRadio& radio_;
    Clock& clock_;
    PersistentStore& store_;
    Frame ack_{};
    ReceiverState state_ = ReceiverState::Stopped;
    Result result_ = Result::NotFound;
    uint32_t startedAt_ = 0;
    void fail();
};
// Local metric registry v1: sensor 1, temperature 1/Celsius 1, humidity 2/percent 2.
// Non-finite/out-of-range DHT22 values become Error with zero payload, not zero readings.
Data climateSample(float temperatureC, float humidityPercent, uint32_t nextSeconds);
} // namespace cajui
