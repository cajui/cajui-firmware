// SPDX-License-Identifier: Apache-2.0
#include "cajui_command.h"
#include "cajui_text.h"
#include <cstring>

namespace cajui {
namespace {
using Text = TextBuffer;
constexpr size_t NodeIdDigits = 16, TypeCapacity = 32, KeyCapacity = 32, MaxDepth = 4;
// "version" is a small integer: more digits than this cannot be 1 and are refused.
constexpr size_t MaxIntegerDigits = 9;
constexpr uint32_t DecimalBase = 10;

bool idChar(char c, bool first) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return alnum || (!first && (c == '.' || c == '_' || c == ':' || c == '-'));
}
bool validId(const char* id) {
    size_t length = 0;
    for (; id[length]; ++length)
        if (!idChar(id[length], length == 0)) return false;
    return length >= 1 && length <= CommandIdCapacity;
}
bool parseNode(const char* text, uint64_t& node) {
    if (std::strlen(text) != NodeIdDigits) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < NodeIdDigits; ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) return false;
        value = value << 4 | uint64_t(digit);
    }
    node = value;
    return true;
}

// A cursor over the payload for the small, closed grammar of commands.
class Reader {
public:
    Reader(const char* data, size_t size) : data_(data), size_(size) {}
    void space() {
        while (at_ < size_ && (data_[at_] == ' ' || data_[at_] == '\t' || data_[at_] == '\n' ||
                               data_[at_] == '\r'))
            ++at_;
    }
    bool take(char c) {
        space();
        if (at_ >= size_ || data_[at_] != c) return false;
        ++at_;
        return true;
    }
    bool peek(char c) {
        space();
        return at_ < size_ && data_[at_] == c;
    }
    // A string without escapes or control characters, copied when it fits.
    bool string(char* output, size_t capacity) {
        if (!take('"')) return false;
        size_t length = 0;
        while (at_ < size_ && data_[at_] != '"') {
            const char c = data_[at_++];
            if (c == '\\' || uint8_t(c) < 0x20 || length + 1 >= capacity) return false;
            output[length++] = c;
        }
        if (at_ >= size_) return false;
        ++at_;
        output[length] = 0;
        return true;
    }
    bool integer(uint32_t& value) {
        space();
        const size_t start = at_;
        uint32_t result = 0;
        while (at_ < size_ && data_[at_] >= '0' && data_[at_] <= '9' &&
               at_ - start < MaxIntegerDigits)
            result = result * DecimalBase + uint32_t(data_[at_++] - '0');
        value = result;
        // JSON forbids leading zeros; a longer number is refused rather than truncated.
        return at_ > start && !(data_[start] == '0' && at_ - start > 1) &&
               (at_ >= size_ || data_[at_] < '0' || data_[at_] > '9');
    }
    bool end() {
        space();
        return at_ == size_;
    }
    // Skips any value of the reserved commands' params without interpreting it: a string
    // without escapes, a number, true/false/null, or a bounded nest of objects and arrays.
    bool skip(size_t depth = 0) {
        space();
        if (at_ >= size_ || depth > MaxDepth) return false;
        const char c = data_[at_];
        if (c == '"') {
            char ignored[CommandPayloadCapacity + 1];
            return string(ignored, sizeof(ignored));
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++at_;
            if (take(close)) return true;
            do {
                if (c == '{') {
                    char key[KeyCapacity]{};
                    if (!string(key, sizeof(key)) || !take(':')) return false;
                }
                if (!skip(depth + 1)) return false;
            } while (take(','));
            return take(close);
        }
        const size_t start = at_;
        while (at_ < size_ && std::strchr("-+.eE0123456789truefalsn", data_[at_])) ++at_;
        return at_ > start;
    }

private:
    const char* data_;
    size_t size_, at_ = 0;
};
// Implemented commands take at most node_id. Other keys are only skipped here: a reserved
// type with its own params must still be answered unsupported, not invalid.
bool parseParams(Reader& in, bool& hasNode, bool& other, char* node, size_t capacity) {
    if (!in.take('{')) return false;
    if (in.take('}')) return true;
    do {
        char key[KeyCapacity]{};
        if (!in.string(key, sizeof(key)) || !in.take(':')) return false;
        if (std::strcmp(key, "node_id") == 0 && !hasNode) {
            hasNode = true;
            if (!in.string(node, capacity)) return false;
        } else {
            other = true;
            if (!in.skip()) return false;
        }
    } while (in.take(','));
    return in.take('}');
}
const char* statusName(CommandStatus status) {
    switch (status) {
    case CommandStatus::Applied: return "applied";
    case CommandStatus::Pending: return "pending";
    case CommandStatus::Rejected: return "rejected";
    }
    return nullptr;
}
const char* reasonName(CommandReason reason) {
    switch (reason) {
    case CommandReason::None: return nullptr;
    case CommandReason::Invalid: return "invalid";
    case CommandReason::Unsupported: return "unsupported";
    case CommandReason::Busy: return "busy";
    case CommandReason::UnknownNode: return "unknown_node";
    case CommandReason::Closed: return "closed";
    case CommandReason::NotRequested: return "not_requested";
    case CommandReason::Conflict: return "conflict";
    case CommandReason::Full: return "full";
    case CommandReason::Superseded: return "superseded";
    case CommandReason::Storage: return "storage";
    case CommandReason::Failed: return "failed";
    }
    return nullptr;
}
} // namespace

