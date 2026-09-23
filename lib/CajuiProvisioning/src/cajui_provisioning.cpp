#include "cajui_provisioning.h"
#include <cstdio>
#include <cstring>
namespace cajui {
namespace {
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
    if (std::strlen(text) != 32) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        char byte[3] = {text[i * 2], text[i * 2 + 1], 0}; uint64_t value = 0;
        if (!hex(byte, 2, value)) return false;
        key[i] = uint8_t(value);
    }
    return true;
}
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
}
bool Provisioning::execute(const char* input, size_t length, char* reply, size_t capacity) {
    if (!reply || capacity < ReplyCapacity) return false;
    std::snprintf(reply, capacity, "CJ1 ERR INVALID");
    if (!input || !length || length >= CommandCapacity) return true;
    char buffer[CommandCapacity]{};
    for (size_t i = 0; i < length; ++i)
        if (input[i] < 32 || input[i] > 126) return true;
    std::memcpy(buffer, input, length);
    char* words[10]{}; size_t count = 0; char* cursor = buffer;
    while (*cursor) {
        if (count == 10 || *cursor == ' ') return true;
        words[count++] = cursor;
        while (*cursor && *cursor != ' ') ++cursor;
        if (*cursor) { *cursor++ = 0; if (!*cursor) return true; }
    }
    if (count < 2 || std::strcmp(words[0], "CJ1")) return true;
    const char* command = words[1];
    if (!std::strcmp(command, "HELLO") && count == 2) {
        std::snprintf(reply, capacity, "CJ1 OK HELLO %016llx %s %s %016llx %016llx %04x %u %08lx",
            static_cast<unsigned long long>(store_.device()),
            store_.role() == Role::Transmitter ? "tx" : "rx", store_.healthy() ? "ready" : "error",
            static_cast<unsigned long long>(store_.network()),
            static_cast<unsigned long long>(store_.receiver()), unsigned(store_.profile()), unsigned(store_.queued()), static_cast<unsigned long>(boot_));
        return true;
    }
    uint64_t device = 0;
    if (count < 3 || !hex(words[2], 16, device) || device != store_.device()) return true;
    // An unhealthy store stays latched until remounted, and only a restart remounts it.
    if (!std::strcmp(command, "REBOOT") && count == 3) {
        restart_ = true; std::snprintf(reply, capacity, "CJ1 OK REBOOT"); return true;
    }
    if (!store_.healthy()) { std::snprintf(reply, capacity, "CJ1 ERR STORAGE"); return true; }
    Result result = Result::Invalid;
    if (!std::strcmp(command, "PREPARE") && count == 9) {
        uint64_t network = 0, receiver = 0, node = 0, generation = 0, profile = 0; Key key{};
        if (hex(words[3], 16, network) && hex(words[4], 16, receiver) && hex(words[5], 16, node) &&
            hex(words[6], 16, generation) && parseKey(words[7], key) && hex(words[8], 4, profile))
            result = store_.prepare(network, receiver, node, generation, key, uint16_t(profile));
    } else if (count == 5) {
        uint64_t node = 0, generation = 0;
        if (!hex(words[3], 16, node) || !hex(words[4], 16, generation)) return true;
        if (!std::strcmp(command, "ACTIVATE")) result = store_.activate(node, generation);
        else if (!std::strcmp(command, "REVOKE")) result = store_.revoke(node, generation);
        else if (!std::strcmp(command, "INFO")) {
            EnrollmentInfo info{};
            if (store_.info(node, generation, info)) {
                std::snprintf(reply, capacity, "CJ1 OK INFO %u %016llx", unsigned(info.state),
                              static_cast<unsigned long long>(info.counter));
            } else std::snprintf(reply, capacity, "CJ1 ERR NOT_FOUND");
            return true;
        } else if (!std::strcmp(command, "RESERVE")) {
            EnrollmentInfo info{}; Binding binding{}; uint64_t counter = 0;
            if (store_.info(node, generation, info) && info.state == Enrollment::Active &&
                store_.binding(node, binding) && store_.reserve(binding, counter)) {
                std::snprintf(reply, capacity, "CJ1 OK RESERVE %016llx", static_cast<unsigned long long>(counter));
                return true;
            }
            result = Result::StorageError;
        }
    }
    if (result == Result::Ok) std::snprintf(reply, capacity, "CJ1 OK %s", command);
    else std::snprintf(reply, capacity, "CJ1 ERR %s", name(result));
    return true;
}
}
