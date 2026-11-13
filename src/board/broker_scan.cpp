// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "broker_scan.h"
#include "cajui_search.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <cstring>

namespace board {
namespace {
// Connections attempted at once. lwIP offers 16 sockets; the setup page, its DNS, mDNS and
// the MQTT client keep some.
constexpr size_t Batch = 6;
// A host on the local network answers a connection within a few milliseconds; an unused
// address never does. 254 addresses take about thirteen seconds.
constexpr uint32_t ConnectWaitMs = 300;
constexpr uint32_t ProbeConnectMs = 1500, ProbeAnswerMs = 3000;
constexpr uint32_t MsPerSecond = 1000, UsPerMs = 1000;
constexpr size_t AnswerSize = 4;

timeval interval(uint32_t ms) {
    timeval value{};
    value.tv_sec = time_t(ms / MsPerSecond);
    value.tv_usec = suseconds_t(ms % MsPerSecond) * suseconds_t(UsPerMs);
    return value;
}
constexpr int NoSocket = -2, NoRoute = -1;
// Starts a non-blocking connection. NoSocket: none is free right now; NoRoute: this
// address cannot be tried.
int startConnection(uint32_t address, uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NoSocket;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(address);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, int(unsigned(flags) | unsigned(O_NONBLOCK))) < 0 ||
        (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0 && errno != EINPROGRESS)) {
        close(fd);
        return NoRoute;
    }
    return fd;
}
// True when the connection completed without error. Call after select() marked it.
bool established(int fd) {
    int error = 0;
    socklen_t size = sizeof(error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error;
}
bool listed(const cajui::BrokerView* found, size_t count, const char* host, uint16_t port) {
    for (size_t i = 0; i < count; ++i)
        if (found[i].port == port && !std::strcmp(found[i].host, host)) return true;
    return false;
}
} // namespace

size_t findBrokers(uint16_t port, cajui::BrokerView* found, size_t known, size_t capacity,
                   const std::atomic<bool>& cancel) {
    size_t count = known;
    const uint32_t own = ntohl(uint32_t(WiFi.localIP()));
    cajui::SearchRange range{};
    if (!cajui::searchRange(own, ntohl(uint32_t(WiFi.subnetMask())), range)) return count;
    uint32_t next = range.first;
    while (next <= range.last && count < capacity && !cancel.load()) {
        int sockets[Batch];
        uint32_t addresses[Batch];
        size_t open = 0;
        bool exhausted = false; // No socket is free: finish this batch, then try again.
        for (; open < Batch && next <= range.last; ++next) {
            if (next == own) continue;
            const int fd = startConnection(next, port);
            if (fd == NoSocket) {
                exhausted = true;
                break;
            }
            if (fd < 0) continue; // This address cannot be tried; the others may.
            sockets[open] = fd;
            addresses[open++] = next;
        }
        bool pending[Batch]{};
        size_t remaining = open;
        for (size_t i = 0; i < open; ++i) pending[i] = true;
        const uint32_t started = millis();
        while (remaining) {
            const uint32_t elapsed = millis() - started;
            if (elapsed >= ConnectWaitMs) break;
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            int highest = -1;
            for (size_t i = 0; i < open; ++i) {
                if (!pending[i]) continue;
                FD_SET(sockets[i], &writable);
                FD_SET(sockets[i], &failed);
                if (sockets[i] > highest) highest = sockets[i];
            }
            timeval wait = interval(ConnectWaitMs - elapsed);
            if (select(highest + 1, nullptr, &writable, &failed, &wait) <= 0) break;
            for (size_t i = 0; i < open; ++i) {
                if (!pending[i] ||
                    (!FD_ISSET(sockets[i], &writable) && !FD_ISSET(sockets[i], &failed)))
                    continue;
                pending[i] = false;
                --remaining;
                char host[cajui::AddressCapacity]{};
                if (!established(sockets[i]) || count == capacity ||
                    !cajui::formatAddress(addresses[i], host, sizeof(host)) ||
                    listed(found, count, host, port))
                    continue;
                std::strcpy(found[count].host, host);
                found[count++].port = port;
            }
        }
        for (size_t i = 0; i < open; ++i) close(sockets[i]);
        if (exhausted && !open) break; // Nothing could be tried at all.
    }
    return count;
}
cajui::ProbeAnswer probeBroker(const char* address, uint16_t port, const char* username,
                               const char* password, uint64_t device) {
    IPAddress target;
    if (!target.fromString(address)) return cajui::ProbeAnswer::Incomplete;
    const int fd = startConnection(ntohl(uint32_t(target)), port);
    if (fd < 0) return cajui::ProbeAnswer::Incomplete;
    auto verdict = cajui::ProbeAnswer::Incomplete;
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval wait = interval(ProbeConnectMs);
    char id[cajui::ProbeIdCapacity + 1]{};
    uint8_t packet[cajui::ProbeCapacity]{};
    size_t size = 0;
    if (select(fd + 1, nullptr, &writable, nullptr, &wait) > 0 && established(fd) &&
        cajui::formatProbeId(device, id, sizeof(id)))
        size = cajui::probeConnect(id, username, password, packet, sizeof(packet));
    if (size && send(fd, packet, size, 0) == ssize_t(size)) {
        uint8_t answer[AnswerSize]{};
        size_t received = 0;
        const uint32_t started = millis();
        while (verdict == cajui::ProbeAnswer::Incomplete) {
            const uint32_t elapsed = millis() - started;
            if (elapsed >= ProbeAnswerMs) break;
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            wait = interval(ProbeAnswerMs - elapsed);
            if (select(fd + 1, &readable, nullptr, nullptr, &wait) <= 0) break;
            const ssize_t got = recv(fd, answer + received, sizeof(answer) - received, 0);
            if (got <= 0) break; // Closed, or an error.
            received += size_t(got);
            verdict = cajui::probeAnswer(answer, received);
        }
        if (verdict == cajui::ProbeAnswer::Accepted)
            send(fd, cajui::ProbeDisconnect, sizeof(cajui::ProbeDisconnect), 0);
    }
    // The packet held the broker password.
    volatile uint8_t* bytes = packet;
    for (size_t i = 0; i < sizeof(packet); ++i) bytes[i] = 0;
    close(fd);
    return verdict;
}
} // namespace board
#endif