ParseResult parseCommand(const char* payload, size_t size, Command& command) {
    command = Command{};
    if (!payload || size > CommandPayloadCapacity) return ParseResult::Unreadable;
    Reader in(payload, size);
    bool version = false;
    bool id = false;
    bool type = false;
    bool params = false;
    bool hasNode = false;
    bool otherParams = false;
    bool ok = true;
    char typeName[TypeCapacity]{};
    char node[NodeIdDigits + 2]{};
    if (!in.take('{')) return ParseResult::Unreadable;
    // Keep reading after a bad field so that a readable command_id can still be answered.
    do {
        char key[KeyCapacity]{};
        if (!in.string(key, sizeof(key)) || !in.take(':')) {
            ok = false;
            break;
        }
        uint32_t number = 0;
        if (std::strcmp(key, "version") == 0 && !version) {
            version = in.integer(number) && number == 1;
            ok = ok && version;
        } else if (std::strcmp(key, "command_id") == 0 && !id) {
            id = in.string(command.id, sizeof(command.id)) && validId(command.id);
            if (!id) command.id[0] = 0;
            ok = ok && id;
        } else if (std::strcmp(key, "type") == 0 && !type) {
            type = in.string(typeName, sizeof(typeName));
            ok = ok && type;
        } else if (std::strcmp(key, "params") == 0 && !params) {
            params = parseParams(in, hasNode, otherParams, node, sizeof(node));
            ok = ok && params;
        } else {
            ok = false;
            break;
        }
    } while (in.take(','));
    if (!id) return ParseResult::Unreadable;
    if (!ok || !version || !type || !params || !in.take('}') || !in.end())
        return ParseResult::Invalid;
    const bool needsNode =
        std::strcmp(typeName, "pairing.accept") == 0 || std::strcmp(typeName, "node.revoke") == 0;
    if (std::strcmp(typeName, "pairing.open") == 0)
        command.type = CommandType::PairingOpen;
    else if (std::strcmp(typeName, "pairing.accept") == 0)
        command.type = CommandType::PairingAccept;
    else if (std::strcmp(typeName, "pairing.close") == 0)
        command.type = CommandType::PairingClose;
    else if (std::strcmp(typeName, "node.revoke") == 0)
        command.type = CommandType::NodeRevoke;
    else
        return ParseResult::Unsupported;
    if (otherParams || needsNode != hasNode || (hasNode && !parseNode(node, command.node)))
        return ParseResult::Invalid;
    return ParseResult::Ok;
}

