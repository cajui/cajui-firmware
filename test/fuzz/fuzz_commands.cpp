// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: MQTT command payloads and topics on untrusted bytes. Built by scripts/fuzz.sh.
#include <cstddef>
#include <cstdint>
#include <string>
#include "cajui_command.h"

using namespace cajui;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Command command{};
    parseCommand(reinterpret_cast<const char*>(data), size, command);
    const std::string topic(reinterpret_cast<const char*>(data), size); // NUL-terminated copy.
    uint64_t device = 0;
    parseCommandTopic(topic.c_str(), "receiver-1", device);
    return 0;
}
