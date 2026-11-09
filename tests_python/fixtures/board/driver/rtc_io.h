// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <driver/gpio.h>
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin);
esp_err_t rtc_gpio_deinit(gpio_num_t pin);
