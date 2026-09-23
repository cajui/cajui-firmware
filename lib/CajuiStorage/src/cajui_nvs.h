#pragma once
#ifdef ARDUINO
#include "cajui_storage.h"
#include <nvs.h>
namespace cajui {
class NvsBlob final : public AtomicBlob {
public:
    ~NvsBlob() override;
    bool begin();
    ReadResult read(uint8_t*, size_t capacity, size_t& size) override;
    bool replace(const uint8_t*, size_t size) override;

private:
    nvs_handle_t handle_ = 0;
    bool opened_ = false;
};
}
#endif
