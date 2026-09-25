// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "display.h"
#include "sx1262_radio.h"
#include <SSD1306Wire.h>
#include <qrcode.h>

namespace board {
namespace {
constexpr uint8_t OledAddress = 0x3c;
constexpr int Height = 64, Margin = 1, TextX = 66, LineHeight = 12;
constexpr int QrMaxVersion = 3; // 29 modules plus margin, two pixels each, fits 64 rows.
SSD1306Wire oled(OledAddress, OledSda, OledScl);
bool powered = false, drawn = false;
// esp_qrcode_generate only reports the symbol through this callback.
void drawQr(esp_qrcode_handle_t code) {
    const int size = esp_qrcode_get_size(code);
    const int scale = Height / (size + 2 * Margin);
    if (scale < 1) return;
    const int side = (size + 2 * Margin) * scale;
    // Dark modules stay unlit on a lit square: phone cameras expect dark-on-light codes.
    oled.setColor(WHITE);
    oled.fillRect(0, (Height - side) / 2, side, side);
    oled.setColor(BLACK);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (esp_qrcode_get_module(code, x, y))
                oled.fillRect((x + Margin) * scale, (Height - side) / 2 + (y + Margin) * scale,
                              scale, scale);
    oled.setColor(WHITE);
    drawn = true;
}
} // namespace
bool showSetup(const char* ssid, const char* qrText, const char* address) {
    digitalWrite(Vext, LOW);
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);
    delay(50);
    pinMode(OledReset, OUTPUT);
    digitalWrite(OledReset, LOW);
    delay(20);
    digitalWrite(OledReset, HIGH);
    delay(20);
    if (!oled.init()) return false;
    powered = true;
    oled.clear();
    drawn = false;
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func = drawQr;
    config.max_qrcode_version = QrMaxVersion;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    const bool coded = esp_qrcode_generate(&config, qrText) == ESP_OK && drawn;
    oled.setFont(ArialMT_Plain_10);
    oled.setTextAlignment(TEXT_ALIGN_LEFT);
    oled.drawString(TextX, 0, "Setup Wi-Fi");
    oled.drawString(TextX, LineHeight, ssid);
    oled.drawString(TextX, 3 * LineHeight, "Open page:");
    oled.drawString(TextX, 4 * LineHeight, address);
    oled.display();
    return coded;
}
void displayOff() {
    if (powered) {
        oled.displayOff();
        powered = false;
    }
    digitalWrite(Vext, HIGH);
}
} // namespace board
#endif
