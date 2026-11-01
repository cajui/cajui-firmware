// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <driver/gpio.h>
// Values as in the ESP32-S3 SDK; its ALL_LOW is a deprecated alias of ANY_LOW.
enum esp_sleep_ext1_wakeup_mode_t { ESP_EXT1_WAKEUP_ANY_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1 };
esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t pin, int level);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, esp_sleep_ext1_wakeup_mode_t mode);
