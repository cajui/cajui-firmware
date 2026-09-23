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
cajui::NvsBlob blob;
// Allocate once outside the task stack. No radio driver is linked in this image.
uint64_t deviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return 0;
    uint64_t result = 0;
    for (auto byte : mac) result = (result << 8) | byte;
    return result;
}
cajui::PersistentStore* store = nullptr;
cajui::Provisioning* admin = nullptr;
char line[cajui::CommandCapacity]{}, reply[cajui::ReplyCapacity]{};
size_t used = 0;
bool overflow = false;
}
void setup() {
    // Keep SX1262 in reset. Enrollment and restart checks do not transmit RF.
    digitalWrite(12, LOW);
    pinMode(12, OUTPUT);
    digitalWrite(12, LOW);
    digitalWrite(8, HIGH);
    pinMode(8, OUTPUT);
    digitalWrite(8, HIGH);
    Serial.begin(115200);
    static cajui::PersistentStore persistent(blob, cajui::Role(CAJUI_ADMIN_ROLE), deviceId());
    static cajui::Provisioning provisioning(persistent, esp_random());
    store = &persistent;
    admin = &provisioning;
    if (blob.begin()) store->mount();
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
