// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace board {
// Heltec WiFi LoRa 32 V3 SSD1306 OLED, powered from Vext. The Wireless Stick Lite has no
// display: there the setup network name must come from a label instead.
constexpr uint8_t OledSda = 17, OledScl = 18, OledReset = 21;
// Shows the setup network as a QR code plus its name and address. False without a display.
bool showSetup(const char* ssid, const char* qrText, const char* address);
void displayOff();
} // namespace board
