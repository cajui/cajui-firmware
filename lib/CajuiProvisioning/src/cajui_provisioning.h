#pragma once
#include "cajui_storage.h"
#include "cajui_uplink.h"
namespace cajui {
constexpr size_t CommandCapacity = 256, ReplyCapacity = 256;
class Provisioning {
public:
    // uplink is the receiver's separate uplink blob; nullptr disables UPLINK* commands.
    explicit Provisioning(PersistentStore& store, uint32_t boot = 1, AtomicBlob* uplink = nullptr)
        : store_(store), boot_(boot), uplink_(uplink) {}
    Provisioning(const Provisioning&) = delete;
    Provisioning& operator=(const Provisioning&) = delete;
    ~Provisioning() { wipe(pending_); }
    // Length-delimited input, no echo and no secret-bearing response. False = output too small.
    bool execute(const char* input, size_t length, char* reply, size_t capacity);
    bool restartRequested() const { return restart_; }

private:
    PersistentStore& store_;
    bool restart_ = false;
    uint32_t boot_;
    AtomicBlob* uplink_;
    UplinkConfig pending_{}; // Staged by UPLINKSET, written only by UPLINKSAVE.
    void uplink(const char* command, size_t count, char* const* words, char* reply,
                size_t capacity);
};
}
