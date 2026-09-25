// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace cajui {
// Binary wire v1. Not compatible with earlier text formats.
constexpr size_t HeaderSize = 32, TagSize = 16, NonceSize = 12, KeySize = 16, MaxReadings = 8;
// DATA payload: battery u16, next interval u32 and count u8, then fixed-size readings.
constexpr size_t DataPrefixSize = 7, ReadingSize = 10;
constexpr size_t MinDataPayload = DataPrefixSize + ReadingSize;
constexpr size_t MaxPayload = DataPrefixSize + MaxReadings * ReadingSize;
constexpr size_t MaxFrame = HeaderSize + MaxPayload + TagSize;
using Key = std::array<uint8_t, KeySize>;
using Tag = std::array<uint8_t, TagSize>;
enum class Type : uint8_t { Data = 1, Ack = 2 };
// Wire versions of DATA and ACK. Version 2 adds a transmit-power command to the ACK; a
// receiver answers in the version of the DATA it accepted, so version 1 nodes keep working.
constexpr uint8_t WireV1 = 1, WireV2 = 2;
// Power command meaning "keep the current power": the only value a v1 ACK can express.
constexpr int8_t KeepPower = 127;
// Pairing types, defined in docs/radio-pairing.md and handled by CajuiPairing.
constexpr uint8_t FirstPairingType = 3, LastPairingType = 6;
constexpr size_t X25519Size = 32;
using X25519Key = std::array<uint8_t, X25519Size>;
enum class Status : uint8_t { Ok = 0, Error = 1, Skipped = 2 };
enum class Result {
    Ok,
    Invalid,
    Unauthorized,
    CryptoError,
    StorageError,
    Full,
    Replay,
    Conflict,
    Duplicate,
    NotFound
};
struct Binding {
    uint64_t network = 0, node = 0;
    Key key{};
    bool active = false;
};
struct Reading {
    uint16_t sensor = 0, metric = 0;
    uint8_t unit = 0;
    Status status = Status::Ok;
    int32_t milliValue = 0;
};
struct Data {
    uint16_t batteryMv = 0; // 0 = unknown, not a zero-volt measurement.
    uint32_t nextSeconds = 0;
    uint8_t count = 0;
    std::array<Reading, MaxReadings> readings{};
};
struct Frame {
    std::array<uint8_t, MaxFrame> bytes{};
    size_t size = 0;
};
struct Message {
    Type type = Type::Data;
    uint8_t version = WireV1;
    uint64_t counter = 0;
    Data data{};
    Tag dataTag{}; // ACK also authenticates the accepted DATA tag.
    // v2 ACK: transmit power the node should use from its next frame, in dBm, or KeepPower.
    int8_t powerDbm = KeepPower;
};
// Signal quality of a received frame as measured by the receiving radio. Not part of any
// frame and not authenticated; `known` is false when it was not measured, never a zero.
struct Link {
    bool known = false;
    int16_t rssiDbm = 0;
    int16_t snrTenthsDb = 0;
};
// AES-128-GCM backend: mbedTLS on ESP32, OpenSSL on the host.
// Use established crypto libraries. Input and output buffers must be distinct.
bool encrypt(const Key&, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[TagSize]);
bool decrypt(const Key&, const uint8_t nonce[NonceSize], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[TagSize], uint8_t* output);
// X25519 (RFC 7748) and HKDF-SHA256 (RFC 5869) through the same established libraries.
// x25519Shared rejects an all-zero result (a low-order peer key).
bool x25519Public(const X25519Key& privateKey, X25519Key& publicKey);
bool x25519Shared(const X25519Key& privateKey, const X25519Key& peerPublic, X25519Key& shared);
bool hkdfSha256(const uint8_t* ikm, size_t ikmSize, const uint8_t* salt, size_t saltSize,
                const uint8_t* info, size_t infoSize, uint8_t* output, size_t outputSize);
// SHA-256 over a stream, and ECDSA P-256 verification of a SHA-256 digest, for signed
// firmware updates. Same libraries as above; nothing is implemented locally.
constexpr size_t Sha256Size = 32;
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool update(const uint8_t* data, size_t size);
    // Ends the stream; further updates fail.
    bool finish(uint8_t digest[Sha256Size]);

private:
    void* context_ = nullptr;
    bool ok_ = false;
};
// `publicKey` is a DER SubjectPublicKeyInfo of a P-256 key; `signature` is DER (r, s).
bool verifyP256(const uint8_t* publicKey, size_t keySize, const uint8_t digest[Sha256Size],
                const uint8_t* signature, size_t signatureSize);
// Header type of a well-formed v1 envelope, or zero. Unauthenticated: routing only.
uint8_t untrustedType(const Frame&);
// Unauthenticated routing hint only; callers MUST authenticate with open/receive.
// Returns zero for malformed/non-DATA envelopes. Never creates a binding.
uint64_t untrustedDataNode(const Frame&);
Result seal(const Binding&, const Message&, Frame&);
Result open(const Binding&, const Frame&, Message&);
bool sameFrame(const Frame&, const Frame&);

// Created during provisioning; retained after draining the queue.
struct Receipt {
    uint64_t counter = 0;
    Frame last{};
    Link link{}; // Of `last`; stored with its queued sample, not with the receipt.
    // Power command of the ACK for `last`. Stored with the receipt: a repeated ACK reuses
    // the counter and therefore the GCM nonce, so it must repeat these exact bytes.
    int8_t ackPower = KeepPower;
};
class Journal {
public:
    virtual ~Journal() = default;
    virtual bool load(const Binding&, Receipt&) = 0;
    // Contract: queue and Receipt become atomic and durable before returning Ok.
    // Failure leaves both unchanged; expectedCounter guards concurrent updates.
    virtual Result commit(const Binding&, uint64_t expectedCounter, const Receipt&) = 0;
};
// A v2 DATA frame gets a v2 ACK carrying `powerDbm` (KeepPower for no change); a v1 frame
// gets a v1 ACK. `link` is recorded with the committed sample. A duplicate is answered
// with the command stored with its receipt, never a new one: identical bytes, same nonce.
Result receive(const Binding&, const Frame&, Journal&, Frame& ack, const Link& link = Link{},
               int8_t powerDbm = KeepPower);

class CounterStore {
public:
    virtual ~CounterStore() = default;
    // Durable reservation BEFORE use, monotonic per credential, never returns zero.
    virtual bool reserve(const Binding&, uint64_t& counter) = 0;
};
// One pending sample per node. Retries reuse identical bytes without re-encryption.
class Sender {
public:
    // Seals DATA in `version`; v2 lets the receiver answer with a power command.
    Result begin(const Binding&, const Data&, CounterStore&, uint8_t version = WireV2);
    const Frame* nextAttempt(); // At most three transmissions; does not drive the radio.
    Result acknowledge(const Frame&);
    // Power command of the accepted ACK; KeepPower before one or for a v1 exchange.
    int8_t powerCommand() const { return power_; }
    void abandon(); // Stop radio/close the ACK window first; account for unconfirmed loss.
    bool delivered() const { return delivered_; }
    uint8_t attempts() const { return attempts_; }
    static constexpr uint8_t MaxAttempts = 3;

private:
    Binding binding_{};
    Frame pending_{};
    uint64_t counter_ = 0;
    uint8_t attempts_ = 0, version_ = WireV1;
    int8_t power_ = KeepPower;
    bool delivered_ = false;
};
} // namespace cajui
