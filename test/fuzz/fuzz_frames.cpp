// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: every radio frame parser on untrusted bytes. Built by scripts/fuzz.sh.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "cajui_pairing.h"
#include "cajui_protocol.h"

using namespace cajui;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Frame frame{};
    // Oversized input is truncated to what a radio adapter can deliver.
    frame.size = size < MaxFrame ? size : MaxFrame;
    std::memcpy(frame.bytes.data(), data, frame.size);
    untrustedType(frame);
    untrustedDataNode(frame);
    Binding binding{};
    binding.network = 42;
    binding.node = 1234;
    binding.active = true;
    for (size_t i = 0; i < binding.key.size(); ++i) binding.key[i] = uint8_t(i + 1);
    Message message{};
    open(binding, frame, message);
    uint64_t node = 0, nonce = 0;
    X25519Key publicKey{};
    parseRequest(frame, node, nonce, publicKey);
    Offer offer{};
    if (parseOffer(frame, offer)) verifyOffer(frame, binding.key);
    verifyTagged(frame, PairingType::Confirm, 42, 1234, 7, binding.key);
    verifyTagged(frame, PairingType::Done, 42, 1234, 7, binding.key);
    return 0;
}
