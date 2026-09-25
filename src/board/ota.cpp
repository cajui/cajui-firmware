// SPDX-License-Identifier: Apache-2.0
#ifdef CAJUI_RUNTIME_ROLE
#include "ota.h"
#include <Arduino.h>

// Arduino confirms a pending image as soon as it boots unless this returns true; the
// application confirms it itself once it has run healthily (confirmFirmware).
extern "C" bool verifyRollbackLater() {
    return true;
}

namespace board {
bool OtaSink::begin(size_t size) {
    abort();
    // After an install the free slot is the one selected for the next boot: writing into it
    // again, even a file that then fails verification, would leave those bytes bootable.
    if (pending()) return false;
    target_ = esp_ota_get_next_update_partition(nullptr);
    if (!target_ || size > target_->size) return false;
    open_ = esp_ota_begin(target_, size, &handle_) == ESP_OK;
    return open_;
}
bool OtaSink::pending() {
    return esp_ota_get_boot_partition() != esp_ota_get_running_partition();
}
bool OtaSink::write(const uint8_t* data, size_t size) {
    return open_ && esp_ota_write(handle_, data, size) == ESP_OK;
}
bool OtaSink::commit() {
    if (!open_) return false;
    open_ = false;
    // esp_ota_end checks the image format and checksum before it can be selected.
    return esp_ota_end(handle_) == ESP_OK && esp_ota_set_boot_partition(target_) == ESP_OK;
}
void OtaSink::abort() {
    if (open_) esp_ota_abort(handle_);
    open_ = false;
    // Defence in depth: never leave a slot that was being written selected for boot.
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (target_ && target_ == esp_ota_get_boot_partition() && running)
        esp_ota_set_boot_partition(running);
}
size_t OtaSink::capacity() {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    return next ? next->size : 0;
}
void reportFirmware() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const bool known = running && esp_ota_get_state_partition(running, &state) == ESP_OK;
    const esp_partition_t* invalid = esp_ota_get_last_invalid_partition();
    Serial.printf("CJAPP FIRMWARE version=%u slot=%s state=%s rolled_back=%u\n",
                  unsigned(FirmwareVersion), running ? running->label : "unknown",
                  // An image written over USB has no update state: "flashed".
                  !known || state == ESP_OTA_IMG_UNDEFINED ? "flashed"
                  : state == ESP_OTA_IMG_PENDING_VERIFY    ? "pending"
                  : state == ESP_OTA_IMG_VALID             ? "valid"
                                                           : "other",
                  unsigned(invalid != nullptr));
}
bool confirmFirmware() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY)
        return true;
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return false; // Retried later.
    Serial.println("CJAPP FIRMWARE confirmed");
    return true;
}
} // namespace board
#endif
