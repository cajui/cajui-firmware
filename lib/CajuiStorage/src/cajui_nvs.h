#pragma once
#ifdef ESP_PLATFORM
#include "cajui_storage.h"
#include <nvs.h>
namespace cajui {
class NvsBlob final : public AtomicBlob {
public:
    // One NVS key per blob; sizes outside [minimum, maximum] are never written.
    explicit NvsBlob(const char* key = "snapshot", size_t minimum = MinSnapshotSize,
                     size_t maximum = SnapshotSize)
        : key_(key), minimum_(minimum), maximum_(maximum) {}
    NvsBlob(const NvsBlob&) = delete; // Owns the NVS handle closed by the destructor.
    NvsBlob& operator=(const NvsBlob&) = delete;
    ~NvsBlob() override;
    bool begin();
    ReadResult read(uint8_t*, size_t capacity, size_t& size) override;
    bool replace(const uint8_t*, size_t size) override;

private:
    const char* key_;
    size_t minimum_, maximum_;
    nvs_handle_t handle_ = 0;
    bool opened_ = false;
};
}
#endif
