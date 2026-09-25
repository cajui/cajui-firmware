// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "snapshot.h"

namespace cajui {
// Record layout v2, docs/persistence.md. Each record is one named blob with its own CRC,
// replaced atomically by the adapter. Pure codecs: no storage adapter involved.
namespace records {
constexpr size_t RetiredCapacity = 64;
constexpr size_t CrcSize = 4;
// kind 1, version 1, role 1, device 8, revision 8, network 8, receiver 8, profile 2, count 1.
constexpr size_t RegistryHeaderSize = 1 + 1 + 1 + 8 + 8 + 8 + 8 + 2 + 1;
// slot 1, state 1, node 8, generation 8, key, counter 8.
constexpr size_t RegistryEntrySize = 1 + 1 + 8 + 8 + KeySize + 8;
constexpr size_t RegistryCapacity =
    RegistryHeaderSize + BindingCapacity * RegistryEntrySize + CrcSize;
// kind 1, version 1, generation 8, counter 8, through 8, frame size 2, frame; version 2
// then adds the ACK power command (1). Version 1 receipts decode with KeepPower.
constexpr size_t ReceiptCapacity = 1 + 1 + 8 + 8 + 8 + 2 + MaxFrame + 1 + CrcSize;
// kind 1, version 1, sequence 8, slot 1, generation 8, frame size 2, frame; version 2 then
// adds the frame's link quality: known 1, RSSI 2, SNR 2. Version 1 records still decode.
constexpr size_t LinkSize = 1 + 2 + 2;
constexpr size_t QueueRecordCapacity = 1 + 1 + 8 + 1 + 8 + 2 + MaxFrame + LinkSize + CrcSize;
// kind 1, version 1, sequence 8.
constexpr size_t HeadSize = 1 + 1 + 8 + CrcSize;
// kind 1, version 1, count 1, then generation 8 and fingerprint 8 per retired credential.
constexpr size_t RetiredRecordCapacity = 1 + 1 + 1 + RetiredCapacity * 16 + CrcSize;
constexpr size_t BufferCapacity =
    RetiredRecordCapacity > RegistryCapacity ? RetiredRecordCapacity : RegistryCapacity;
static_assert(BufferCapacity >= ReceiptCapacity && BufferCapacity >= QueueRecordCapacity,
              "Scratch buffer must hold every record");

// Record names. NVS keys have at most 15 characters.
constexpr char RegistryKey[] = "registry", RetiredKey[] = "retired", HeadKey[] = "head",
               LegacyKey[] = "snapshot";
constexpr size_t NameCapacity = 4;
void receiptKey(size_t slot, char (&name)[NameCapacity]);
void queueKey(uint64_t sequence, char (&name)[NameCapacity]);

struct Entry {
    Enrollment state = Enrollment::Empty;
    uint64_t node = 0, generation = 0, counter = 0;
    Key key{};
};
// Network identity and enrollments. Written only by administration and, on a
// transmitter, by counter reservations (a transmitter holds few enrollments).
struct Registry {
    uint64_t revision = 0, network = 0, receiver = 0;
    uint16_t profile = 0;
    std::array<Entry, BindingCapacity> entries{};
};
// Credentials removed from the registry to free their slot. They can never be enrolled
// again: reusing a key with fresh counters would repeat GCM nonces.
struct Retired {
    uint64_t generation = 0, fingerprint = 0;
};
struct RetiredList {
    size_t count = 0;
    std::array<Retired, RetiredCapacity> items{};
};
// Receiver replay state of one slot. `through` is one past the queue sequence of the last
// sample this receipt enqueued, or zero: the queue tail is recovered from receipts, so a
// queue record written before a failed receipt write is never mistaken for a sample.
struct ReceiptRecord {
    uint64_t generation = 0, through = 0;
    Receipt receipt{};
};
struct QueueRecord {
    uint64_t sequence = 0, generation = 0;
    uint8_t slot = 0;
    Frame frame{};
    Link link{}; // Unknown for records written before version 2.
};

size_t encode(const Registry&, Role, uint64_t device, uint8_t* output);
// Format, identity and field checks; cross-record rules are in validate().
Health decode(const uint8_t* input, size_t size, Role, uint64_t device, Registry&);
// Uniqueness, one active generation per node and the retired list.
Health validate(const Registry&, Role, uint64_t device, const RetiredList&);
size_t encode(const RetiredList&, uint8_t* output);
bool decode(const uint8_t* input, size_t size, RetiredList&);
size_t encode(const ReceiptRecord&, uint8_t* output);
bool decode(const uint8_t* input, size_t size, ReceiptRecord&);
size_t encode(const QueueRecord&, uint8_t* output);
bool decode(const uint8_t* input, size_t size, QueueRecord&);
size_t encodeHead(uint64_t sequence, uint8_t* output);
bool decodeHead(const uint8_t* input, size_t size, uint64_t& sequence);

// One-way 64-bit fingerprint of a key (HKDF-SHA256), so retired keys are not kept.
bool fingerprint(const Key&, uint64_t&);
bool isRetired(const RetiredList&, uint64_t generation, uint64_t fingerprint);
// Appends; when full, the oldest retirement is forgotten (documented limit).
void retire(RetiredList&, uint64_t generation, uint64_t fingerprint);
} // namespace records
} // namespace cajui
