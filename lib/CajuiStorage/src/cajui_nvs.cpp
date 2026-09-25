#ifdef ESP_PLATFORM
#include "cajui_nvs.h"
#include <nvs_flash.h>
namespace cajui {
NvsRecords::~NvsRecords() {
    if (opened_) nvs_close(handle_);
}
bool NvsRecords::begin() {
    if (opened_) return true;
    // Never erase on initialization failure: that could reset replay/counter state.
    if (nvs_flash_init_partition("cajui") != ESP_OK) return false;
    opened_ = nvs_open_from_partition("cajui", "store", NVS_READWRITE, &handle_) == ESP_OK;
    return opened_;
}
ReadResult NvsRecords::read(const char* name, uint8_t* out, size_t capacity, size_t& size) {
    size = 0;
    if (!opened_) return ReadResult::Error;
    auto error = nvs_get_blob(handle_, name, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ReadResult::Missing;
    if (error != ESP_OK || size > capacity) return ReadResult::Error;
    return nvs_get_blob(handle_, name, out, &size) == ESP_OK ? ReadResult::Ok : ReadResult::Error;
}
bool NvsRecords::write(const char* name, const uint8_t* input, size_t size) {
    return opened_ && size && nvs_set_blob(handle_, name, input, size) == ESP_OK &&
           nvs_commit(handle_) == ESP_OK;
}
bool NvsRecords::erase(const char* name) {
    if (!opened_) return false;
    const auto error = nvs_erase_key(handle_, name);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    return error == ESP_OK && nvs_commit(handle_) == ESP_OK;
}
}
#endif
