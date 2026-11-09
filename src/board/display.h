// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace board {
// SSD1306 OLED on the profile's Oled pins, powered from Vext. A board without a display
// (HasDisplay false) gets the setup network name from a label instead.
// Shows the setup network as a QR code plus its name and address. False without a display.
bool showSetup(const char* ssid, const char* qrText, const char* address);
void displayOff();
} // namespace board
