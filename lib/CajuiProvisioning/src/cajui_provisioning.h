#pragma once
#include "cajui_storage.h"
namespace cajui {
constexpr size_t CommandCapacity = 256, ReplyCapacity = 256;
class Provisioning {
public:
    explicit Provisioning(PersistentStore& store, uint32_t boot = 1) : store_(store), boot_(boot) {}
    // Length-delimited input, no echo and no secret-bearing response. False = output too small.
    bool execute(const char* input, size_t length, char* reply, size_t capacity);
    bool restartRequested() const { return restart_; }
private:
    PersistentStore& store_;
    bool restart_ = false;
    uint32_t boot_;
};
}
