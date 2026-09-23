#ifdef ESP_PLATFORM
#include "cajui_nvs.h"
#include <nvs_flash.h>
namespace cajui {
NvsBlob::~NvsBlob() {
    if (opened_) nvs_close(handle_);
}
bool NvsBlob::begin() {
    if (opened_) return true;
    // Never erase on initialization failure: that could reset replay/counter state.
    if (nvs_flash_init_partition("cajui") != ESP_OK) return false;
    opened_ = nvs_open_from_partition("cajui", "store", NVS_READWRITE, &handle_) == ESP_OK;
    return opened_;
}
ReadResult NvsBlob::read(uint8_t* out, size_t capacity, size_t& size) {
    size = 0;
    if (!opened_) return ReadResult::Error;
    auto error = nvs_get_blob(handle_, "snapshot", nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ReadResult::Missing;
    if (error != ESP_OK || size > capacity) return ReadResult::Error;
    return nvs_get_blob(handle_, "snapshot", out, &size) == ESP_OK ? ReadResult::Ok
                                                                   : ReadResult::Error;
}
bool NvsBlob::replace(const uint8_t* input, size_t size) {
    return opened_ && size >= MinSnapshotSize && size <= SnapshotSize &&
           nvs_set_blob(handle_, "snapshot", input, size) == ESP_OK &&
           nvs_commit(handle_) == ESP_OK;
}
}
#endif
