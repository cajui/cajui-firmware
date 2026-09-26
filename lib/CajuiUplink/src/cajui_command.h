// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_pairing.h"
#include "cajui_storage.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// Commands of the MQTT management channel v1, docs/management-v1.md.
constexpr size_t CommandIdCapacity = 64, CommandPayloadCapacity = 512, ResultCapacity = 160;
constexpr uint32_t CommandDeadlineMs = 5000;
enum class CommandType { PairingOpen, PairingAccept, PairingClose, NodeRevoke };
struct Command {
    char id[CommandIdCapacity + 1]{};
    CommandType type = CommandType::PairingOpen;
    uint64_t node = 0; // node_id of pairing.accept and node.revoke.
};
// Unreadable: no usable command_id, so no result can be addressed. Invalid and Unsupported
// carry the command_id.
enum class ParseResult { Ok, Invalid, Unsupported, Unreadable };
// Strict: one object with exactly version 1, command_id, type and params; no unknown or
// duplicate keys, no string escapes, nothing after the object.
ParseResult parseCommand(const char* payload, size_t size, Command&);
// manage/v1/<source>/<16 lowercase hex digits>/commands for this source.
bool parseCommandTopic(const char* topic, const char* source, uint64_t& device);

enum class CommandStatus { Applied, Pending, Rejected };
enum class CommandReason {
    None,
    Invalid,
    Unsupported,
    Busy,
    UnknownNode,
    Closed,
    NotRequested,
    Conflict,
    Full,
    Superseded,
    Storage,
    Failed
};
bool formatResult(const char* id, CommandStatus, CommandReason, char* output, size_t capacity,
                  size_t& size);

class ResultSink {
public:
    virtual ~ResultSink() = default;
    // device: the device ID of the command's topic.
    virtual void result(uint64_t device, const char* id, CommandStatus, CommandReason) = 0;
};

// Executes commands for one receiver under the caller's lock, only while its radio is
// listening (revocation writes flash). A repeated command_id gets its stored result again
// instead of running twice; a pairing.accept answered pending gets exactly one later result
// from poll().
class CommandRunner final {
public:
    static constexpr size_t Remembered = 8;
    CommandRunner(PairingHost&, PersistentStore&, Clock&);
    CommandRunner(const CommandRunner&) = delete;
    CommandRunner& operator=(const CommandRunner&) = delete;
    void execute(uint64_t device, const char* payload, size_t size, uint32_t receivedAt,
                 ResultSink&);
    void poll(ResultSink&);

private:
    struct Entry {
        char id[CommandIdCapacity + 1]{};
        uint64_t device = 0;
        CommandStatus status = CommandStatus::Rejected;
        CommandReason reason = CommandReason::None;
    };
    PairingHost& pairing_;
    PersistentStore& store_;
    Clock& clock_;
    Entry entries_[Remembered]{};
    size_t next_ = 0;
    // The accept waiting for its node to confirm, if any.
    Entry* pending_ = nullptr;
    uint64_t pendingNode_ = 0;
    Entry* find(uint64_t device, const char* id);
    Entry& remember(uint64_t device, const char* id, CommandStatus, CommandReason);
    void finish(Entry&, CommandStatus, CommandReason, ResultSink&);
    void run(uint64_t device, const Command&, ResultSink&);
    bool known(uint64_t node) const;
    CommandReason revoke(uint64_t node);
};
} // namespace cajui
