#ifdef CAJUI_RUNTIME_ROLE
#include "admin_console.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <cstring>

namespace board {
namespace {
// Survives a software restart, not a power cycle; the magic value guards against the
// random contents RTC memory has after power-on.
constexpr uint32_t AdminMagic = 0x41444d4e, PairMagic = 0x50414952; // "ADMN", "PAIR"
RTC_NOINIT_ATTR uint32_t bootRequest;
constexpr uint32_t FlushDelayMs = 50;
}
bool Console::poll() {
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c != '\n') {
            if (used_ + 1 >= sizeof(line_))
                overflow_ = true;
            else if (!overflow_)
                line_[used_++] = c;
            continue;
        }
        if (overflow_)
            Serial.println("CJ1 ERR INVALID");
        else {
            provisioning_.execute(line_, used_, reply_, sizeof(reply_));
            Serial.println(reply_);
        }
        // PREPARE and UPLINKSET carry secrets, and this buffer outlives the command.
        std::memset(line_, 0, sizeof(line_));
        used_ = 0;
        overflow_ = false;
        if (provisioning_.restartRequested()) return true;
    }
    return false;
}
BootRequest takeBootRequest() {
    const BootRequest request = bootRequest == AdminMagic  ? BootRequest::Admin
                                : bootRequest == PairMagic ? BootRequest::Pair
                                                           : BootRequest::None;
    bootRequest = 0;
    return request;
}
void restartFor(const cajui::Provisioning& provisioning) {
    restartInto(provisioning.pairRequested()    ? BootRequest::Pair
                : provisioning.adminRequested() ? BootRequest::Admin
                                                : BootRequest::None);
}
void restartInto(BootRequest request) {
    bootRequest = request == BootRequest::Admin  ? AdminMagic
                  : request == BootRequest::Pair ? PairMagic
                                                 : 0;
    Serial.flush();
    delay(FlushDelayMs);
    ESP.restart();
    while (true) {
    }
}
} // namespace board
#endif
