#include "cajui_pairing.h"
#include <cstring>

namespace cajui {
namespace {
// v1 header layout, see docs/protocol-v1.md.
constexpr uint8_t Magic[4] = {'C', 'J', 'L', 'R'};
constexpr uint8_t Version = 1;
constexpr size_t TypeAt = 5, NetworkAt = 6, NodeAt = 14, CounterAt = 22, LengthAt = 30;
constexpr size_t RequestPayload = X25519Size;
constexpr size_t OfferPayload = X25519Size + 8 + 8 + 2;
constexpr size_t RequestSize = HeaderSize + RequestPayload;
constexpr size_t OfferSize = HeaderSize + OfferPayload + TagSize;
constexpr size_t TaggedSize = HeaderSize + TagSize;
constexpr char Salt[] = "cajui-pair-v1";
constexpr int RandomAttempts = 8;

void put(uint8_t* out, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) out[size - 1 - i] = uint8_t(value >> (i * 8));
}
uint64_t get(const uint8_t* in, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value = (value << 8) | in[i];
    return value;
}
void header(PairingType type, uint64_t network, uint64_t node, uint64_t nonce, size_t length,
            Frame& frame) {
    frame = Frame{};
    auto* h = frame.bytes.data();
    std::memcpy(h, Magic, sizeof(Magic));
    h[4] = Version;
    h[TypeAt] = uint8_t(type);
    put(h + NetworkAt, network, 8);
    put(h + NodeAt, node, 8);
    put(h + CounterAt, nonce, 8);
    put(h + LengthAt, length, 2);
}
bool shaped(const Frame& frame, PairingType type, size_t size, size_t length) {
    const auto* h = frame.bytes.data();
    return frame.size == size && std::memcmp(h, Magic, sizeof(Magic)) == 0 && h[4] == Version &&
           h[TypeAt] == uint8_t(type) && get(h + LengthAt, 2) == length;
}
void nonceFor(PairingType type, uint64_t nonce, uint8_t out[NonceSize]) {
    out[0] = 'C';
    out[1] = 'J';
    out[2] = uint8_t(type);
    out[3] = Version;
    put(out + 4, nonce, 8);
}
bool nonzero(const X25519Key& key) {
    uint8_t any = 0;
    for (auto byte : key) any |= byte;
    return any != 0;
}
// GCM with an empty plaintext: the tag authenticates `authenticated` bytes as AAD.
bool tag(const Key& key, PairingType type, uint64_t nonce, const uint8_t* authenticated,
         size_t size, uint8_t* output) {
    uint8_t iv[NonceSize];
    nonceFor(type, nonce, iv);
    uint8_t empty = 0;
    uint8_t sink = 0;
    return encrypt(key, iv, authenticated, size, &empty, 0, &sink, output);
}
bool check(const Key& key, PairingType type, uint64_t nonce, const uint8_t* authenticated,
           size_t size, const uint8_t* received) {
    uint8_t iv[NonceSize];
    nonceFor(type, nonce, iv);
    uint8_t empty = 0;
    uint8_t sink = 0;
    return decrypt(key, iv, authenticated, size, &empty, 0, received, &sink);
}
void wipe(void* data, size_t size) {
    volatile auto* bytes = static_cast<volatile uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}
// A prepared generation left by an interrupted attempt blocks a new one for that node.
Result prepareFresh(PersistentStore& store, uint64_t network, uint64_t receiver, uint64_t node,
                    uint64_t generation, const Key& key) {
    Result result = store.prepare(network, receiver, node, generation, key, PairingProfile);
    if (result != Result::Conflict) return result;
    EnrollmentInfo entries[BindingCapacity]{};
    const size_t count = store.list(entries, BindingCapacity);
    bool revoked = false;
    for (size_t i = 0; i < count; ++i)
        if (entries[i].node == node && entries[i].state == Enrollment::Prepared) {
            revoked = store.revoke(node, entries[i].generation) == Result::Ok || revoked;
        }
    return revoked ? store.prepare(network, receiver, node, generation, key, PairingProfile)
                   : result;
}
} // namespace

