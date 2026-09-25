#include <unity.h>
#include <cstring>
#include <deque>
#include <string>
#include "assertions.h"
#include "cajui_pairing.h"
#include "storage_support.h"

namespace {
using namespace cajui;
using fixtures::MemoryRecords;

X25519Key hexKey(const char* hex) {
    X25519Key key{};
    for (size_t i = 0; i < key.size(); ++i) {
        unsigned value = 0;
        std::sscanf(hex + 2 * i, "%2x", &value);
        key[i] = uint8_t(value);
    }
    return key;
}
std::string hexOf(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}
class CountingEntropy final : public Entropy {
public:
    uint8_t next = 1;
    bool ok = true;
    bool fill(uint8_t* output, size_t size) override {
        for (size_t i = 0; i < size; ++i) output[i] = next += 37;
        return ok;
    }
};
class ZeroEntropy final : public Entropy {
public:
    bool fill(uint8_t* output, size_t size) override {
        std::memset(output, 0, size);
        return true;
    }
};
class TestClock final : public Clock {
public:
    uint32_t time = 5000;
    uint32_t nowMs() const override { return time; }
};
class MinJitter final : public Jitter {
public:
    bool ok = true;
    bool between(uint32_t minimum, uint32_t, uint32_t& value) override {
        value = minimum;
        return ok;
    }
};
// In-memory radio: records transmissions and delivers queued frames.
class FakeRadio final : public ReceiverRadio {
public:
    std::deque<Frame> sent, inbox;
    TransmitStatus tx = TransmitStatus::Complete;
    bool sendOk = true, receiveError = false;
    int16_t rssi = -42;
    bool listen() override { return true; }
    bool startChannelCheck() override { return false; }
    ChannelStatus channelStatus() override { return ChannelStatus::Error; }
    bool startTransmit(const Frame& frame) override {
        if (!sendOk) return false;
        sent.push_back(frame);
        return true;
    }
    TransmitStatus transmitStatus(uint32_t& completed) override {
        completed = 0;
        return tx;
    }
    ReceiveStatus receive(Frame& frame) override {
        if (receiveError) return ReceiveStatus::Error;
        if (inbox.empty()) return ReceiveStatus::Empty;
        frame = inbox.front();
        inbox.pop_front();
        return ReceiveStatus::Received;
    }
    bool sleep() override { return true; }
    int16_t lastRssi() const override { return rssi; }
};

void test_x25519_and_hkdf_match_rfc_vectors() {
    // RFC 7748 section 6.1.
    const auto alice = hexKey("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob = hexKey("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    X25519Key alicePublic{}, bobPublic{}, shared1{}, shared2{};
    TEST_ASSERT_TRUE(x25519Public(alice, alicePublic));
    TEST_ASSERT_TRUE(x25519Public(bob, bobPublic));
    TEST_ASSERT_EQUAL_STRING("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
                             hexOf(alicePublic.data(), 32).c_str());
    TEST_ASSERT_EQUAL_STRING("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
                             hexOf(bobPublic.data(), 32).c_str());
    TEST_ASSERT_TRUE(x25519Shared(alice, bobPublic, shared1));
    TEST_ASSERT_TRUE(x25519Shared(bob, alicePublic, shared2));
    TEST_ASSERT_EQUAL_STRING("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
                             hexOf(shared1.data(), 32).c_str());
    TEST_ASSERT_EQUAL_MEMORY(shared1.data(), shared2.data(), 32);
    // A low-order peer key yields an all-zero secret, which must be rejected.
    TEST_ASSERT_FALSE(x25519Shared(alice, X25519Key{}, shared1));
    // RFC 5869 test case 1.
    uint8_t ikm[22], salt[13], info[10], okm[42];
    std::memset(ikm, 0x0b, sizeof(ikm));
    for (size_t i = 0; i < sizeof(salt); ++i) salt[i] = uint8_t(i);
    for (size_t i = 0; i < sizeof(info); ++i) info[i] = uint8_t(0xf0 + i);
    TEST_ASSERT_TRUE(
        hkdfSha256(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info), okm, sizeof(okm)));
    TEST_ASSERT_EQUAL_STRING("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf3400"
                             "7208d5b887185865",
                             hexOf(okm, sizeof(okm)).c_str());
    TEST_ASSERT_FALSE(hkdfSha256(nullptr, 1, salt, 1, info, 1, okm, 16));
    TEST_ASSERT_FALSE(hkdfSha256(ikm, 1, salt, 1, info, 1, okm, 0));
    TEST_ASSERT_FALSE(hkdfSha256(ikm, 1, salt, 1, info, 1, okm, 255 * 32 + 1));
}
void test_pairing_frames_round_trip_and_reject_tampering() {
    CountingEntropy entropy;
    KeyPair node{}, receiver{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, node));
    TEST_ASSERT_TRUE(newKeyPair(entropy, receiver));
    Frame request{};
    TEST_ASSERT_TRUE(buildRequest(2, 77, node.publicKey, request));
    TEST_ASSERT_EQUAL_size_t(64, request.size);
    TEST_ASSERT_EQUAL_UINT8(3, untrustedType(request));
    uint64_t id = 0, nonce = 0;
    X25519Key parsed{};
    TEST_ASSERT_TRUE(parseRequest(request, id, nonce, parsed));
    TEST_ASSERT_EQUAL_UINT64(2, id);
    TEST_ASSERT_EQUAL_UINT64(77, nonce);
    TEST_ASSERT_FALSE(buildRequest(0, 77, node.publicKey, request));
    TEST_ASSERT_FALSE(buildRequest(2, 0, node.publicKey, request));
    TEST_ASSERT_FALSE(buildRequest(2, 77, X25519Key{}, request));
    TEST_ASSERT_TRUE(buildRequest(2, 77, node.publicKey, request));
    const size_t badAt[] = {0, 4, 5, 6, 14, 22, 30};
    for (size_t i = 0; i < 7; ++i) {
        SCENARIO(i);
        auto bad = request;
        bad.bytes[badAt[i]] ^= (badAt[i] == 14 || badAt[i] == 22) ? 0 : 1;
        if (badAt[i] == 14) std::memset(bad.bytes.data() + 14, 0, 8); // Node zero.
        if (badAt[i] == 22) std::memset(bad.bytes.data() + 22, 0, 8); // Nonce zero.
        TEST_ASSERT_FALSE(parseRequest(bad, id, nonce, parsed));
    }
    auto zeroKey = request;
    std::memset(zeroKey.bytes.data() + 32, 0, 32);
    TEST_ASSERT_FALSE(parseRequest(zeroKey, id, nonce, parsed));

