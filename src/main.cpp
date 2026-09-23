#include <Arduino.h>
#include <cstring>
#include <esp_mac.h>
#include <esp_system.h>
#include "cajui_nvs.h"
#include "cajui_provisioning.h"
#ifndef CAJUI_ADMIN_ROLE
#error "Select an admin_tx or admin_rx build environment."
#endif
namespace {
// SX1262 wiring on the Heltec WiFi LoRa 32 V3. No radio driver is linked in this image.
constexpr uint8_t RadioReset = 12, RadioChipSelect = 8;
cajui::NvsBlob blob;
uint64_t deviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return 0;
    uint64_t result = 0;
    for (auto byte : mac) result = (result << 8) | byte;
    return result;
}
cajui::Provisioning* admin = nullptr;
char line[cajui::CommandCapacity]{};
char reply[cajui::ReplyCapacity]{};
size_t used = 0;
bool overflow = false;
}
void setup() {
    // Keep SX1262 in reset and deselected. Enrollment and restart checks do not transmit RF.
    // Each level is written before and after enabling the output so the pin never glitches.
    digitalWrite(RadioReset, LOW);
    pinMode(RadioReset, OUTPUT);
    digitalWrite(RadioReset, LOW);
    digitalWrite(RadioChipSelect, HIGH);
    pinMode(RadioChipSelect, OUTPUT);
    digitalWrite(RadioChipSelect, HIGH);
    Serial.begin(115200);
    // Static, not on the 8 KB loop-task stack: the store holds over 60 KB of state.
    static cajui::PersistentStore persistent(blob, cajui::Role(CAJUI_ADMIN_ROLE), deviceId());
    static cajui::Provisioning provisioning(persistent, esp_random());
    admin = &provisioning;
    if (blob.begin()) persistent.mount();
}
void loop() {
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c == '\n') {
            if (overflow)
                Serial.println("CJ1 ERR INVALID");
            else {
                admin->execute(line, used, reply, sizeof(reply));
                Serial.println(reply);
            }
            // PREPARE carries a key, and this buffer outlives the command.
            std::memset(line, 0, sizeof(line));
            used = 0;
            overflow = false;
            if (admin->restartRequested()) {
                Serial.flush();
                delay(50);
                ESP.restart();
            }
        } else if (!overflow) {
            if (used + 1 >= sizeof(line))
                overflow = true;
            else
                line[used++] = c;
        }
    }
    delay(1);
}
