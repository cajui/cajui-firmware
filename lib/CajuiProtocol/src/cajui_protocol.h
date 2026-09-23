#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace cajui {
// Binary wire v1. Not compatible with earlier text formats.
constexpr size_t HeaderSize = 32, TagSize = 16, MaxReadings = 8;
constexpr size_t MaxPayload = 7 + MaxReadings * 10;
constexpr size_t MaxFrame = HeaderSize + MaxPayload + TagSize;
using Key = std::array<uint8_t, 16>;
using Tag = std::array<uint8_t, TagSize>;
enum class Type : uint8_t { Data = 1, Ack = 2 };
enum class Status : uint8_t { Ok = 0, Error = 1, Skipped = 2 };
enum class Result { Ok, Invalid, Unauthorized, CryptoError, StorageError,
                    Full, Replay, Conflict, Duplicate };
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
    uint64_t counter = 0;
    Data data{};
    Tag dataTag{}; // ACK also authenticates the accepted DATA tag.
};
// AES-128-GCM backend: mbedTLS on ESP32, OpenSSL on the host.
// Use established crypto libraries. Input and output buffers must be distinct.
bool encrypt(const Key&, const uint8_t nonce[12], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, uint8_t* output, uint8_t tag[16]);
bool decrypt(const Key&, const uint8_t nonce[12], const uint8_t* aad, size_t aadSize,
             const uint8_t* input, size_t size, const uint8_t tag[16], uint8_t* output);
Result seal(const Binding&, const Message&, Frame&);
Result open(const Binding&, const Frame&, Message&);
bool sameFrame(const Frame&, const Frame&);

// Created during provisioning; retained after draining the queue.
struct Receipt { uint64_t counter = 0; Frame last{}; };
class Journal {
public:
    virtual ~Journal() = default;
    virtual bool load(const Binding&, Receipt&) = 0;
    // Contract: queue and Receipt become atomic and durable before returning Ok.
    // Failure leaves both unchanged; expectedCounter guards concurrent updates.
    virtual Result commit(const Binding&, uint64_t expectedCounter,
                          const Receipt&, const Data&) = 0;
};
Result receive(const Binding&, const Frame&, Journal&, Frame& ack);

class CounterStore {
public:
    virtual ~CounterStore() = default;
    // Durable reservation BEFORE use, monotonic per credential, never returns zero.
    virtual bool reserve(const Binding&, uint64_t& counter) = 0;
};
// One pending sample per node. Retries reuse identical bytes without re-encryption.
class Sender {
public:
    Result begin(const Binding&, const Data&, CounterStore&);
    const Frame* nextAttempt(); // At most three transmissions; does not drive the radio.
    Result acknowledge(const Frame&);
    void abandon(); // Stop radio/close the ACK window first; account for unconfirmed loss.
    bool delivered() const { return delivered_; }
    uint8_t attempts() const { return attempts_; }
    static constexpr uint8_t MaxAttempts = 3;
private:
    Binding binding_{};
    Frame pending_{};
    uint64_t counter_ = 0;
    uint8_t attempts_ = 0;
    bool delivered_ = false;
};
} // namespace cajui
