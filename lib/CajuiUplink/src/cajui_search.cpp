// SPDX-License-Identifier: Apache-2.0
#include "cajui_search.h"
#include <cstdio>
#include <cstring>

namespace cajui {
namespace {
constexpr uint32_t BlockMask = 0xffffff00; // /24
// /31 and /32 leave no network and broadcast addresses to skip, and no other host.
constexpr uint32_t SmallestHostBits = 3;
constexpr uint8_t ConnectType = 0x10, ConnackType = 0x20, ConnackLength = 2;
constexpr uint8_t ProtocolLevel = 4; // MQTT 3.1.1
constexpr char ProtocolName[] = "MQTT";
constexpr unsigned CleanSession = 0x02, HasPassword = 0x40, HasUsername = 0x80;
constexpr uint16_t KeepaliveSeconds = 10;
constexpr size_t ConnackSize = 4, ReturnCodeAt = 3;
constexpr unsigned ByteBits = 8, LengthBits = 7, Octet1 = 24, Octet2 = 16;
constexpr uint8_t LengthMore = 0x80, LengthMask = 0x7f, ByteMask = 0xff;
uint8_t* text(uint8_t* at, const char* value, size_t length) {
    *at++ = uint8_t(length >> ByteBits);
    *at++ = uint8_t(length & ByteMask);
    std::memcpy(at, value, length);
    return at + length;
}
} // namespace

bool searchRange(uint32_t address, uint32_t netmask, SearchRange& range) {
    const uint32_t hostBits = ~netmask;
    // A contiguous mask leaves host bits of the form 0...01...1.
    if (!netmask || (hostBits & (hostBits + 1)) || hostBits < SmallestHostBits) return false;
    // The station cannot hold its network's own or broadcast address.
    const uint32_t host = address & hostBits;
    if (!host || host == hostBits) return false;
    const uint32_t mask = hostBits > MaxSearchHosts + 1 ? BlockMask : netmask;
    const uint32_t block = address & mask;
    range.first = block + 1;
    range.last = (block | ~mask) - 1;
    return true;
}
bool formatAddress(uint32_t address, char* output, size_t capacity) {
    if (!output || capacity < AddressCapacity) return false;
    std::snprintf(output, capacity, "%u.%u.%u.%u", unsigned(address >> Octet1),
                  unsigned((address >> Octet2) & ByteMask),
                  unsigned((address >> ByteBits) & ByteMask), unsigned(address & ByteMask));
    return true;
}
bool formatProbeId(uint64_t device, char* output, size_t capacity) {
    if (!output || capacity <= ProbeIdCapacity) return false;
    std::snprintf(output, capacity, "cjprobe%016llx", static_cast<unsigned long long>(device));
    return true;
}
size_t probeConnect(const char* clientId, const char* username, const char* password,
                    uint8_t* output, size_t capacity) {
    if (!clientId || !username || !password || !output) return 0;
    const size_t id = std::strlen(clientId);
    const size_t user = std::strlen(username);
    const size_t secret = std::strlen(password);
    if (!id || id > ProbeIdCapacity || !user || user > ProbeFieldCapacity || !secret ||
        secret > ProbeFieldCapacity)
        return 0;
    const size_t remaining = ProbeVariableSize + ProbePrefixSize + id + ProbePrefixSize + user +
                             ProbePrefixSize + secret;
    const bool longLength = remaining > LengthMask;
    if (capacity < (longLength ? ProbeHeaderSize : ProbeHeaderSize - 1) + remaining) return 0;
    uint8_t* at = output;
    *at++ = ConnectType;
    if (longLength) {
        *at++ = uint8_t((remaining & LengthMask) | LengthMore);
        *at++ = uint8_t(remaining >> LengthBits);
    } else {
        *at++ = uint8_t(remaining);
    }
    at = text(at, ProtocolName, sizeof(ProtocolName) - 1);
    *at++ = ProtocolLevel;
    *at++ = uint8_t(CleanSession | HasPassword | HasUsername);
    *at++ = uint8_t(KeepaliveSeconds >> ByteBits);
    *at++ = uint8_t(KeepaliveSeconds & ByteMask);
    at = text(at, clientId, id);
    at = text(at, username, user);
    at = text(at, password, secret);
    return size_t(at - output);
}
ProbeAnswer probeAnswer(const uint8_t* bytes, size_t size) {
    if (!bytes || !size) return ProbeAnswer::Incomplete;
    if (bytes[0] != ConnackType || (size > 1 && bytes[1] != ConnackLength))
        return ProbeAnswer::NotMqtt;
    if (size < ConnackSize) return ProbeAnswer::Incomplete;
    return bytes[ReturnCodeAt] ? ProbeAnswer::Refused : ProbeAnswer::Accepted;
}
bool SearchSchedule::due(uint32_t nowMs, bool wifiConnected, bool brokerOnline) {
    if (!wifiConnected || brokerOnline) {
        offline_ = false;
        return false;
    }
    if (!offline_) {
        offline_ = true;
        searched_ = false;
        since_ = nowMs;
        return false;
    }
    const uint32_t wait = searched_ ? uint32_t(RetryMs) : uint32_t(FirstDelayMs);
    if (uint32_t(nowMs - since_) < wait) return false;
    searched_ = true;
    since_ = nowMs;
    return true;
}
} // namespace cajui