bool newKeyPair(Entropy& entropy, KeyPair& keys) {
    keys = KeyPair{};
    if (!entropy.fill(keys.privateKey.data(), keys.privateKey.size()) ||
        !x25519Public(keys.privateKey, keys.publicKey)) {
        wipe(&keys, sizeof(keys));
        return false;
    }
    return true;
}
bool nonzeroRandom(Entropy& entropy, uint64_t& value) {
    value = 0;
    for (int attempt = 0; attempt < RandomAttempts && !value; ++attempt) {
        uint8_t bytes[8]{};
        if (!entropy.fill(bytes, sizeof(bytes))) return false;
        value = get(bytes, sizeof(bytes));
    }
    return value != 0;
}
bool deriveBindingKey(const X25519Key& privateKey, const X25519Key& peerPublic, uint64_t network,
                      uint64_t receiver, uint64_t node, uint64_t generation,
                      const X25519Key& nodePublic, const X25519Key& receiverPublic, Key& key) {
    key = Key{};
    if (!network || !receiver || !node || !generation) return false;
    X25519Key shared{};
    if (!x25519Shared(privateKey, peerPublic, shared)) return false;
    // info = network || receiver || node || generation || node key || receiver key.
    constexpr size_t Id = 8;
    uint8_t info[4 * Id + 2 * X25519Size]{};
    uint8_t* at = info;
    for (uint64_t id : {network, receiver, node, generation}) {
        put(at, id, Id);
        at += Id;
    }
    std::memcpy(at, nodePublic.data(), X25519Size);
    std::memcpy(at + X25519Size, receiverPublic.data(), X25519Size);
    const bool ok = hkdfSha256(shared.data(), shared.size(), reinterpret_cast<const uint8_t*>(Salt),
                               sizeof(Salt) - 1, info, sizeof(info), key.data(), key.size());
    wipe(shared.data(), shared.size());
    return ok;
}
bool buildRequest(uint64_t node, uint64_t nonce, const X25519Key& nodePublic, Frame& frame) {
    frame = Frame{};
    if (!node || !nonce || !nonzero(nodePublic)) return false;
    header(PairingType::Request, 0, node, nonce, RequestPayload, frame);
    std::memcpy(frame.bytes.data() + HeaderSize, nodePublic.data(), X25519Size);
    frame.size = RequestSize;
    return true;
}
bool parseRequest(const Frame& frame, uint64_t& node, uint64_t& nonce, X25519Key& nodePublic) {
    node = nonce = 0;
    nodePublic = X25519Key{};
    if (!shaped(frame, PairingType::Request, RequestSize, RequestPayload)) return false;
    const auto* h = frame.bytes.data();
    std::memcpy(nodePublic.data(), h + HeaderSize, X25519Size);
    if (get(h + NetworkAt, 8) || !get(h + NodeAt, 8) || !get(h + CounterAt, 8) ||
        !nonzero(nodePublic)) {
        nodePublic = X25519Key{};
        return false;
    }
    node = get(h + NodeAt, 8);
    nonce = get(h + CounterAt, 8);
    return true;
}
bool buildOffer(const Offer& offer, const Key& key, Frame& frame) {
    frame = Frame{};
    if (!offer.network || !offer.receiver || !offer.node || !offer.nonce || !offer.generation ||
        !nonzero(offer.receiverPublic))
        return false;
    header(PairingType::Offer, offer.network, offer.node, offer.nonce, OfferPayload, frame);
    auto* p = frame.bytes.data() + HeaderSize;
    std::memcpy(p, offer.receiverPublic.data(), X25519Size);
    put(p + X25519Size, offer.receiver, 8);
    put(p + X25519Size + 8, offer.generation, 8);
    put(p + X25519Size + 16, offer.profile, 2);
    if (!tag(key, PairingType::Offer, offer.nonce, frame.bytes.data(), HeaderSize + OfferPayload,
             frame.bytes.data() + HeaderSize + OfferPayload)) {
        frame = Frame{};
        return false;
    }
    frame.size = OfferSize;
    return true;
}
bool parseOffer(const Frame& frame, Offer& offer) {
    offer = Offer{};
    if (!shaped(frame, PairingType::Offer, OfferSize, OfferPayload)) return false;
    const auto* h = frame.bytes.data();
    const auto* p = h + HeaderSize;
    offer.network = get(h + NetworkAt, 8);
    offer.node = get(h + NodeAt, 8);
    offer.nonce = get(h + CounterAt, 8);
    std::memcpy(offer.receiverPublic.data(), p, X25519Size);
    offer.receiver = get(p + X25519Size, 8);
    offer.generation = get(p + X25519Size + 8, 8);
    offer.profile = uint16_t(get(p + X25519Size + 16, 2));
    if (offer.network && offer.receiver && offer.node && offer.nonce && offer.generation &&
        nonzero(offer.receiverPublic))
        return true;
    offer = Offer{};
    return false;
}
bool verifyOffer(const Frame& frame, const Key& key) {
    Offer parsed{};
    return parseOffer(frame, parsed) &&
           check(key, PairingType::Offer, parsed.nonce, frame.bytes.data(),
                 HeaderSize + OfferPayload, frame.bytes.data() + HeaderSize + OfferPayload);
}
bool buildTagged(PairingType type, uint64_t network, uint64_t node, uint64_t nonce, const Key& key,
                 Frame& frame) {
    frame = Frame{};
    if ((type != PairingType::Confirm && type != PairingType::Done) || !network || !node || !nonce)
        return false;
    header(type, network, node, nonce, 0, frame);
    if (!tag(key, type, nonce, frame.bytes.data(), HeaderSize, frame.bytes.data() + HeaderSize)) {
        frame = Frame{};
        return false;
    }
    frame.size = TaggedSize;
    return true;
}
bool verifyTagged(const Frame& frame, PairingType type, uint64_t network, uint64_t node,
                  uint64_t nonce, const Key& key) {
    if ((type != PairingType::Confirm && type != PairingType::Done) ||
        !shaped(frame, type, TaggedSize, 0))
        return false;
    const auto* h = frame.bytes.data();
    return get(h + NetworkAt, 8) == network && get(h + NodeAt, 8) == node &&
           get(h + CounterAt, 8) == nonce && check(key, type, nonce, h, HeaderSize, h + HeaderSize);
}