    Key key{};
    TEST_ASSERT_TRUE(deriveBindingKey(receiver.privateKey, node.publicKey, 42, 1, 2, 10,
                                      node.publicKey, receiver.publicKey, key));
    Key peer{};
    TEST_ASSERT_TRUE(deriveBindingKey(node.privateKey, receiver.publicKey, 42, 1, 2, 10,
                                      node.publicKey, receiver.publicKey, peer));
    TEST_ASSERT_EQUAL_MEMORY(key.data(), peer.data(), key.size());
    Key other{};
    TEST_ASSERT_TRUE(deriveBindingKey(node.privateKey, receiver.publicKey, 42, 1, 2, 11,
                                      node.publicKey, receiver.publicKey, other));
    TEST_ASSERT_TRUE(std::memcmp(key.data(), other.data(), key.size()) !=
                     0); // Bound to generation.
    TEST_ASSERT_FALSE(deriveBindingKey(node.privateKey, receiver.publicKey, 0, 1, 2, 10,
                                       node.publicKey, receiver.publicKey, other));
    TEST_ASSERT_FALSE(deriveBindingKey(node.privateKey, X25519Key{}, 42, 1, 2, 10, node.publicKey,
                                       receiver.publicKey, other));

    Offer offer{};
    offer.network = 42;
    offer.receiver = 1;
    offer.node = 2;
    offer.nonce = 77;
    offer.generation = 10;
    offer.profile = 1;
    offer.receiverPublic = receiver.publicKey;
    Frame frame{};
    TEST_ASSERT_TRUE(buildOffer(offer, key, frame));
    TEST_ASSERT_EQUAL_size_t(98, frame.size);
    Offer read{};
    TEST_ASSERT_TRUE(parseOffer(frame, read));
    TEST_ASSERT_EQUAL_UINT64(10, read.generation);
    TEST_ASSERT_TRUE(verifyOffer(frame, key));
    for (size_t i = 0; i < frame.size; ++i) {
        SCENARIO(i);
        auto bad = frame;
        bad.bytes[i] ^= 0x01;
        TEST_ASSERT_FALSE(verifyOffer(bad, key)); // Every byte is authenticated or checked.
    }
    TEST_ASSERT_FALSE(verifyOffer(frame, other));
    auto noGeneration = offer;
    noGeneration.generation = 0;
    TEST_ASSERT_FALSE(buildOffer(noGeneration, key, frame));

