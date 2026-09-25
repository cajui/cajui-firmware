#pragma once
#ifdef ESP_PLATFORM
#include "cajui_storage.h"
#include <nvs.h>
namespace cajui {
// Records as NVS blobs in the dedicated `cajui` partition, namespace `store`. NVS replaces
// one key atomically; each write is committed before returning.
class NvsRecords final : public RecordStore {
public:
    NvsRecords() = default;
    NvsRecords(const NvsRecords&) = delete; // Owns the NVS handle closed by the destructor.
    NvsRecords& operator=(const NvsRecords&) = delete;
    ~NvsRecords() override;
    bool begin();
    ReadResult read(const char* name, uint8_t*, size_t capacity, size_t& size) override;
    bool write(const char* name, const uint8_t*, size_t size) override;
    bool erase(const char* name) override;

private:
    nvs_handle_t handle_ = 0;
    bool opened_ = false;
};
}
#endif
