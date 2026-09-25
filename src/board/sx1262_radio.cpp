// SPDX-License-Identifier: Apache-2.0
#ifdef CAJUI_RUNTIME_ROLE
#include "sx1262_radio.h"
#include <SPI.h>

namespace board {
namespace {
// Profile 1 is an experimental bench profile, not regional automatic configuration.
constexpr float FrequencyMHz = 915.2, BandwidthKHz = 125, TcxoVoltage = 1.8;
constexpr uint8_t SpreadingFactor = 7, CodingRate = 5, SyncWord = 0x12;
constexpr uint16_t PreambleSymbols = 8;
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t mutex_;
};
}
Sx1262Radio* Sx1262Radio::instance_ = nullptr;
portMUX_TYPE Sx1262Radio::irqLock_ = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t Sx1262Radio::irqAt_ = 0;
Sx1262Radio::Sx1262Radio() : module_(RadioCs, RadioDio, RadioReset, RadioBusy), radio_(&module_) {}
bool Sx1262Radio::begin(int8_t powerDbm) {
    if (instance_) return false;
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    SPI.begin(RadioClock, RadioMiso, RadioMosi, RadioCs);
    if (radio_.begin(FrequencyMHz, BandwidthKHz, SpreadingFactor, CodingRate, SyncWord, powerDbm,
                     PreambleSymbols, TcxoVoltage) != RADIOLIB_ERR_NONE ||
        radio_.setCRC(true) != RADIOLIB_ERR_NONE)
        return false;
    instance_ = this;
    if (xTaskCreate(service, "cajui-radio", 4096, this, 3, &task_) != pdPASS) {
        instance_ = nullptr;
        return false;
    }
    initialized_ = true;
    radio_.setDio1Action(interrupt);
    return true;
}
void IRAM_ATTR Sx1262Radio::interrupt() {
    portENTER_CRITICAL_ISR(&irqLock_);
    irqAt_ = millis();
    portEXIT_CRITICAL_ISR(&irqLock_);
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(instance_->task_, &wake);
    if (wake) portYIELD_FROM_ISR();
}
void Sx1262Radio::service(void* argument) {
    auto& self = *static_cast<Sx1262Radio*>(argument);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        Lock lock(self.mutex_);
        self.handleInterrupt();
    }
}
void Sx1262Radio::fail() {
    mode_ = Mode::Failed;
    tx_ = cajui::TransmitStatus::Error;
    cad_ = cajui::ChannelStatus::Error;
}
bool Sx1262Radio::standby() {
    if (mode_ == Mode::Failed) return false;
    if (radio_.standby() != RADIOLIB_ERR_NONE) {
        fail();
        return false;
    }
    received_ = cajui::Frame{};
    return true;
}
bool Sx1262Radio::listen() {
    if (!initialized_) return false;
    Lock lock(mutex_);
    if (!standby()) return false;
    if (radio_.startReceive() != RADIOLIB_ERR_NONE) {
        fail();
        return false;
    }
    mode_ = Mode::Rx;
    return true;
}
bool Sx1262Radio::startChannelCheck() {
    if (!initialized_) return false;
    Lock lock(mutex_);
    if (!standby()) return false;
    cad_ = cajui::ChannelStatus::Pending;
    if (radio_.startChannelScan() != RADIOLIB_ERR_NONE) {
        fail();
        return false;
    }
    mode_ = Mode::Cad;
    return true;
}
cajui::ChannelStatus Sx1262Radio::channelStatus() {
    Lock lock(mutex_);
    return cad_;
}
bool Sx1262Radio::startTransmit(const cajui::Frame& frame) {
    if (!initialized_ || !frame.size || frame.size > cajui::MaxFrame) return false;
    Lock lock(mutex_);
    if (!standby()) return false;
    tx_ = cajui::TransmitStatus::Pending;
    if (radio_.startTransmit(frame.bytes.data(), frame.size) != RADIOLIB_ERR_NONE) {
        fail();
        return false;
    }
    mode_ = Mode::Tx;
    return true;
}
cajui::TransmitStatus Sx1262Radio::transmitStatus(uint32_t& time) {
    Lock lock(mutex_);
    time = completedAt_;
    return tx_;
}
cajui::ReceiveStatus Sx1262Radio::receive(cajui::Frame& out) {
    Lock lock(mutex_);
    out = cajui::Frame{};
    if (mode_ == Mode::Failed) return cajui::ReceiveStatus::Error;
    if (!received_.size) return cajui::ReceiveStatus::Empty;
    out = received_;
    received_ = cajui::Frame{};
    return cajui::ReceiveStatus::Received;
}
cajui::Link Sx1262Radio::lastLink() const {
    Lock lock(mutex_);
    return link_;
}
bool Sx1262Radio::sleep() {
    if (!initialized_) return false;
    Lock lock(mutex_);
    // Even after a driver failure, attempt physical quiescence. Keep failure latched.
    const bool stopped =
        radio_.standby() == RADIOLIB_ERR_NONE && radio_.sleep() == RADIOLIB_ERR_NONE;
    if (!stopped) {
        fail();
        return false;
    }
    if (mode_ != Mode::Failed) mode_ = Mode::Idle;
    received_ = cajui::Frame{};
    return true;
}
void Sx1262Radio::handleInterrupt() {
    if (mode_ == Mode::Idle || mode_ == Mode::Failed) return;
    // A notification from a cancelled operation may still be queued. Hardware flags
    // are cleared by each RadioLib start operation and must match the current mode.
    uint32_t timestamp = 0;
    portENTER_CRITICAL(&irqLock_);
    timestamp = irqAt_;
    portEXIT_CRITICAL(&irqLock_);
    const auto irq = radio_.getIrqFlags();
    if (mode_ == Mode::Tx && (irq & RADIOLIB_SX126X_IRQ_TX_DONE)) {
        completedAt_ = timestamp;
        if (radio_.finishTransmit() != RADIOLIB_ERR_NONE ||
            radio_.startReceive() != RADIOLIB_ERR_NONE) {
            fail();
            return;
        }
        // Publish completion only after RX is armed; no dependency on application polling.
        mode_ = Mode::Rx;
        tx_ = cajui::TransmitStatus::Complete;
    } else if (mode_ == Mode::Cad &&
               (irq & (RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED))) {
        const auto result = radio_.getChannelScanResult();
        cad_ = result == RADIOLIB_CHANNEL_FREE    ? cajui::ChannelStatus::Clear
               : result == RADIOLIB_LORA_DETECTED ? cajui::ChannelStatus::Busy
                                                  : cajui::ChannelStatus::Error;
        mode_ = Mode::Idle;
    } else if (mode_ == Mode::Rx && (irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
        // Stop RX before querying length/readData so another packet cannot change length.
        // readData(len=0) would read the full hardware buffer; never call it for empty input.
        if (radio_.standby() != RADIOLIB_ERR_NONE) {
            fail();
            return;
        }
        cajui::Frame frame{};
        frame.size = radio_.getPacketLength();
        if (frame.size && frame.size <= cajui::MaxFrame) {
            const auto result = radio_.readData(frame.bytes.data(), frame.size);
            if (result == RADIOLIB_ERR_NONE) {
                if (!received_.size) { // Bounded inbox; sender retries drops.
                    received_ = frame;
                    // Packet RSSI and SNR of this frame, read before RX is restarted.
                    link_.known = true;
                    link_.rssiDbm = int16_t(lroundf(radio_.getRSSI()));
                    link_.snrTenthsDb = int16_t(lroundf(radio_.getSNR() * 10));
                }
            } else if (result != RADIOLIB_ERR_CRC_MISMATCH) {
                fail();
                return;
            }
        }
        if (radio_.startReceive() != RADIOLIB_ERR_NONE) fail();
    }
}
} // namespace board
#endif
