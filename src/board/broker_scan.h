// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_setup.h"
#include "cajui_uplink.h"
#include <atomic>

namespace board {
// Finding the broker on the station's own network without an announcement
// (docs/adr/0003-broker-search.md). Both calls block for seconds: run them off the radio
// loop and the setup page task.

// Tries `port` on every address of the station's network and lists those that accept a TCP
// connection, skipping `known` entries already in the list. Returns the new total. Stops
// early when `cancel` turns true.
size_t findBrokers(uint16_t port, cajui::BrokerView* found, size_t known, size_t capacity,
                   const std::atomic<bool>& cancel);
// Asks the MQTT broker at an IPv4 address whether it accepts these credentials. Sends the
// stored password in clear, like the forwarding client: call only on the network the
// credentials were saved for.
bool brokerAccepts(const char* address, const cajui::UplinkConfig&, uint64_t device);
} // namespace board
