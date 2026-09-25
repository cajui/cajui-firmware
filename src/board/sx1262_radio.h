// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
#include <Arduino.h>
#include <RadioLib.h>
#include "cajui_application.h"

namespace board {
constexpr uint8_t RadioCs = 8, RadioClock = 9, RadioMiso = 11, RadioMosi = 10;
constexpr uint8_t RadioReset = 12, RadioBusy = 13, RadioDio = 14;
constexpr uint8_t Vext = 36, SensorData = 47, Led = 35;
constexpr uint16_t RadioProfile = 1;

// One statically allocated adapter for the physical radio; never destroyed while active.
// Task-side SPI is serialized. The ISR only timestamps and wakes the service task.
class Sx1262Radio final : public cajui::ReceiverRadio {
public:
    Sx1262Radio();
    Sx1262Radio(const Sx1262Radio&) = delete;
    Sx1262Radio& operator=(const Sx1262Radio&) = delete;
    // Transmit power in dBm, within cajui::MinPowerDbm..MaxPowerDbm.
    bool begin(int8_t powerDbm);
    bool listen() override;
    bool startChannelCheck() override;
    cajui::ChannelStatus channelStatus() override;
    bool startTransmit(const cajui::Frame&) override;
    cajui::TransmitStatus transmitStatus(uint32_t&) override;
    cajui::ReceiveStatus receive(cajui::Frame&) override;
    bool sleep() override;
    cajui::Link lastLink() const override;

private:
    enum class Mode { Idle, Cad, Tx, Rx, Failed };
    Module module_;
    SX1262 radio_;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    Mode mode_ = Mode::Idle;
    cajui::ChannelStatus cad_ = cajui::ChannelStatus::Pending;
    cajui::TransmitStatus tx_ = cajui::TransmitStatus::Pending;
    cajui::Frame received_{};
    cajui::Link link_{};
    uint32_t completedAt_ = 0;
    bool initialized_ = false;
    static Sx1262Radio* instance_;
    static portMUX_TYPE irqLock_;
    static volatile uint32_t irqAt_;
    static void IRAM_ATTR interrupt();
    static void service(void*);
    void handleInterrupt();
    void fail();
    bool standby();
};
} // namespace board
#endif
