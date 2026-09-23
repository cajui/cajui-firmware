# Persistent state

`PersistentStore` implements `CounterStore` and `Journal` over an `AtomicBlob`.
The ESP32 adapter uses one NVS blob in the dedicated `cajui` partition, namespace
`store`, key `snapshot`. The codec is versioned, big-endian and protected by CRC32
for accidental corruption. CRC is not authentication or physical tamper protection.
The adapter does not enable NVS/flash encryption; USB and physical flash access are
trusted administrative access in this development version.

## Transaction boundary

A snapshot contains the network/profile, credential generations and their enrollment
states, reserved counters, replay receipts and queued authenticated frames. Updating
the queue and its receipt replaces **one blob**, followed by `nvs_commit`. There is
no multi-key transaction that could persist a receipt without its sample.

An adapter error is ambiguous: a write may have become durable before returning an
error. The store therefore latches unhealthy and refuses further mutations and
counter reservations until remounted. After restart, the complete old or new snapshot
must be loaded. The store validates version, identity, role, sizes, state constraints,
unique credentials and every retained authenticated frame. Corruption is an error,
never an empty configuration. No initialization error triggers an automatic erase.

Counter reservations are persisted before use. An unreturned reservation may be
skipped after failure; it must never be reused after it might have encrypted a frame.
Each reservation currently writes a snapshot. Only occupied enrollment and queue
slots are serialized, so an empty transmitter queue is not rewritten as 128 empty
records. A single-enrollment empty-queue snapshot is 232 bytes. Flash endurance and
full-queue write latency still require workload measurements.

## Limits and retained history

- 16 enrollment generations per device, including prepared and revoked records.
- One active generation per node. Activating a prepared generation revokes the prior
  active generation in the same atomic snapshot.
- A 128-frame global receiver queue. Full means no new acceptance ACK; an already
  accepted duplicate can still be acknowledged.
- Previously used keys/generation IDs cannot be reused. Revoked records remain as
  tombstones; queued samples can still be decoded with their historical credential.
- A full enrollment registry is an explicit error. There is no automatic tombstone
  pruning, migration to another network or factory-reset command in this version.
- `forwarded` removes only the queue head matching node, generation and counter.
  The receipt survives draining, so a repeated frame cannot be enqueued again.

The snapshot revision and counters cannot wrap. Public operations are serialized;
this class is not a multi-task concurrent store. It owns scratch buffers to avoid
large temporary allocations on the MCU task stack. The expected-counter check detects
stale commits, but is not a substitute for synchronizing multiple tasks.

## Partition layout

`partitions.csv` reserves 256 KiB for the dedicated NVS partition at `0x310000`,
with a single 3 MiB factory application. There is no OTA slot in this development
layout. The administration build uses this table. Back up existing device state
before changing a partition table; never restore old counter state under a key
that has already been used with newer counters.

Only an absent snapshot permits initial setup. Restoring lost or rolled-back state
from a recovery file cannot be treated as routine resume: use fresh credentials
through an explicit recovery procedure. The USB client refuses resume if a device
previously confirmed as prepared has lost that enrollment. State-file phase updates
are durable, but deleting/rolling back flash or the private recovery file is outside
the automatic interruption recovery guarantee.

## Validation boundary

Host Unity tests inject failures before/after replacement, torn/corrupt snapshots,
full queues, replay after reboot, rotations and counter exhaustion. They also load
semantically invalid snapshots with a valid CRC. These tests validate application
logic. Build-only ESP32 tests verify compilation of the mbedTLS/NVS adapter.
Software restarts on devices are a separate manual test; neither proves behavior
under arbitrary power cuts or long-term flash wear. The NVS adapter relies on the
storage recovery guarantees documented by Espressif; a physical power-cut campaign
remains necessary before claiming field robustness.

Reference: [ESP-IDF NVS storage and recovery](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/storage/nvs_flash.html).