// --------------------------------------------------------------------------- receiver
PairingHost::PairingHost(PersistentStore& store, Entropy& entropy, Clock& clock)
    : store_(store), entropy_(entropy), clock_(clock) {}
PairingHost::~PairingHost() {
    wipe(key_.data(), key_.size());
    wipe(last_.key.data(), last_.key.size());
}
void PairingHost::forgetCompleted() {
    wipe(last_.key.data(), last_.key.size());
    last_ = Completed{};
}
void PairingHost::open() {
    close();
    forgetCompleted();
    state_ = HostState::Open;
    openedAt_ = clock_.nowMs();
    paired_ = 0;
}
void PairingHost::close() {
    dropOffer();
    state_ = HostState::Closed;
    count_ = 0;
    for (auto& c : candidates_) c = Candidate{};
}
uint32_t PairingHost::remainingMs() const {
    if (state_ == HostState::Closed) return 0;
    const uint32_t elapsed = clock_.nowMs() - openedAt_;
    return elapsed >= PairingWindowMs ? 0 : PairingWindowMs - elapsed;
}
void PairingHost::poll() {
    if (state_ != HostState::Closed && !remainingMs()) close();
}
void PairingHost::dropOffer() {
    wipe(key_.data(), key_.size());
    offer_ = Offer{};
    offerFrame_ = Frame{};
    if (state_ == HostState::Offered) state_ = paired_ ? HostState::Paired : HostState::Open;
}
bool PairingHost::track(uint64_t node, uint64_t nonce, const X25519Key& publicKey, int16_t rssi) {
    for (size_t i = 0; i < count_; ++i) {
        Candidate& listed = candidates_[i];
        if (listed.node != node) continue;
        // Never replace a pinned key: one injected frame would otherwise redirect the
        // operator's click to an attacker while the list still shows the victim's ID.
        if (listed.nonce != nonce || listed.publicKey != publicKey) listed.conflict = true;
        listed.rssi = rssi;
        listed.seenAt = clock_.nowMs();
        return !listed.conflict;
    }
    if (count_ == MaxCandidates) return false;
    Candidate& slot = candidates_[count_++];
    slot.node = node;
    slot.nonce = nonce;
    slot.publicKey = publicKey;
    slot.rssi = rssi;
    slot.seenAt = clock_.nowMs();
    return true;
}
bool PairingHost::handle(const Frame& frame, int16_t rssi, Frame& reply) {
    reply = Frame{};
    poll();
    const uint8_t type = untrustedType(frame);
    // A repeated confirmation of the last completed exchange, even after the window closed
    // or while another node is being added: answer with the identical JOIN_DONE, within
    // the bounds a genuine node needs.
    if (last_.node && (uint32_t(clock_.nowMs() - last_.at) >= DoneReplayMs ||
                       last_.replies >= MaxDoneReplies))
        forgetCompleted();
    if (type == uint8_t(PairingType::Confirm) && last_.node &&
        verifyTagged(frame, PairingType::Confirm, last_.network, last_.node, last_.nonce,
                     last_.key)) {
        ++last_.replies;
        reply = last_.done;
        return true;
    }
    if (state_ == HostState::Closed) return false;
    if (type == uint8_t(PairingType::Request)) {
        uint64_t node = 0;
        uint64_t nonce = 0;
        X25519Key publicKey{};
        if (!parseRequest(frame, node, nonce, publicKey) || node == store_.device()) return false;
        const bool consistent = track(node, nonce, publicKey, rssi);
        if (state_ != HostState::Offered || node != offer_.node) return false;
        // Two devices claim the offered node: withdraw the offer before either confirms.
        // Requests are unauthenticated, so this lets a radio attacker cancel an offer
        // (like jamming would) but never redirect it.
        if (!consistent) {
            dropOffer();
            return false;
        }
        reply = offerFrame_; // Identical retransmission for a repeated request.
        return true;
    }
    if (type != uint8_t(PairingType::Confirm) || state_ != HostState::Offered ||
        !verifyTagged(frame, PairingType::Confirm, offer_.network, offer_.node, offer_.nonce, key_))
        return false;
    // The node proved it holds the key: store the binding, already active.
    Completed done{};
    done.network = offer_.network;
    done.node = offer_.node;
    done.nonce = offer_.nonce;
    done.key = key_;
    if (prepareFresh(store_, offer_.network, offer_.receiver, offer_.node, offer_.generation,
                     key_) != Result::Ok ||
        store_.activate(offer_.node, offer_.generation) != Result::Ok ||
        !buildTagged(PairingType::Done, done.network, done.node, done.nonce, done.key, done.done)) {
        store_.revoke(offer_.node, offer_.generation);
        wipe(done.key.data(), done.key.size());
        return false;
    }
    forgetCompleted();
    done.at = clock_.nowMs();
    done.replies = 1; // This first JOIN_DONE.
    last_ = done;
    wipe(done.key.data(), done.key.size());
    paired_ = offer_.node;
    dropOffer();
    state_ = HostState::Paired;
    reply = last_.done;
    return true;
}
Result PairingHost::accept(uint64_t node) {
    poll();
    if (state_ == HostState::Closed) return Result::Invalid;
    const Candidate* candidate = nullptr;
    for (size_t i = 0; i < count_; ++i)
        if (candidates_[i].node == node) candidate = &candidates_[i];
    if (!candidate) return Result::NotFound;
    if (candidate->conflict) return Result::Conflict;
    if (!store_.freeSlots()) return store_.healthy() ? Result::Full : Result::StorageError;
    KeyPair keys{};
    Offer offer{};
    offer.node = node;
    offer.nonce = candidate->nonce;
    offer.receiver = store_.device();
    offer.profile = PairingProfile;
    offer.network = store_.network();
    Key key{};
    Frame frame{};
    bool ok = newKeyPair(entropy_, keys) &&
              (offer.network || nonzeroRandom(entropy_, offer.network)) &&
              nonzeroRandom(entropy_, offer.generation);
    if (ok) {
        offer.receiverPublic = keys.publicKey;
        ok = deriveBindingKey(keys.privateKey, candidate->publicKey, offer.network, offer.receiver,
                              node, offer.generation, candidate->publicKey, keys.publicKey, key) &&
             buildOffer(offer, key, frame);
    }
    wipe(&keys, sizeof(keys));
    if (!ok) {
        wipe(key.data(), key.size());
        return Result::CryptoError;
    }
    dropOffer(); // Replaces any earlier offer; nothing was stored for it.
    offer_ = offer;
    key_ = key;
    wipe(key.data(), key.size());
    offerFrame_ = frame;
    state_ = HostState::Offered;
    return Result::Ok;
}