    Frame confirm{};
    TEST_ASSERT_TRUE(buildTagged(PairingType::Confirm, 42, 2, 77, key, confirm));
    TEST_ASSERT_EQUAL_size_t(48, confirm.size);
    TEST_ASSERT_TRUE(verifyTagged(confirm, PairingType::Confirm, 42, 2, 77, key));
    TEST_ASSERT_FALSE(verifyTagged(confirm, PairingType::Done, 42, 2, 77, key));
    TEST_ASSERT_FALSE(verifyTagged(confirm, PairingType::Confirm, 43, 2, 77, key));
    TEST_ASSERT_FALSE(verifyTagged(confirm, PairingType::Confirm, 42, 2, 78, key));
    TEST_ASSERT_FALSE(verifyTagged(confirm, PairingType::Confirm, 42, 2, 77, other));
    TEST_ASSERT_FALSE(verifyTagged(confirm, PairingType::Request, 42, 2, 77, key));
    TEST_ASSERT_FALSE(buildTagged(PairingType::Offer, 42, 2, 77, key, confirm));
    TEST_ASSERT_FALSE(buildTagged(PairingType::Done, 0, 2, 77, key, confirm));
    TEST_ASSERT_EQUAL_UINT8(0, untrustedType(Frame{}));
    ZeroEntropy zero;
    uint64_t value = 1;
    TEST_ASSERT_FALSE(nonzeroRandom(zero, value));
    entropy.ok = false;
    TEST_ASSERT_FALSE(newKeyPair(entropy, node));
    TEST_ASSERT_FALSE(nonzeroRandom(entropy, value));
}

