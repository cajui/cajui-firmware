// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_protocol.h"

namespace cajui {
constexpr size_t BindingCapacity = 16, QueueCapacity = 128;
// Legacy layout, read only to migrate a device to records v2 (records.h); the storage
// types below are shared by both layouts.
// Snapshot v1: header, one record per occupied enrollment and queued frame, then CRC32.
// Header: magic 4, version 1, role 1, device 8, revision 8, network 8, receiver 8,
// profile 2, queue count 1, enrollment count 1.
constexpr size_t SnapshotHeaderSize = 4 + 1 + 1 + 8 + 8 + 8 + 8 + 2 + 1 + 1, ChecksumSize = 4;
// State 1, node 8, generation 8, key, counter 8, receipt counter 8, frame size 2, frame.
constexpr size_t EntryRecordSize = 1 + 8 + 8 + sizeof(Key) + 8 + 8 + 2 + MaxFrame;
constexpr size_t QueueRecordSize = 1 + 2 + MaxFrame; // Entry index, frame size, frame.
constexpr size_t MinSnapshotSize = SnapshotHeaderSize + ChecksumSize;
constexpr size_t SnapshotSize =
    MinSnapshotSize + BindingCapacity * EntryRecordSize + QueueCapacity * QueueRecordSize;
// Stored snapshots depend on these sizes: changing MaxFrame requires a new snapshot version.
static_assert(EntryRecordSize == 186 && QueueRecordSize == 138, "Snapshot v1 layout changed");
enum class Role : uint8_t { Transmitter = 1, Receiver = 2 };
enum class Enrollment : uint8_t { Empty = 0, Prepared = 1, Active = 2, Revoked = 3 };
// Why the store refuses work; Ready is the only usable state.
enum class Health : uint8_t {
    Unmounted,
    Ready,
    Identity,
    ReadError,
    Corrupt,
    Format,
    Role,
    Device,
    Invalid,
    WriteError
};
// The v1 state and its snapshot encoding, kept for migration and its tests. Pure.
namespace snapshot {
struct Entry {
    Enrollment state = Enrollment::Empty;
    uint64_t node = 0, generation = 0, counter = 0;
    Key key{};
    Receipt receipt{};
};
struct Queued {
    uint8_t entry = 0;
    Frame frame{};
};
struct State {
    uint64_t revision = 0, network = 0, receiver = 0;
    uint16_t profile = 0, count = 0;
    std::array<Entry, BindingCapacity> entries{};
    std::array<Queued, QueueCapacity> queue{};
};
bool nonzero(const Key&);
Binding binding(const Entry&, uint64_t network);
// Clears in place: `state = State{}` may build a ~22 KB temporary on the 8 KB loop-task stack.
void reset(State&);
// Writes a complete snapshot including its CRC; returns its size (at most SnapshotSize).
size_t encode(const State&, Role, uint64_t device, uint8_t* output);
// Parses and validates a complete snapshot into a reset state. Ready means valid.
Health decode(const uint8_t* input, size_t size, Role, uint64_t device, State&);
} // namespace snapshot
} // namespace cajui
