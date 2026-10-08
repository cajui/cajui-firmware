// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef CAJUI_RUNTIME_ROLE
#include <Arduino.h>
#include <RadioLib.h>
#include <atomic>
#include "cajui_application.h"
#include "profile.h"
#include "cajui_service.h"

namespace board {
constexpr uint8_t RadioCs = 8, RadioClock = 9, RadioMiso = 11, RadioMosi = 10;
constexpr uint8_t RadioReset = 12, RadioBusy = 13, RadioDio = 14;
constexpr uint16_t RadioProfile = 1;

// One statically allocated adapter for the physical radio; never destroyed while active.
// Task-side SPI is serialized. The ISR only timestamps and wakes the service task.
class Sx1262Radio final : public cajui::ReceiverRadio, private cajui::WatchedTaskStartup {
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
    cajui::ReceiveStatus receiveMeasured(cajui::Frame&, cajui::Link&) override;
    // Packets the radio reported, valid or not (CRC errors included): proof it still hears.
    uint32_t packets() const { return packets_.load(); }

private:
    enum class Mode { Idle, Cad, Tx, Rx, Failed };
    Module module_;
    SX1262 radio_;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    Mode mode_ = Mode::Idle;
    cajui::ChannelStatus cad_ = cajui::ChannelStatus::Pending;
    cajui::TransmitStatus tx_ = cajui::TransmitStatus::Pending;
    // Received frames wait here until read: a frame from a second node that arrives while the
    // receiver commits and acknowledges the first is kept, not overwritten. Only listen()
    // and sleep() empty it; a full inbox drops the newest frame and its sender retries.
    static constexpr size_t InboxCapacity = 4;
    cajui::Frame inbox_[InboxCapacity]{};
    cajui::Link links_[InboxCapacity]{}; // Signal quality of the frame in the same slot.
    size_t inboxHead_ = 0, inboxCount_ = 0;
    std::atomic<uint32_t> packets_{0};
    uint32_t completedAt_ = 0;
    bool initialized_ = false;
    static Sx1262Radio* instance_;
    static portMUX_TYPE irqLock_;
    static volatile uint32_t irqAt_;
    static void IRAM_ATTR interrupt();
    static void service(void*);
    bool createParked() override;
    bool subscribeWatchdog() override;
    void release() override;
    void discard() override;
    void handleInterrupt();
    void fail();
    bool standby();
    void clearInbox();
    cajui::ReceiveStatus pop(cajui::Frame&, cajui::Link&);
};
} // namespace board
#endif