// --------------------------------------------------------------------------- node
PairingClient::PairingClient(ReceiverRadio& radio, Clock& clock, Jitter& jitter,
                             PersistentStore& store, Entropy& entropy)
    : radio_(radio), clock_(clock), jitter_(jitter), store_(store), entropy_(entropy) {}
PairingClient::~PairingClient() {
    wipe(&keys_, sizeof(keys_));
    wipe(key_.data(), key_.size());
}
bool PairingClient::start() {
    if (state_ != ClientState::Idle || !store_.healthy() || store_.role() != Role::Transmitter ||
        !store_.freeSlots() || !newKeyPair(entropy_, keys_) || !nonzeroRandom(entropy_, nonce_) ||
        !buildRequest(store_.device(), nonce_, keys_.publicKey, outgoing_)) {
        fail();
        return false;
    }
    startedAt_ = clock_.nowMs();
    transmit(ClientState::Listening);
    return state_ != ClientState::Failed;
}
void PairingClient::transmit(ClientState next) {
    phaseAt_ = clock_.nowMs();
    if (!radio_.startTransmit(outgoing_)) {
        fail();
        return;
    }
    state_ = ClientState::Sending;
    afterSend_ = next;
}
void PairingClient::fail() {
    wipe(&keys_, sizeof(keys_));
    wipe(key_.data(), key_.size());
    state_ = ClientState::Failed;
}
void PairingClient::handleOffer(const Frame& frame) {
    Offer offer{};
    const uint64_t self = store_.device();
    Key key{};
    if (!parseOffer(frame, offer) || offer.node != self || offer.nonce != nonce_ ||
        offer.receiver == self || offer.profile != PairingProfile ||
        !deriveBindingKey(keys_.privateKey, offer.receiverPublic, offer.network, offer.receiver,
                          self, offer.generation, keys_.publicKey, offer.receiverPublic, key) ||
        !verifyOffer(frame, key)) {
        wipe(key.data(), key.size());
        return; // Not ours or forged: keep listening.
    }
    // Refuse before confirming what storage could not accept, so the receiver never stores
    // a binding this node would then drop: one network (and receiver) per device.
    const bool compatible = !store_.network() || (store_.network() == offer.network &&
                                                  store_.receiver() == offer.receiver);
    if (!compatible ||
        !buildTagged(PairingType::Confirm, offer.network, self, nonce_, key, outgoing_)) {
        wipe(key.data(), key.size());
        fail();
        return;
    }
    paired_ = offer;
    key_ = key;
    wipe(key.data(), key.size());
    confirms_ = 1;
    transmit(ClientState::AwaitingDone);
}
void PairingClient::poll() {
    if (state_ == ClientState::Idle || state_ == ClientState::Paired ||
        state_ == ClientState::Failed)
        return;
    const uint32_t now = clock_.nowMs();
    if (uint32_t(now - startedAt_) >= DeadlineMs) {
        fail();
        return;
    }
    if (state_ == ClientState::Sending) {
        uint32_t completed = 0;
        const auto status = radio_.transmitStatus(completed);
        if (status == TransmitStatus::Error ||
            (status == TransmitStatus::Pending && uint32_t(now - phaseAt_) >= TransmitTimeoutMs)) {
            fail();
        } else if (status == TransmitStatus::Complete) {
            // The adapter arms continuous RX at TX completion.
            state_ = afterSend_;
            phaseAt_ = now;
        }
        return;
    }
    if (state_ == ClientState::Waiting) {
        if (uint32_t(now - phaseAt_) >= waitMs_) transmit(ClientState::Listening);
        return;
    }
    Frame frame{};
    const auto status = radio_.receive(frame);
    if (status == ReceiveStatus::Error) {
        fail();
        return;
    }
    if (status == ReceiveStatus::Received) {
        const uint8_t type = untrustedType(frame);
        if (state_ == ClientState::Listening && type == uint8_t(PairingType::Offer)) {
            handleOffer(frame);
            return;
        }
        if (state_ == ClientState::AwaitingDone && type == uint8_t(PairingType::Done) &&
            verifyTagged(frame, PairingType::Done, paired_.network, store_.device(), nonce_,
                         key_)) {
            const uint64_t self = store_.device();
            const Result stored = prepareFresh(store_, paired_.network, paired_.receiver, self,
                                               paired_.generation, key_);
            if (stored != Result::Ok || store_.activate(self, paired_.generation) != Result::Ok) {
                if (stored == Result::Ok) store_.revoke(self, paired_.generation);
                fail();
                return;
            }
            wipe(&keys_, sizeof(keys_));
            wipe(key_.data(), key_.size());
            state_ = ClientState::Paired;
            return;
        }
    }
    if (uint32_t(now - phaseAt_) < ListenMs) return;
    if (state_ == ClientState::AwaitingDone) {
        if (confirms_ >= ConfirmAttempts) {
            fail();
            return;
        }
        ++confirms_;
        transmit(ClientState::AwaitingDone);
        return;
    }
    uint32_t wait = RetryMinMs;
    if (!jitter_.between(RetryMinMs, RetryMaxMs, wait)) wait = RetryMaxMs;
    waitMs_ = wait;
    phaseAt_ = now;
    state_ = ClientState::Waiting;
}
} // namespace cajui
