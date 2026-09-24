#include "cajui_provisioning.h"
#include <cstdio>
#include <cstring>
namespace cajui {
namespace {
// Word counts include "CJ1", the command and the device ID.
constexpr size_t MaxWords = 10, RebootWords = 3, EnrollmentWords = 5, PrepareWords = 9,
                 UplinkSetWords = 5;
constexpr size_t IdDigits = 16, ProfileDigits = 4;
constexpr char FirstPrintable = ' ', LastPrintable = '~';
bool hex(const char* text, size_t length, uint64_t& result) {
    if (std::strlen(text) != length) return false;
    result = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (n < 0) return false;
        result = (result << 4) | unsigned(n);
    }
    return true;
}
bool parseKey(const char* text, Key& key) {
    if (std::strlen(text) != key.size() * 2) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        char byte[3] = {text[i * 2], text[i * 2 + 1], 0};
        uint64_t value = 0;
        if (!hex(byte, 2, value)) return false;
        key[i] = uint8_t(value);
    }
    return true;
}
// Reads command arguments in their documented order.
class Fields {
public:
    explicit Fields(char* const* words) : next_(words) {}
    bool id(uint64_t& value) { return hex(*next_++, IdDigits, value); }
    bool key(Key& value) { return parseKey(*next_++, value); }
    bool profile(uint16_t& value) {
        uint64_t parsed = 0;
        if (!hex(*next_++, ProfileDigits, parsed)) return false;
        value = uint16_t(parsed);
        return true;
    }

private:
    char* const* next_;
};
const char* name(Result r) {
    switch (r) {
    case Result::Ok: return "OK";
    case Result::Full: return "FULL";
    case Result::Conflict: return "CONFLICT";
    case Result::StorageError: return "STORAGE";
    case Result::Unauthorized: return "UNAUTHORIZED";
    case Result::NotFound: return "NOT_FOUND";
    default: return "INVALID";
    }
}
const char* name(Health h) {
    switch (h) {
    case Health::Ready: return "ready";
    case Health::Unmounted: return "unmounted";
    case Health::Identity: return "identity";
    case Health::ReadError: return "read";
    case Health::Corrupt: return "corrupt";
    case Health::Format: return "format";
    case Health::Role: return "role";
    case Health::Device: return "device";
    case Health::Invalid: return "invalid";
    case Health::WriteError: return "write";
    }
    return "unknown";
}
// Splits on single spaces; returns 0 for empty, doubled or trailing separators or too many words.
size_t tokenize(char* cursor, char* words[MaxWords]) {
    size_t count = 0;
    while (*cursor) {
        if (count == MaxWords || *cursor == ' ') return 0;
        words[count++] = cursor;
        while (*cursor && *cursor != ' ') ++cursor;
        if (*cursor) {
            *cursor++ = 0;
            if (!*cursor) return 0;
        }
    }
    return count;
}
void respond(Result result, const char* command, char* reply, size_t capacity) {
    if (result == Result::Ok)
        std::snprintf(reply, capacity, "CJ1 OK %s", command);
    else
        std::snprintf(reply, capacity, "CJ1 ERR %s", name(result));
}
void hello(const PersistentStore& store, uint32_t boot, ConsoleMode mode, char* reply,
           size_t capacity) {
    std::snprintf(reply, capacity, "CJ1 OK HELLO %016llx %s %s %016llx %016llx %04x %u %08lx %s",
                  static_cast<unsigned long long>(store.device()),
                  store.role() == Role::Transmitter ? "tx" : "rx", name(store.health()),
                  static_cast<unsigned long long>(store.network()),
                  static_cast<unsigned long long>(store.receiver()), unsigned(store.profile()),
                  unsigned(store.queued()), static_cast<unsigned long>(boot),
                  mode == ConsoleMode::Admin ? "admin" : "run");
}
Result prepare(PersistentStore& store, Fields& fields) {
    uint64_t network = 0;
    uint64_t receiver = 0;
    uint64_t node = 0;
    uint64_t generation = 0;
    uint16_t profile = 0;
    Key key{};
    if (!fields.id(network) || !fields.id(receiver) || !fields.id(node) || !fields.id(generation) ||
        !fields.key(key) || !fields.profile(profile))
        return Result::Invalid;
    return store.prepare(network, receiver, node, generation, key, profile);
}
void info(const PersistentStore& store, uint64_t node, uint64_t generation, char* reply,
          size_t capacity) {
    EnrollmentInfo info{};
    if (!store.info(node, generation, info)) {
        respond(Result::NotFound, "INFO", reply, capacity);
        return;
    }
    std::snprintf(reply, capacity, "CJ1 OK INFO %u %016llx", unsigned(info.state),
                  static_cast<unsigned long long>(info.counter));
}
void reserve(PersistentStore& store, uint64_t node, uint64_t generation, char* reply,
             size_t capacity) {
    EnrollmentInfo info{};
    Binding binding{};
    uint64_t counter = 0;
    Result result = Result::StorageError;
    if (store.role() != Role::Transmitter)
        result = Result::Invalid;
    else if (!store.info(node, generation, info))
        result = Result::NotFound;
    else if (info.state != Enrollment::Active)
        result = Result::Conflict;
    else if (store.binding(node, binding) && store.reserve(binding, counter)) {
        std::snprintf(reply, capacity, "CJ1 OK RESERVE %016llx",
                      static_cast<unsigned long long>(counter));
        return;
    }
    respond(result, "RESERVE", reply, capacity);
}
// Decodes a lower-case hex value of 1..capacity bytes without NUL bytes into a C string.
bool hexText(const char* text, char* output, size_t capacity) {
    const size_t length = std::strlen(text);
    if (!length || length % 2 || length / 2 > capacity) return false;
    for (size_t i = 0; i < length / 2; ++i) {
        const char byte[3] = {text[i * 2], text[i * 2 + 1], 0};
        uint64_t value = 0;
        if (!hex(byte, 2, value) || !value) return false;
        output[i] = char(value);
    }
    output[length / 2] = 0;
    return true;
}
bool setField(UplinkConfig& pending, const char* field, const char* value) {
    struct Target {
        const char* name;
        char* output;
        size_t capacity;
    };
    const Target targets[] = {{"ssid", pending.ssid, SsidCapacity},
                              {"wifipass", pending.wifiPassword, WifiPasswordCapacity},
                              {"host", pending.host, HostCapacity},
                              {"user", pending.username, UsernameCapacity},
                              {"pass", pending.password, MqttPasswordCapacity}};
    for (const auto& target : targets)
        if (!std::strcmp(field, target.name)) return hexText(value, target.output, target.capacity);
    if (std::strcmp(field, "port") != 0) return false;
    constexpr size_t PortDigits = 5;
    constexpr unsigned long MaxPort = 65535;
    constexpr unsigned long Decimal = 10;
    char digits[PortDigits + 1]{};
    if (!hexText(value, digits, PortDigits)) return false;
    unsigned long port = 0;
    for (const char* c = digits; *c; ++c) {
        if (*c < '0' || *c > '9') return false;
        port = port * Decimal + unsigned(*c - '0');
    }
    if (!port || port > MaxPort) return false;
    pending.port = uint16_t(port);
    return true;
}
}
void Provisioning::uplink(const char* command, size_t count, char* const* words, char* reply,
                          size_t capacity) {
    if (!uplink_ || store_.role() != Role::Receiver) return; // Reply stays CJ1 ERR INVALID.
    if (mode_ == ConsoleMode::Operation && std::strcmp(command, "UPLINKINFO") != 0) {
        std::snprintf(reply, capacity, "CJ1 ERR ADMIN");
        return;
    }
    if (!std::strcmp(command, "UPLINKSET") && count == UplinkSetWords) {
        respond(setField(pending_, words[3], words[4]) ? Result::Ok : Result::Invalid, command,
                reply, capacity);
        return;
    }
    if (count != RebootWords) return;
    if (!std::strcmp(command, "UPLINKSAVE")) {
        const Result result = !validUplink(pending_)           ? Result::Invalid
                              : saveUplink(*uplink_, pending_) ? Result::Ok
                                                               : Result::StorageError;
        if (result == Result::Ok) wipe(pending_);
        respond(result, command, reply, capacity);
    } else if (!std::strcmp(command, "UPLINKINFO")) {
        UplinkConfig stored{};
        const auto loaded = loadUplink(*uplink_, stored);
        if (loaded == ReadResult::Ok)
            std::snprintf(reply, capacity, "CJ1 OK UPLINKINFO 1 %s %u %s", stored.host,
                          unsigned(stored.port), stored.username);
        else if (loaded == ReadResult::Missing)
            std::snprintf(reply, capacity, "CJ1 OK UPLINKINFO 0");
        else
            respond(Result::StorageError, command, reply, capacity);
        wipe(stored);
    }
}
bool Provisioning::execute(const char* input, size_t length, char* reply, size_t capacity) {
    if (!reply || capacity < ReplyCapacity) return false;
    std::snprintf(reply, capacity, "CJ1 ERR INVALID");
    if (!input || !length || length >= CommandCapacity) return true;
    for (size_t i = 0; i < length; ++i)
        if (input[i] < FirstPrintable || input[i] > LastPrintable) return true;
    char buffer[CommandCapacity]{};
    std::memcpy(buffer, input, length);
    char* words[MaxWords]{};
    const size_t count = tokenize(buffer, words);
    if (count < 2 || std::strcmp(words[0], "CJ1") != 0) return true;
    const char* command = words[1];
    if (!std::strcmp(command, "HELLO") && count == 2) {
        hello(store_, boot_, mode_, reply, capacity);
        return true;
    }
    Fields fields(words + 2);
    uint64_t device = 0;
    if (count < RebootWords || !fields.id(device) || device != store_.device()) return true;
    // An unhealthy store stays latched until remounted, and only a restart remounts it.
    if (!std::strcmp(command, "REBOOT") && count == RebootWords) {
        restart_ = true;
        std::snprintf(reply, capacity, "CJ1 OK REBOOT");
        return true;
    }
    if (!std::strcmp(command, "ADMIN") && count == RebootWords) {
        restart_ = admin_ = true;
        std::snprintf(reply, capacity, "CJ1 OK ADMIN");
        return true;
    }
    if (!std::strncmp(command, "UPLINK", std::strlen("UPLINK"))) {
        uplink(command, count, words, reply, capacity);
        return true;
    }
    if (mode_ == ConsoleMode::Operation && std::strcmp(command, "INFO") != 0) {
        std::snprintf(reply, capacity, "CJ1 ERR ADMIN"); // Mutations need the radio stopped.
        return true;
    }
    if (!store_.healthy()) {
        std::snprintf(reply, capacity, "CJ1 ERR STORAGE");
        return true;
    }
    if (!std::strcmp(command, "PREPARE") && count == PrepareWords) {
        respond(prepare(store_, fields), command, reply, capacity);
        return true;
    }
    uint64_t node = 0;
    uint64_t generation = 0;
    if (count != EnrollmentWords || !fields.id(node) || !fields.id(generation)) return true;
    if (!std::strcmp(command, "INFO"))
        info(store_, node, generation, reply, capacity);
    else if (!std::strcmp(command, "RESERVE"))
        reserve(store_, node, generation, reply, capacity);
    else if (!std::strcmp(command, "ACTIVATE"))
        respond(store_.activate(node, generation), command, reply, capacity);
    else if (!std::strcmp(command, "REVOKE"))
        respond(store_.revoke(node, generation), command, reply, capacity);
    return true;
}
}