bool parseCommandTopic(const char* topic, const char* source, uint64_t& device) {
    if (!topic || !source) return false;
    static const char Prefix[] = "manage/v1/";
    static const char Suffix[] = "/commands";
    const size_t prefix = sizeof(Prefix) - 1;
    const size_t suffix = sizeof(Suffix) - 1;
    const size_t length = std::strlen(topic);
    const size_t sourceLength = std::strlen(source);
    if (length != prefix + sourceLength + 1 + NodeIdDigits + suffix ||
        std::strncmp(topic, Prefix, prefix) != 0 ||
        std::strncmp(topic + prefix, source, sourceLength) != 0 ||
        topic[prefix + sourceLength] != '/' ||
        std::strcmp(topic + prefix + sourceLength + 1 + NodeIdDigits, Suffix) != 0)
        return false;
    char digits[NodeIdDigits + 1]{};
    std::memcpy(digits, topic + prefix + sourceLength + 1, NodeIdDigits);
    return parseNode(digits, device);
}

bool formatResult(const char* id, CommandStatus status, CommandReason reason, char* output,
                  size_t capacity, size_t& size) {
    size = 0;
    const char* name = statusName(status);
    const char* why = reasonName(reason);
    if (!id || !validId(id) || !name || !output || !capacity ||
        (status == CommandStatus::Rejected) != (why != nullptr))
        return false;
    Text text(output, capacity);
    text.format("{\"version\":1,\"command_id\":\"%s\",\"status\":\"%s\",\"reason\":", id, name);
    if (why)
        text.format("\"%s\"}", why);
    else
        text.format("null}");
    if (!text.ok()) return false;
    size = text.size();
    return true;
}

CommandRunner::CommandRunner(PairingHost& pairing, PersistentStore& store, Clock& clock)
    : pairing_(pairing), store_(store), clock_(clock) {}

