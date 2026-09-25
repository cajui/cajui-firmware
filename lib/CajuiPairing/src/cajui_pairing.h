#pragma once
#include "cajui_application.h"
#include "cajui_storage.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// Radio pairing, docs/radio-pairing.md. Frames reuse the v1 header; `counter` carries
// the node's random attempt nonce.
enum class PairingType : uint8_t { Request = 3, Offer = 4, Confirm = 5, Done = 6 };
constexpr uint16_t PairingProfile = 1;

// Cryptographically secure random bytes (keys, nonces, generations). Unlike Jitter,
// implementations must use a CSPRNG.
class Entropy {
public:
    virtual ~Entropy() = default;
    virtual bool fill(uint8_t* output, size_t size) = 0;
};
struct KeyPair {
    X25519Key privateKey{}, publicKey{};
};
bool newKeyPair(Entropy&, KeyPair&);
bool nonzeroRandom(Entropy&, uint64_t&);
// HKDF-SHA256 over the X25519 secret, bound to identities and both public keys.
bool deriveBindingKey(const X25519Key& privateKey, const X25519Key& peerPublic, uint64_t network,
                      uint64_t receiver, uint64_t node, uint64_t generation,
                      const X25519Key& nodePublic, const X25519Key& receiverPublic, Key& key);

struct Offer {
    uint64_t network = 0, receiver = 0, node = 0, nonce = 0, generation = 0;
    uint16_t profile = 0;
    X25519Key receiverPublic{};
};
bool buildRequest(uint64_t node, uint64_t nonce, const X25519Key& nodePublic, Frame&);
bool parseRequest(const Frame&, uint64_t& node, uint64_t& nonce, X25519Key& nodePublic);
bool buildOffer(const Offer&, const Key&, Frame&);
// Parses without authenticating; the caller derives the key and calls verifyOffer.
bool parseOffer(const Frame&, Offer&);
bool verifyOffer(const Frame&, const Key&);
// JOIN_CONFIRM and JOIN_DONE: an authenticated header without payload.
bool buildTagged(PairingType, uint64_t network, uint64_t node, uint64_t nonce, const Key&, Frame&);
bool verifyTagged(const Frame&, PairingType, uint64_t network, uint64_t node, uint64_t nonce,
                  const Key&);

constexpr size_t MaxCandidates = 4;
constexpr uint32_t PairingWindowMs = 120000;
struct Candidate {
    uint64_t node = 0, nonce = 0;
    X25519Key publicKey{};
    int16_t rssi = 0;
    uint32_t seenAt = 0;
};
enum class HostState { Closed, Open, Offered, Paired };
// Receiver side. Owned by the receiver loop; the setup page opens the window and accepts.
// Nothing is stored until a valid JOIN_CONFIRM: enrollment slots are never freed, so
// abandoned, expired or spoofed attempts must not consume one.
class PairingHost final : public PairingPort {
public:
    PairingHost(PersistentStore&, Entropy&, Clock&);
    PairingHost(const PairingHost&) = delete;
    PairingHost& operator=(const PairingHost&) = delete;
    ~PairingHost() override;
    void open();
    // Drops an unconfirmed offer; it was never stored.
    void close();
    // Closes the window when it expires.
    void poll();
    bool handle(const Frame& frame, int16_t rssi, Frame& reply) override;
    // Offers a binding to a listed node on its next request. Full when no slot is left.
    Result accept(uint64_t node);
    HostState state() const { return state_; }
    const Candidate* candidates() const { return candidates_; }
    size_t candidateCount() const { return count_; }
    uint64_t offeredNode() const { return offer_.node; }
    uint64_t pairedNode() const { return paired_; }
    uint32_t remainingMs() const;

private:
    PersistentStore& store_;
    Entropy& entropy_;
    Clock& clock_;
    HostState state_ = HostState::Closed;
    uint32_t openedAt_ = 0;
    Candidate candidates_[MaxCandidates]{};
    size_t count_ = 0;
    Offer offer_{};
    Key key_{};
    Frame offerFrame_{};
    uint64_t paired_ = 0;
    // The last completed exchange, kept apart from the current offer: its node may still
    // repeat JOIN_CONFIRM after a lost JOIN_DONE while the operator adds the next node.
    struct Completed {
        uint64_t network = 0, node = 0, nonce = 0;
        Key key{};
        Frame done{};
    } last_{};
    void track(uint64_t node, uint64_t nonce, const X25519Key&, int16_t rssi);
    void dropOffer();
};

enum class ClientState {
    Idle,
    Sending,
    Listening,
    Waiting,
    Confirming,
    AwaitingDone,
    Paired,
    Failed
};
// Node side: drives the radio through request, offer, confirm and done. Stores the
// binding only after a valid JOIN_DONE.
class PairingClient final {
public:
    static constexpr uint32_t DeadlineMs = PairingWindowMs, ListenMs = 1500,
                              TransmitTimeoutMs = 3000;
    static constexpr uint32_t RetryMinMs = 300, RetryMaxMs = 800;
    static constexpr uint8_t ConfirmAttempts = 5;
    PairingClient(ReceiverRadio&, Clock&, Jitter&, PersistentStore&, Entropy&);
    PairingClient(const PairingClient&) = delete;
    PairingClient& operator=(const PairingClient&) = delete;
    ~PairingClient();
    bool start();
    void poll();
    ClientState state() const { return state_; }
    uint64_t receiver() const { return paired_.receiver; }

private:
    ReceiverRadio& radio_;
    Clock& clock_;
    Jitter& jitter_;
    PersistentStore& store_;
    Entropy& entropy_;
    ClientState state_ = ClientState::Idle, afterSend_ = ClientState::Idle;
    KeyPair keys_{};
    uint64_t nonce_ = 0;
    uint32_t startedAt_ = 0, phaseAt_ = 0, waitMs_ = 0;
    uint8_t confirms_ = 0;
    Offer paired_{};
    Key key_{};
    Frame outgoing_{};
    void transmit(ClientState next);
    void fail();
    void handleOffer(const Frame&);
};
} // namespace cajui
