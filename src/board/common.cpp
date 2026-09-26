// SPDX-License-Identifier: Apache-2.0
#ifdef CAJUI_RUNTIME_ROLE
#include "common.h"
#include "sx1262_radio.h"
#include <bootloader_random.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_system.h>

namespace board {
namespace {
constexpr uint32_t SerialBaud = 115200;
// The magic guards against the random contents RTC memory has after power-on.
constexpr uint32_t FaultMagic = 0x46415554; // "FAUT"
RTC_NOINIT_ATTR uint32_t faultMagic;
RTC_NOINIT_ATTR uint32_t faults;
}
bool BoardJitter::between(uint32_t minimum, uint32_t maximum, uint32_t& value) {
    if (minimum > maximum) return false;
    const uint64_t width = uint64_t(maximum) - minimum + 1;
    value = uint32_t(minimum + uint64_t(esp_random()) % width);
    return true;
}
bool BoardEntropy::fill(uint8_t* output, size_t size) {
    if (!wifi_) bootloader_random_enable();
    esp_fill_random(output, size);
    if (!wifi_) bootloader_random_disable();
    return true;
}
bool AppLock::begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    return mutex_ != nullptr;
}
void AppLock::take() {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
}
void AppLock::give() {
    if (mutex_) xSemaphoreGive(mutex_);
}
uint64_t deviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return 0;
    uint64_t id = 0;
    for (auto byte : mac) id = (id << 8) | byte;
    return id;
}
void output(uint8_t pin, uint8_t level) {
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}
const char* resetReasonName() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}
void startBoard() {
    gpio_deep_sleep_hold_dis();
    for (auto pin : {Vext, Led, RadioCs, RadioReset}) gpio_hold_dis(gpio_num_t(pin));
    output(RadioReset, LOW);
    output(RadioCs, HIGH);
    output(Vext, HIGH);
    output(Led, LOW);
    Serial.begin(SerialBaud);
    // USB serial carries the CJ1 console. ESP-IDF components log from other tasks and
    // interleaved an MQTT error into the middle of a HELLO reply on hardware; CJAPP lines
    // remain the diagnostic output.
    esp_log_level_set("*", ESP_LOG_NONE);
    const esp_reset_reason_t reason = esp_reset_reason();
    if (faultMagic != FaultMagic || reason == ESP_RST_POWERON) {
        faultMagic = FaultMagic;
        faults = 0;
    }
    // A watchdog or panic restart already happened immediately; it still counts, so a
    // repeating hang lengthens the delay after the next latched fault.
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_PANIC)
        recordFault();
}
uint32_t faultCount() {
    return faults;
}
uint32_t recordFault() {
    if (faults != UINT32_MAX) ++faults;
    return faults;
}
void clearFaults() {
    faults = 0;
}
} // namespace board
#endif
