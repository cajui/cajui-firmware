// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
#include <Arduino.h>
#include "cajui_runtime.h"
#include "cajui_pairing.h"

// Board services shared by the transmitter and receiver applications.
namespace board {
class BoardClock final : public cajui::Clock {
public:
    uint32_t nowMs() const override { return millis(); }
};
// Scheduling jitter only; never key material.
class BoardJitter final : public cajui::Jitter {
public:
    bool between(uint32_t minimum, uint32_t maximum, uint32_t& value) override;
};
// Keys, nonces, generations and session tokens. The hardware RNG is a true RNG only while
// Wi-Fi or Bluetooth runs, or with the SAR ADC entropy source enabled: a device that never
// starts Wi-Fi (the transmitter) passes wifiRunning=false.
class BoardEntropy final : public cajui::Entropy {
public:
    explicit BoardEntropy(bool wifiRunning) : wifi_(wifiRunning) {}
    bool fill(uint8_t* output, size_t size) override;

private:
    bool wifi_;
};
// A FreeRTOS mutex held by the receiver loop around radio, storage, pairing and forwarding,
// and by the setup page task around the same objects.
class AppLock {
public:
    AppLock() = default;
    AppLock(const AppLock&) = delete;
    AppLock& operator=(const AppLock&) = delete;
    // Creates the mutex; call once from setup() before any other task can take it.
    bool begin();
    void take();
    void give();

private:
    SemaphoreHandle_t mutex_ = nullptr;
};
class Locked {
public:
    explicit Locked(AppLock& lock) : lock_(lock) { lock_.take(); }
    ~Locked() { lock_.give(); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

private:
    AppLock& lock_;
};
uint64_t deviceId();
// Drives a pin before and after enabling the output, so it never glitches.
void output(uint8_t pin, uint8_t level);
// GPIO holds released, radio in reset, rails off, serial console up, component logs off.
void startBoard();
// Consecutive faults, kept in RTC memory across software restarts and deep sleep.
uint32_t faultCount();
uint32_t recordFault();
void clearFaults();
} // namespace board
#endif
