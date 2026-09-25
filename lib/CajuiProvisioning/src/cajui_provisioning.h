// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_device.h"
#include "cajui_storage.h"
#include "cajui_uplink.h"
namespace cajui {
constexpr size_t CommandCapacity = 256, ReplyCapacity = 256;
// Admin: full administration with the radio stopped. Operation: the radio application is
// running, so only read-only queries and the request to restart into Admin are accepted.
enum class ConsoleMode { Admin, Operation };
class Provisioning {
public:
    // uplink is the receiver's separate uplink blob; nullptr disables UPLINK* commands.
    // radio holds the configured transmit power; nullptr disables POWER.
    explicit Provisioning(PersistentStore& store, uint32_t boot = 1, AtomicBlob* uplink = nullptr,
                          ConsoleMode mode = ConsoleMode::Admin, AtomicBlob* radio = nullptr)
        : store_(store), boot_(boot), uplink_(uplink), mode_(mode), radio_(radio) {}
    Provisioning(const Provisioning&) = delete;
    Provisioning& operator=(const Provisioning&) = delete;
    ~Provisioning() { wipe(pending_); }
    // Length-delimited input, no echo and no secret-bearing response. False = output too small.
    bool execute(const char* input, size_t length, char* reply, size_t capacity);
    bool restartRequested() const { return restart_; }
    // Set with restartRequested(): the next boot must start in ConsoleMode::Admin.
    bool adminRequested() const { return admin_; }
    // Set with restartRequested(): the next boot must start radio pairing (transmitter).
    bool pairRequested() const { return pair_; }

private:
    PersistentStore& store_;
    bool restart_ = false, admin_ = false, pair_ = false;
    uint32_t boot_;
    AtomicBlob* uplink_;
    ConsoleMode mode_;
    AtomicBlob* radio_;
    UplinkConfig pending_{}; // Staged by UPLINKSET, written only by UPLINKSAVE.
    void uplink(const char* command, size_t count, char* const* words, char* reply,
                size_t capacity);
    void power(size_t count, char* const* words, char* reply, size_t capacity);
};
}