CommandRunner::Entry* CommandRunner::find(uint64_t device, const char* id) {
    for (auto& entry : entries_)
        if (entry.id[0] && entry.device == device && std::strcmp(entry.id, id) == 0) return &entry;
    return nullptr;
}
CommandRunner::Entry& CommandRunner::remember(uint64_t device, const char* id, CommandStatus status,
                                              CommandReason reason) {
    Entry* slot = &entries_[next_];
    next_ = (next_ + 1) % Remembered;
    // Never overwrite the pending accept: its later result still needs the command_id.
    if (slot == pending_) {
        slot = &entries_[next_];
        next_ = (next_ + 1) % Remembered;
    }
    std::memcpy(slot->id, id, std::strlen(id) + 1);
    slot->device = device;
    slot->status = status;
    slot->reason = reason;
    return *slot;
}
void CommandRunner::finish(Entry& entry, CommandStatus status, CommandReason reason,
                           ResultSink& sink) {
    entry.status = status;
    entry.reason = reason;
    sink.result(entry.device, entry.id, status, reason);
}
void CommandRunner::execute(uint64_t device, const char* payload, size_t size, uint32_t receivedAt,
                            ResultSink& sink) {
    Command command{};
    const ParseResult parsed = parseCommand(payload, size, command);
    if (parsed == ParseResult::Unreadable) return;
    if (Entry* seen = find(device, command.id)) {
        sink.result(seen->device, seen->id, seen->status, seen->reason);
        return;
    }
    const CommandReason early = parsed == ParseResult::Invalid       ? CommandReason::Invalid
                                : parsed == ParseResult::Unsupported ? CommandReason::Unsupported
                                : uint32_t(clock_.nowMs() - receivedAt) > CommandDeadlineMs
                                    ? CommandReason::Busy
                                    : CommandReason::None;
    if (early != CommandReason::None) {
        finish(remember(device, command.id, CommandStatus::Rejected, early),
               CommandStatus::Rejected, early, sink);
        return;
    }
    run(device, command, sink);
}
bool CommandRunner::known(uint64_t node) const {
    EnrollmentInfo list[BindingCapacity]{};
    const size_t count = store_.list(list, BindingCapacity);
    for (size_t i = 0; i < count; ++i)
        if (list[i].node == node) return true;
    return false;
}
CommandReason CommandRunner::revoke(uint64_t node) {
    EnrollmentInfo list[BindingCapacity]{};
    const size_t count = store_.list(list, BindingCapacity);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (list[i].node != node) continue;
        found = true;
        if (list[i].state != Enrollment::Active && list[i].state != Enrollment::Prepared) continue;
        const Result result = store_.revoke(node, list[i].generation);
        if (result == Result::StorageError) return CommandReason::Storage;
        if (result != Result::Ok) return CommandReason::Failed;
    }
    return found ? CommandReason::None : CommandReason::UnknownNode;
}
void CommandRunner::run(uint64_t device, const Command& command, ResultSink& sink) {
    auto answer = [&](CommandStatus status, CommandReason reason) {
        finish(remember(device, command.id, status, reason), status, reason, sink);
    };
    if (device != store_.device()) {
        // Version 1 has no transmitter commands.
        answer(CommandStatus::Rejected,
               known(device) ? CommandReason::Unsupported : CommandReason::UnknownNode);
        return;
    }
    switch (command.type) {
    case CommandType::PairingOpen:
        // An open window is left as it is: its requests and offer may come from the page.
        if (pairing_.state() == HostState::Closed) pairing_.open();
        answer(CommandStatus::Applied, CommandReason::None);
        return;
    case CommandType::PairingClose:
        pairing_.close();
        answer(CommandStatus::Applied, CommandReason::None);
        poll(sink);
        return;
    case CommandType::NodeRevoke: {
        const CommandReason reason = revoke(command.node);
        answer(reason == CommandReason::None ? CommandStatus::Applied : CommandStatus::Rejected,
               reason);
        return;
    }
    case CommandType::PairingAccept: break;
    }
    const Result result = pairing_.accept(command.node);
    if (result != Result::Ok) {
        answer(CommandStatus::Rejected, result == Result::Invalid    ? CommandReason::Closed
                                        : result == Result::NotFound ? CommandReason::NotRequested
                                        : result == Result::Conflict ? CommandReason::Conflict
                                        : result == Result::Full     ? CommandReason::Full
                                        : result == Result::StorageError ? CommandReason::Storage
                                                                         : CommandReason::Failed);
        return;
    }
    // The new offer replaced any earlier one.
    if (pending_) finish(*pending_, CommandStatus::Rejected, CommandReason::Superseded, sink);
    pending_ = nullptr;
    Entry& entry = remember(device, command.id, CommandStatus::Pending, CommandReason::None);
    pending_ = &entry;
    pendingNode_ = command.node;
    sink.result(device, entry.id, CommandStatus::Pending, CommandReason::None);
}
void CommandRunner::poll(ResultSink& sink) {
    if (!pending_) return;
    pairing_.poll();
    CommandStatus status = CommandStatus::Pending;
    CommandReason reason = CommandReason::None;
    if (pairing_.pairedNode() == pendingNode_) {
        status = CommandStatus::Applied;
    } else if (pairing_.state() == HostState::Closed) {
        status = CommandStatus::Rejected;
        reason = CommandReason::Closed;
    } else if (pairing_.offeredNode() != pendingNode_) {
        // Dropped after a key conflict for the node, or replaced from the setup page.
        bool conflict = false;
        for (size_t i = 0; i < pairing_.candidateCount(); ++i)
            conflict = conflict || (pairing_.candidates()[i].node == pendingNode_ &&
                                    pairing_.candidates()[i].conflict);
        status = CommandStatus::Rejected;
        reason = conflict ? CommandReason::Conflict : CommandReason::Superseded;
    }
    if (status == CommandStatus::Pending) return;
    Entry& entry = *pending_;
    pending_ = nullptr;
    finish(entry, status, reason, sink);
}
} // namespace cajui
