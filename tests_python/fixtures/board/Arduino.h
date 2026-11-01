// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
constexpr uint8_t LOW = 0, HIGH = 1, INPUT = 0, OUTPUT = 1, ANALOG = 2;
constexpr uint8_t ADC_2_5db = 1;
void digitalWrite(uint8_t pin, uint8_t level);
void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);
void delay(uint32_t ms);
void analogSetPinAttenuation(uint8_t pin, uint8_t attenuation);
uint32_t analogReadMilliVolts(uint8_t pin);