// Receiver (device 1) and node (device 2) with in-memory storage and radios.
struct PairRig {
    MemoryRecords rxBlob, txBlob;
    std::unique_ptr<PersistentStore> rx = fixtures::mounted(rxBlob, Role::Receiver);
    std::unique_ptr<PersistentStore> tx = fixtures::mounted(txBlob, Role::Transmitter);
    TestClock clock;
    MinJitter jitter;
    CountingEntropy rxEntropy, txEntropy;
    FakeRadio rxRadio, txRadio;
    PairingHost host{*rx, rxEntropy, clock};
    ReceiverController controller{rxRadio, clock, *rx};
    PairingClient client{txRadio, clock, jitter, *tx, txEntropy};
    PairRig() {
        txEntropy.next = 101;
        controller.setPairing(&host);
        TEST_ASSERT_TRUE(controller.start());
    }
    // Node -> receiver: deliver every node transmission and let the controller answer.
    void toReceiver() {
        while (!txRadio.sent.empty()) {
            rxRadio.inbox.push_back(txRadio.sent.front());
            txRadio.sent.pop_front();
            controller.poll();
            controller.poll(); // Finish the reply transmission.
        }
    }
    void toNode() {
        while (!rxRadio.sent.empty()) {
            txRadio.inbox.push_back(rxRadio.sent.front());
            rxRadio.sent.pop_front();
        }
    }
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            client.poll();
            toReceiver();
            toNode();
            client.poll();
            clock.time += 100;
        }
    }
};
void test_radio_pairing_creates_matching_active_bindings() {
    PairRig rig;
    TEST_ASSERT_TRUE(rig.client.start());
    rig.host.open();
    rig.run(1);
    TEST_ASSERT_EQUAL_size_t(1, rig.host.candidateCount());
    TEST_ASSERT_EQUAL_UINT64(2, rig.host.candidates()[0].node);
    TEST_ASSERT_EQUAL_INT16(-42, rig.host.candidates()[0].rssi);
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    EXPECT_RESULT(HostState::Offered, rig.host.state());
    rig.run(40);
    EXPECT_RESULT(ClientState::Paired, rig.client.state());
    EXPECT_RESULT(HostState::Paired, rig.host.state());
    TEST_ASSERT_EQUAL_UINT64(2, rig.host.pairedNode());
    TEST_ASSERT_EQUAL_UINT64(1, rig.client.receiver());
    Binding atNode{}, atReceiver{};
    TEST_ASSERT_TRUE(rig.tx->binding(2, atNode));
    TEST_ASSERT_TRUE(rig.rx->binding(2, atReceiver));
    TEST_ASSERT_EQUAL_MEMORY(atNode.key.data(), atReceiver.key.data(), KeySize);
    TEST_ASSERT_EQUAL_UINT64(atReceiver.network, atNode.network);
    TEST_ASSERT_NOT_EQUAL(0, rig.rx->network()); // A fresh receiver chose a network.
    // The derived key carries real traffic: node DATA is accepted by the receiver.
    Sender sender;
    EXPECT_RESULT(Result::Ok, sender.begin(atNode, fixtures::sample(), *rig.tx));
    Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(atReceiver, *sender.nextAttempt(), *rig.rx, ack));
    EXPECT_RESULT(Result::Ok, sender.acknowledge(ack));
    // Bindings survive a remount.
    auto reopened = fixtures::mounted(rig.txBlob, Role::Transmitter);
    TEST_ASSERT_TRUE(reopened->binding(2, atNode));
}
void test_pairing_again_rotates_the_generation_on_both_sides() {
    PairRig rig;
    TEST_ASSERT_TRUE(fixtures::enroll(*rig.rx));
    TEST_ASSERT_TRUE(fixtures::enroll(*rig.tx));
    TEST_ASSERT_TRUE(rig.client.start());
    rig.host.open();
    rig.run(1);
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    rig.run(40);
    EXPECT_RESULT(ClientState::Paired, rig.client.state());
    EnrollmentInfo old{};
    TEST_ASSERT_TRUE(rig.rx->info(2, 10, old));
    EXPECT_RESULT(Enrollment::Revoked, old.state);
    TEST_ASSERT_TRUE(rig.tx->info(2, 10, old));
    EXPECT_RESULT(Enrollment::Revoked, old.state);
    TEST_ASSERT_EQUAL_UINT64(42, rig.rx->network()); // Same network kept.
}
// Delivers one DATA frame from the node's stored binding through the receiver controller.
Result sendSample(PairRig& rig, uint64_t counter) {
    Binding atNode{};
    TEST_ASSERT_TRUE(rig.tx->binding(2, atNode));
    Message m{};
    m.counter = counter;
    m.data = fixtures::sample();
    Frame frame{};
    EXPECT_RESULT(Result::Ok, seal(atNode, m, frame));
    rig.rxRadio.inbox.push_back(frame);
    rig.controller.poll();
    rig.controller.poll();
    rig.rxRadio.sent.clear();
    return rig.controller.lastResult();
}
void test_lost_done_on_repairing_never_cuts_the_node_off() {
    for (int doneArrives = 0; doneArrives < 2; ++doneArrives) {
        SCENARIO(doneArrives);
        PairRig rig;
        TEST_ASSERT_TRUE(fixtures::enroll(*rig.rx));
        TEST_ASSERT_TRUE(fixtures::enroll(*rig.tx));
        EXPECT_RESULT(Result::Ok, sendSample(rig, 1)); // Generation 10 is in use.
        TEST_ASSERT_TRUE(rig.client.start());
        rig.host.open();
        rig.run(1);
        EXPECT_RESULT(Result::Ok, rig.host.accept(2));
        if (doneArrives) {
            rig.run(40);
            EXPECT_RESULT(ClientState::Paired, rig.client.state());
        } else {
            for (int i = 0; i < 200 && rig.client.state() != ClientState::Failed; ++i) {
                rig.client.poll();
                rig.toReceiver();
                for (auto it = rig.rxRadio.sent.begin(); it != rig.rxRadio.sent.end();)
                    it = untrustedType(*it) == uint8_t(PairingType::Done) // Every DONE is lost.
                             ? rig.rxRadio.sent.erase(it)
                             : it + 1;
                rig.toNode();
                rig.clock.time += 100;
            }
            EXPECT_RESULT(ClientState::Failed, rig.client.state());
        }
        EXPECT_RESULT(HostState::Paired, rig.host.state());
        EnrollmentInfo old{};
        TEST_ASSERT_TRUE(rig.rx->info(2, 10, old));
        EXPECT_RESULT(Enrollment::Active, old.state); // Still valid at the receiver.
        // The node's next sample, under whichever key it holds, is accepted and settles it.
        EXPECT_RESULT(Result::Ok, sendSample(rig, doneArrives ? 1 : 2));
        Binding bindings[2]{};
        TEST_ASSERT_EQUAL_size_t(1, rig.rx->bindings(2, bindings, 2));
        Binding atNode{};
        TEST_ASSERT_TRUE(rig.tx->binding(2, atNode));
        TEST_ASSERT_TRUE(bindings[0].key == atNode.key);
    }
}
void test_host_ignores_input_outside_the_window_and_limits_candidates() {
    PairRig rig;
    CountingEntropy entropy;
    KeyPair keys{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, keys));
    Frame request{}, reply{};
    TEST_ASSERT_TRUE(buildRequest(7, 1, keys.publicKey, request));
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply)); // Closed.
    EXPECT_RESULT(Result::Invalid, rig.host.accept(7));
    TEST_ASSERT_EQUAL_UINT32(0, rig.host.remainingMs());
    rig.host.open();
    TEST_ASSERT_EQUAL_UINT32(PairingWindowMs, rig.host.remainingMs());
    TEST_ASSERT_TRUE(buildRequest(1, 1, keys.publicKey, request)); // The receiver's own ID.
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    TEST_ASSERT_EQUAL_size_t(0, rig.host.candidateCount());
    for (uint64_t node = 10; node < 15; ++node) {
        rig.clock.time += 10;
        TEST_ASSERT_TRUE(buildRequest(node, node, keys.publicKey, request));
        TEST_ASSERT_FALSE(rig.host.handle(request, int16_t(-40 - node), reply));
    }
    TEST_ASSERT_EQUAL_size_t(MaxCandidates, rig.host.candidateCount());
    // A full list ignores later requesters instead of evicting a listed node, which would
    // let an attacker flush the victim and re-list its ID with another key.
    for (size_t i = 0; i < rig.host.candidateCount(); ++i)
        TEST_ASSERT_EQUAL_UINT64(10 + i, rig.host.candidates()[i].node);
    EXPECT_RESULT(Result::NotFound, rig.host.accept(14));
    EXPECT_RESULT(Result::NotFound, rig.host.accept(99));
    TEST_ASSERT_FALSE(rig.host.handle(Frame{}, -50, reply));
    Frame forged{};
    TEST_ASSERT_TRUE(buildTagged(PairingType::Confirm, 42, 11, 11, Key{}, forged));
    TEST_ASSERT_FALSE(rig.host.handle(forged, -50, reply)); // No offer pending.
    rig.clock.time += PairingWindowMs;
    rig.host.poll();
    EXPECT_RESULT(HostState::Closed, rig.host.state());
    TEST_ASSERT_EQUAL_size_t(0, rig.host.candidateCount());
}
void test_offers_store_nothing_and_resist_spoofed_requests() {
    PairRig rig;
    CountingEntropy entropy;
    KeyPair keys{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, keys));
    Frame request{}, first{}, second{};
    rig.host.open();
    TEST_ASSERT_TRUE(buildRequest(2, 5, keys.publicKey, request));
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, first));
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    TEST_ASSERT_EQUAL_size_t(BindingCapacity, rig.rx->freeSlots()); // Nothing stored yet.
    TEST_ASSERT_TRUE(rig.host.handle(request, -50, first));
    TEST_ASSERT_TRUE(rig.host.handle(request, -50, second));
    TEST_ASSERT_TRUE(sameFrame(first, second));
    // Repeated adds never consume an enrollment slot.
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    // Another device claiming the offered node's ID withdraws the offer: neither gets it.
    Frame spoofed{};
    TEST_ASSERT_TRUE(buildRequest(2, 6, keys.publicKey, spoofed));
    TEST_ASSERT_FALSE(rig.host.handle(spoofed, -50, second));
    EXPECT_RESULT(HostState::Open, rig.host.state());
    TEST_ASSERT_TRUE(rig.host.candidates()[0].conflict);
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, second));
    EXPECT_RESULT(Result::Conflict, rig.host.accept(2));
    // Stopping, searching again and window expiry never consume a slot either.
    rig.host.close();
    rig.host.open();
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, first));
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    rig.clock.time += PairingWindowMs;
    rig.host.poll();
    EXPECT_RESULT(HostState::Closed, rig.host.state());
    TEST_ASSERT_EQUAL_size_t(BindingCapacity, rig.rx->freeSlots());
}
void test_injected_request_cannot_redirect_a_listed_node() {
    PairRig rig;
    CountingEntropy entropy;
    KeyPair victim{}, attacker{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, victim));
    TEST_ASSERT_TRUE(newKeyPair(entropy, attacker));
    Frame request{}, injected{}, reply{};
    rig.host.open();
    TEST_ASSERT_TRUE(buildRequest(2, 5, victim.publicKey, request));
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    // The attacker repeats the victim's public node ID with its own key and nonce before
    // the operator clicks Add. The listed key stays the victim's and Add is refused.
    TEST_ASSERT_TRUE(buildRequest(2, 9, attacker.publicKey, injected));
    TEST_ASSERT_FALSE(rig.host.handle(injected, -80, reply));
    TEST_ASSERT_EQUAL_size_t(1, rig.host.candidateCount());
    TEST_ASSERT_TRUE(rig.host.candidates()[0].conflict);
    TEST_ASSERT_TRUE(rig.host.candidates()[0].publicKey == victim.publicKey);
    TEST_ASSERT_EQUAL_UINT64(5, rig.host.candidates()[0].nonce);
    EXPECT_RESULT(Result::Conflict, rig.host.accept(2));
    EXPECT_RESULT(HostState::Open, rig.host.state());
    // The same key with a new nonce (a restarted attempt) is also a conflict.
    rig.host.open();
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    TEST_ASSERT_TRUE(buildRequest(2, 6, victim.publicKey, injected));
    TEST_ASSERT_FALSE(rig.host.handle(injected, -50, reply));
    EXPECT_RESULT(Result::Conflict, rig.host.accept(2));
    // A new window starts clean.
    rig.host.open();
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
}
void test_previous_node_still_gets_done_after_another_add() {
    PairRig rig;
    TEST_ASSERT_TRUE(rig.client.start());
    rig.host.open();
    rig.run(1);
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    for (int i = 0; i < 30 && rig.rxRadio.sent.empty(); ++i) {
        rig.client.poll();
        rig.toReceiver();
        rig.clock.time += 100;
    }
    rig.toNode();
    rig.client.poll();
    rig.client.poll(); // Confirm transmitted.
    rig.toReceiver();  // Receiver stores and answers JOIN_DONE...
    Frame done = rig.rxRadio.sent.front();
    rig.rxRadio.sent.clear(); // ...which is lost.
    EXPECT_RESULT(HostState::Paired, rig.host.state());
    // The operator adds another node before the first one confirms again.
    CountingEntropy entropy;
    KeyPair other{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, other));
    Frame request{}, reply{};
    TEST_ASSERT_TRUE(buildRequest(9, 3, other.publicKey, request));
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    EXPECT_RESULT(Result::Ok, rig.host.accept(9));
    for (int i = 0; i < 4 && rig.txRadio.sent.empty(); ++i) { // Wait for the resend.
        rig.clock.time += PairingClient::ListenMs;
        rig.client.poll();
        rig.client.poll();
    }
    rig.toReceiver();
    TEST_ASSERT_EQUAL_size_t(1, rig.rxRadio.sent.size());
    TEST_ASSERT_TRUE(sameFrame(done, rig.rxRadio.sent.front()));
    rig.toNode();
    rig.client.poll();
    EXPECT_RESULT(ClientState::Paired, rig.client.state());
    EXPECT_RESULT(HostState::Offered, rig.host.state()); // Node 9's offer is intact.
    Binding binding{};
    TEST_ASSERT_TRUE(rig.tx->binding(2, binding));
}
void test_repeated_confirmations_get_done_only_within_bounds() {
    for (int scenario = 0; scenario < 2; ++scenario) {
        SCENARIO(scenario);
        PairRig rig;
        TEST_ASSERT_TRUE(rig.client.start());
        rig.host.open();
        rig.run(1);
        EXPECT_RESULT(Result::Ok, rig.host.accept(2));
        for (int i = 0; i < 30 && rig.rxRadio.sent.empty(); ++i) {
            rig.client.poll();
            rig.toReceiver();
            rig.clock.time += 100;
        }
        rig.toNode();
        rig.client.poll();
        rig.client.poll(); // Confirm transmitted.
        Frame confirm = rig.txRadio.sent.front();
        rig.toReceiver();
        TEST_ASSERT_EQUAL_size_t(1, rig.rxRadio.sent.size());
        rig.rxRadio.sent.clear();
        Frame reply{};
        if (scenario == 0) { // A replayed confirmation: at most MaxDoneReplies in total.
            for (int i = 1; i < MaxDoneReplies; ++i)
                TEST_ASSERT_TRUE(rig.host.handle(confirm, -40, reply));
            TEST_ASSERT_FALSE(rig.host.handle(confirm, -40, reply));
        } else { // Much later, even within the reply count.
            rig.clock.time += DoneReplayMs;
            TEST_ASSERT_FALSE(rig.host.handle(confirm, -40, reply));
        }
    }
}
void test_full_storage_is_refused_before_any_exchange() {
    PairRig rig;
    for (uint64_t node = 10; node < 10 + BindingCapacity; ++node)
        TEST_ASSERT_TRUE(rig.rx->prepare(42, 1, node, node, fixtures::key(uint8_t(node)), 1) ==
                         Result::Ok);
    TEST_ASSERT_EQUAL_size_t(0, rig.rx->freeSlots());
    CountingEntropy entropy;
    KeyPair keys{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, keys));
    Frame request{}, reply{};
    rig.host.open();
    TEST_ASSERT_TRUE(buildRequest(2, 5, keys.publicKey, request));
    TEST_ASSERT_FALSE(rig.host.handle(request, -50, reply));
    EXPECT_RESULT(Result::Full, rig.host.accept(2));
    MemoryRecords full;
    auto node = fixtures::mounted(full, Role::Transmitter);
    for (uint64_t generation = 1; generation <= BindingCapacity; ++generation) {
        TEST_ASSERT_TRUE(node->prepare(42, 1, 2, generation, fixtures::key(uint8_t(generation)),
                                       1) == Result::Ok);
        TEST_ASSERT_TRUE(node->activate(2, generation) == Result::Ok);
    }
    // Fifteen revoked generations are reclaimed: a transmitter can always pair again.
    TEST_ASSERT_EQUAL_size_t(BindingCapacity - 1, node->freeSlots());
    PairingClient client(rig.txRadio, rig.clock, rig.jitter, *node, entropy);
    TEST_ASSERT_TRUE(client.start());
}
void test_node_ignores_forged_offers_and_resends_confirm_until_done() {
    PairRig lossy;
    TEST_ASSERT_TRUE(lossy.client.start());
    lossy.host.open();
    lossy.run(1);
    EXPECT_RESULT(Result::Ok, lossy.host.accept(2));
    // Drive until the receiver emits its offer, then tamper with it in flight.
    for (int i = 0; i < 30 && lossy.rxRadio.sent.empty(); ++i) {
        lossy.client.poll();
        lossy.toReceiver();
        lossy.clock.time += 100;
    }
    TEST_ASSERT_EQUAL_size_t(1, lossy.rxRadio.sent.size());
    Frame genuine = lossy.rxRadio.sent.front();
    lossy.rxRadio.sent.clear();
    Frame forged = genuine;
    forged.bytes[32 + 32 + 8] ^= 1; // Generation byte.
    lossy.client.poll();            // Finish the last request transmission; now listening.
    EXPECT_RESULT(ClientState::Listening, lossy.client.state());
    lossy.txRadio.inbox.push_back(forged);
    lossy.client.poll();
    TEST_ASSERT_TRUE(lossy.txRadio.inbox.empty());
    EXPECT_RESULT(ClientState::Listening, lossy.client.state()); // Consumed and ignored.
    lossy.txRadio.inbox.push_back(genuine);
    lossy.client.poll();
    EXPECT_RESULT(ClientState::Sending, lossy.client.state()); // Confirming.
    lossy.client.poll();
    // Drop the receiver's first JOIN_DONE: the node confirms again and gets it again.
    lossy.toReceiver();
    TEST_ASSERT_EQUAL_size_t(1, lossy.rxRadio.sent.size());
    Frame done = lossy.rxRadio.sent.front();
    lossy.rxRadio.sent.clear();
    lossy.clock.time += PairingClient::ListenMs;
    lossy.client.poll(); // Timeout: resend confirm.
    lossy.client.poll();
    lossy.toReceiver();
    TEST_ASSERT_EQUAL_size_t(1, lossy.rxRadio.sent.size());
    TEST_ASSERT_TRUE(sameFrame(done, lossy.rxRadio.sent.front()));
    lossy.toNode();
    lossy.client.poll();
    EXPECT_RESULT(ClientState::Paired, lossy.client.state());
}
void test_node_gives_up_without_storing_anything() {
    PairRig rig;
    TEST_ASSERT_TRUE(rig.client.start());
    rig.host.open();
    rig.run(1);
    EXPECT_RESULT(Result::Ok, rig.host.accept(2));
    for (int i = 0; i < 30 && rig.rxRadio.sent.empty(); ++i) {
        rig.client.poll();
        rig.toReceiver();
        rig.clock.time += 100;
    }
    rig.toNode();
    rig.client.poll(); // Offer accepted, confirm sent; the receiver never answers now.
    rig.txRadio.sent.clear();
    for (int i = 0; i < 20; ++i) {
        rig.clock.time += PairingClient::ListenMs;
        rig.client.poll();
        rig.client.poll();
        rig.txRadio.sent.clear();
    }
    EXPECT_RESULT(ClientState::Failed, rig.client.state());
    TEST_ASSERT_EQUAL_size_t(BindingCapacity, rig.tx->freeSlots());
    TEST_ASSERT_EQUAL_size_t(BindingCapacity, rig.rx->freeSlots()); // Confirm never arrived.
}
void test_node_failures_deadline_radio_and_foreign_network() {
    {
        PairRig rig;
        TEST_ASSERT_TRUE(rig.client.start());
        rig.clock.time += PairingClient::DeadlineMs;
        rig.client.poll();
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
        TEST_ASSERT_FALSE(rig.client.start()); // Single use.
    }
    {
        PairRig rig;
        rig.txRadio.sendOk = false;
        TEST_ASSERT_FALSE(rig.client.start());
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
    }
    {
        PairRig rig;
        rig.txRadio.tx = TransmitStatus::Pending;
        TEST_ASSERT_TRUE(rig.client.start());
        rig.clock.time += PairingClient::TransmitTimeoutMs;
        rig.client.poll();
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
    }
    {
        PairRig rig;
        rig.txRadio.tx = TransmitStatus::Error;
        TEST_ASSERT_TRUE(rig.client.start());
        rig.client.poll();
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
    }
    {
        PairRig rig;
        TEST_ASSERT_TRUE(rig.client.start());
        rig.client.poll();
        rig.txRadio.receiveError = true;
        rig.client.poll();
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
    }
    {
        PairRig rig;
        rig.jitter.ok = false; // Falls back to the maximum retry delay.
        TEST_ASSERT_TRUE(rig.client.start());
        rig.client.poll();
        rig.clock.time += PairingClient::ListenMs;
        rig.client.poll();
        EXPECT_RESULT(ClientState::Waiting, rig.client.state());
        rig.clock.time += PairingClient::RetryMaxMs;
        rig.client.poll();
        EXPECT_RESULT(ClientState::Sending, rig.client.state());
    }
    {
        // A node already in network 42 cannot join a receiver of another network.
        PairRig rig;
        TEST_ASSERT_TRUE(rig.tx->prepare(42, 1, 2, 10, fixtures::key(), 1) == Result::Ok);
        TEST_ASSERT_TRUE(rig.rx->prepare(99, 1, 3, 20, fixtures::key(5), 1) == Result::Ok);
        TEST_ASSERT_TRUE(rig.client.start());
        rig.host.open();
        rig.run(1);
        EXPECT_RESULT(Result::Ok, rig.host.accept(2));
        rig.run(40);
        EXPECT_RESULT(ClientState::Failed, rig.client.state());
        Binding binding{};
        TEST_ASSERT_FALSE(rig.rx->binding(2, binding)); // Refused before confirming.
    }
    {
        MemoryRecords blob;
        auto store = fixtures::mounted(blob, Role::Receiver); // Wrong role for a node.
        TestClock clock;
        MinJitter jitter;
        CountingEntropy entropy;
        FakeRadio radio;
        PairingClient client(radio, clock, jitter, *store, entropy);
        TEST_ASSERT_FALSE(client.start());
    }
}
void test_receiver_without_pairing_handler_drops_pairing_frames() {
    MemoryRecords blob;
    auto store = fixtures::mounted(blob, Role::Receiver);
    TestClock clock;
    FakeRadio radio;
    ReceiverController controller(radio, clock, *store);
    TEST_ASSERT_TRUE(controller.start());
    CountingEntropy entropy;
    KeyPair keys{};
    TEST_ASSERT_TRUE(newKeyPair(entropy, keys));
    Frame request{};
    TEST_ASSERT_TRUE(buildRequest(2, 1, keys.publicKey, request));
    radio.inbox.push_back(request);
    controller.poll();
    TEST_ASSERT_TRUE(radio.sent.empty());
    EXPECT_RESULT(ReceiverState::Listening, controller.state());
    // With a handler, a failed reply transmission is terminal like any ACK failure.
    PairingHost host(*store, entropy, clock);
    controller.setPairing(&host);
    host.open();
    radio.inbox.push_back(request);
    controller.poll();
    EXPECT_RESULT(Result::Ok, host.accept(2));
    radio.sendOk = false;
    radio.inbox.push_back(request);
    controller.poll();
    EXPECT_RESULT(ReceiverState::Failed, controller.state());
}
} // namespace

void runPairingTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_x25519_and_hkdf_match_rfc_vectors);
    RUN_TEST(test_pairing_frames_round_trip_and_reject_tampering);
    RUN_TEST(test_radio_pairing_creates_matching_active_bindings);
    RUN_TEST(test_pairing_again_rotates_the_generation_on_both_sides);
    RUN_TEST(test_lost_done_on_repairing_never_cuts_the_node_off);
    RUN_TEST(test_host_ignores_input_outside_the_window_and_limits_candidates);
    RUN_TEST(test_offers_store_nothing_and_resist_spoofed_requests);
    RUN_TEST(test_injected_request_cannot_redirect_a_listed_node);
    RUN_TEST(test_previous_node_still_gets_done_after_another_add);
    RUN_TEST(test_repeated_confirmations_get_done_only_within_bounds);
    RUN_TEST(test_full_storage_is_refused_before_any_exchange);
    RUN_TEST(test_node_ignores_forged_offers_and_resends_confirm_until_done);
    RUN_TEST(test_node_gives_up_without_storing_anything);
    RUN_TEST(test_node_failures_deadline_radio_and_foreign_network);
    RUN_TEST(test_receiver_without_pairing_handler_drops_pairing_frames);
}
