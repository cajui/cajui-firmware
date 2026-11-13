// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace cajui {
// Finding the broker without an announcement (docs/adr/0003-broker-search.md): the receiver
// tries the broker port on the addresses of its own network. The setup page lists what
// answers; a receiver in operation that lost its broker asks each one whether it accepts
// the stored credentials.
constexpr uint16_t DefaultMqttPort = 1883;
// Inclusive range of IPv4 addresses, in host byte order.
struct SearchRange {
    uint32_t first = 0, last = 0;
};
constexpr uint32_t MaxSearchHosts = 254;
// The addresses of the station's network, without its network and broadcast addresses. A
// network larger than MaxSearchHosts is narrowed to the /24 block around the station, so a
// search is bounded. False for a mask that is not contiguous or that leaves no other host.
bool searchRange(uint32_t address, uint32_t netmask, SearchRange&);
constexpr size_t AddressCapacity = sizeof("255.255.255.255");
// Dotted decimal text of a host byte order address.
bool formatAddress(uint32_t address, char* output, size_t capacity);

// MQTT 3.1.1 CONNECT with a clean session and no will: enough to learn whether a broker
// accepts a username and password. The packet carries the password in clear, like every
// connection of this plain MQTT uplink; callers wipe it after use.
constexpr size_t ProbeIdCapacity = 23;    // Longest client ID every 3.1.1 broker accepts.
constexpr size_t ProbeFieldCapacity = 64; // Username and password, as in UplinkConfig.
// Fixed header (type plus a two-byte length), variable header, three length-prefixed fields.
constexpr size_t ProbeHeaderSize = 3, ProbeVariableSize = 10, ProbePrefixSize = 2;
constexpr size_t ProbeCapacity = ProbeHeaderSize + ProbeVariableSize + ProbePrefixSize +
                                 ProbeIdCapacity + ProbePrefixSize + ProbeFieldCapacity +
                                 ProbePrefixSize + ProbeFieldCapacity;
constexpr uint8_t ProbeDisconnect[] = {0xe0, 0x00};
// Client ID of a probe: distinct from the forwarding client's, so a probe never takes
// over a live session of this receiver.
bool formatProbeId(uint64_t device, char* output, size_t capacity);
// Returns the packet size, or 0 when a field is empty or does not fit.
size_t probeConnect(const char* clientId, const char* username, const char* password,
                    uint8_t* output, size_t capacity);
enum class ProbeAnswer { Incomplete, Accepted, Refused, NotMqtt };
// Reads the first bytes a candidate sent back. Only a CONNACK with return code 0 accepts.
ProbeAnswer probeAnswer(const uint8_t* bytes, size_t size);

// When a receiver in operation searches again: after the broker has been unreachable for
// FirstDelayMs with Wi-Fi connected, then every RetryMs while that lasts. A broker that
// restarts comes back at the same address well before the first search.
class SearchSchedule final {
public:
    static constexpr uint32_t FirstDelayMs = 120000, RetryMs = 300000;
    // Call regularly; true once each time a search is due.
    bool due(uint32_t nowMs, bool wifiConnected, bool brokerOnline);

private:
    bool offline_ = false, searched_ = false;
    uint32_t since_ = 0;
};
} // namespace cajui
