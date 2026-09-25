# Persistent state

`PersistentStore` implements `CounterStore` and `Journal` over a `RecordStore`: named
records, each replaced atomically and independently. `records.h` holds the pure record
codecs, which tests exercise without an adapter. The ESP32 adapter stores each record as
an NVS blob in the dedicated `cajui` partition, namespace `store`. Every record is
versioned, big-endian and protected by CRC32 against accidental corruption. CRC is not
authentication or physical tamper protection. The adapter does not enable NVS/flash
encryption; USB and physical flash access are trusted administrative access in this
development version.

## Records (layout v2)

| Record | Content | Written by |
| --- | --- | --- |
| `registry` | role, device, network, receiver, profile and up to 16 enrollments (state, node, generation, key, reserved counter) | administration; a transmitter's counter reservation |
| `retired` | generation and key fingerprint of up to 64 credentials removed from the registry | freeing a slot, reset |
| `r00`–`r0f` | receiver receipt of one slot: generation, last counter, last frame and the queue position it filled | each accepted sample |
| `q00`–`q7f` | one queued frame: sequence, slot, generation, frame | each accepted sample |
| `head` | sequence of the queue front | each forwarded sample |

The receiver queue is a ring of 128 records addressed by a monotonic sequence. Accepting
a sample writes **two small records**: the queue record at the tail, not yet referenced,
then the slot's receipt, which references it (`through` = its sequence + 1). The receipt
write is the commit point. At mount the tail is recovered from the receipts, so a queue
record whose receipt write was lost is not a sample and is overwritten by the next one.
Every sequence between head and tail must hold a valid record; anything else is
corruption, never a shorter queue. Forwarding a sample writes only `head`.

Each sample therefore costs two writes of at most 166 bytes, and each forward one write
of 14 bytes, whatever the number of enrollments or queued samples. The v1 layout rewrote
the whole state, up to 20 KB, twice per sample. The registry is rewritten only by
administration and, on a transmitter, once per sample to reserve its counter; with one
enrollment that record is 84 bytes.

## Transaction boundary

An adapter error is ambiguous: a write may have become durable before returning an
error. The store therefore latches unhealthy and refuses further mutations and counter
reservations until remounted. After restart the records are validated again: version,
identity, role, sizes, state constraints, unique credentials, the retired list, and
every retained authenticated frame. Corruption is an error, never an empty
configuration. No initialization error triggers an automatic erase.

Multi-record operations are ordered so that every intermediate state is valid:

- **Accepting a sample**: queue record, then receipt (above).
- **Freeing a slot**: the revoked credential is added to `retired`, then the registry
  replaces it. A revoked entry that is also retired is a valid intermediate state.
- **Reset**: the registry revokes every enrollment, `retired` lists them, `head` drops
  the queue (only when discarding was requested), and an empty registry is written.
  Repeating an interrupted reset completes it.

Counter reservations are persisted before use. An unreturned reservation may be
skipped after failure; it must never be reused after it might have encrypted a frame.

## Limits and retained history

- 16 enrollment slots per device. Prepared and active enrollments, and revoked ones with
  samples still queued, occupy a slot. When none is empty, `prepare` frees a revoked slot
  without queued samples, so rotation, re-pairing and re-enrollment keep working.
- A freed credential joins the retired list: its generation and a 64-bit HKDF-SHA256
  fingerprint of its key, never the key. A retired generation or key is refused forever,
  because reusing a key with fresh counters would repeat GCM nonces. The list keeps the
  latest 64 retirements; older ones are forgotten. Keys and generations are random, so
  only a deliberate replay of an old credential, such as an old recovery file, could
  meet a forgotten one.
- One active generation per node on a transmitter. A receiver may hold two for a node
  while a re-paired node has not used its new key yet (see
  [radio pairing](radio-pairing.md)); USB activation revokes the previous one at once.
- A 128-frame global receiver queue. Full means no new acceptance ACK; an already
  accepted duplicate can still be acknowledged.
- `forwarded` removes only the queue head matching node, generation and counter. The
  receipt survives draining, so a repeated frame cannot be enqueued again.

The registry revision and counters cannot wrap. Public operations are serialized;
this class is not a multi-task concurrent store. The expected-counter check detects
stale commits, but is not a substitute for synchronizing multiple tasks.

`PersistentStore` is neither copyable nor movable: copying its cached state could
reserve the same counter twice under one key. There must be exactly one live store
owner per backing store, including across separate adapter handles to the same NVS
namespace. Do not construct a second store while the first is in use.

## Leaving a network

`CJ1 RESET` (admin mode, [provisioning](provisioning.md)) retires every enrollment and
empties the registry, so the device can join another network by USB or radio pairing.
Its old keys can never return, which also makes existing recovery files for the device
useless. A receiver refuses while samples are queued unless asked to discard them. The
other side keeps its binding until it is revoked there.

## Migration from v1

A device with only the v1 `snapshot` record is migrated at its first mount: queue
records, receipts and `head` are written, then the registry, and only then is the
snapshot erased. Until the registry exists, a restart migrates again from the untouched
snapshot. A damaged snapshot is reported as such and never migrated. Migration needs
about 42 KB of heap once; the store itself keeps no copy of the queue in RAM.

## Partition layout

`partitions.csv` reserves 256 KiB for the dedicated NVS partition at `0x310000`,
with a single 3 MiB factory application. There is no OTA slot in this development
layout. Back up existing device state before changing a partition table; never restore
old counter state under a key that has already been used with newer counters.

Only an absent registry and snapshot permit initial setup. Restoring lost or rolled-back
state from a recovery file cannot be treated as routine resume: use fresh credentials
through an explicit recovery procedure. The USB client refuses resume if a device
previously confirmed as prepared has lost that enrollment.

## Validation boundary

Host Unity tests inject failures before and after each record write, ambiguous writes,
torn and corrupt records, full queues and ring wrap-around, replay after reboot,
rotations, slot reuse, reset and migration. They also load semantically invalid records
with a valid CRC. These tests validate application logic. Build-only ESP32 tests verify
compilation of the mbedTLS/NVS adapter. Software restarts on devices are a separate
manual test; neither proves behavior under arbitrary power cuts or long-term flash wear.
The NVS adapter relies on the per-key atomicity documented by Espressif; a physical
power-cut campaign remains necessary before claiming field robustness.

Reference: [ESP-IDF NVS storage and recovery](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/storage/nvs_flash.html).
