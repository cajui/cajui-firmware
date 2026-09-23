# Direct LoRa protocol v1

Draft implementation contract. The shared core is implemented; storage, provisioning
and radio integrations described below are requirements for subsequent work. This
specification has not undergone an independent security audit.

## Scope

A star network has one receiver and multiple sensor nodes. Each node can report
multiple sensor metrics. Nodes and their receiver share a radio profile. The first
version uses local USB provisioning, not automatic radio pairing. It does not
implement LoRaWAN, mesh routing, TDMA, actuator commands or radio firmware updates.

Network IDs and node IDs are nonzero unsigned 64-bit values. They are public
routing identifiers, not credentials. A binding has a unique random 128-bit key
shared only by one node and its authorized receiver. Moving a node to another
network or replacing its receiver requires reprovisioning with fresh credentials.
The receiver must resolve an existing authorized binding before decoding a frame.
It must not create bindings from received identifiers.

One sample may be outstanding per node. Up to eight metrics fit in one DATA frame;
there is no fragmentation. Capacity depends on airtime and storage, not the size
of the identifier space.

## Wire format

All multibyte integers are big-endian. Serialize fields explicitly; never send a
native struct or rely on compiler padding. The 32-byte plaintext header is included
in full as authenticated additional data (AAD):

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `CJLR` |
| 4 | 1 | Version: `1` |
| 5 | 1 | Type: DATA=`1`, ACK=`2` |
| 6 | 8 | Network ID |
| 14 | 8 | Node ID |
| 22 | 8 | Sample counter, starting at 1 |
| 30 | 2 | Encrypted payload length |

The header is followed by the encrypted payload and a 16-byte GCM authentication
tag. Binary version 1 is incompatible with older text telemetry formats. Reject
unsupported versions/types, invalid lengths, trailing bytes and authentication
failures. Do not expose unauthenticated plaintext to the application. Input and
output objects passed to core APIs must be distinct.

DATA starts with battery voltage (u16 millivolts; zero means unknown), the expected
next reporting interval (u32 seconds, nonzero) and metric count (u8, 1–8). Each
metric occupies ten bytes:

| Bytes | Field |
| --- | --- |
| 2 | Nonzero sensor ID |
| 2 | Nonzero metric ID |
| 1 | Nonzero unit ID |
| 1 | Status: OK=`0`, error=`1`, skipped=`2` |
| 4 | Signed value in thousandths of the unit, two's-complement i32 |

A `(sensor, metric)` pair must not repeat within a frame. Error/skipped entries
must encode a zero value, which consumers must ignore based on status. A zero
value with OK status is a valid measurement. There is no wall-clock measurement
time in this format; an expected interval is not a timestamp.

Initial semantic registry: metric 1=temperature, metric 2=relative humidity;
unit 1=degrees Celsius, unit 2=percent relative humidity. The codec accepts other
positive IDs for extension; drivers and consumers must understand their meaning.
A physical connector does not imply automatic sensor discovery.

DATA occupies 65–135 bytes including header and tag; two metrics occupy 75 bytes.
ACK occupies 64 bytes. It carries the DATA sample counter and an encrypted payload
containing exactly the 16-byte tag of the accepted DATA frame. ACK has no variable
status or timestamp field. It confirms durable acceptance by the receiver, not
successful delivery to a server.

## Authenticated encryption and counters

AES-128-GCM with a full 128-bit tag is implemented through OpenSSL EVP on the host
and mbedTLS on ESP32. Provisioning must generate a unique binding key with a
cryptographically secure random generator. Never derive keys from device names or
IDs, share a global default key, or expose credentials in logs, source or radio
traffic.

The 12-byte nonce is `43 4a <type> 01` followed by the big-endian sample counter.
The type separates DATA and ACK nonces. Keys must be unique per binding. A counter
must never identify new content under an existing key. `seal` is a low-level
primitive; production senders must reserve counters durably through `Sender` and
`CounterStore` instead of choosing them directly.

`CounterStore::reserve` must atomically persist a monotonically increasing
reservation BEFORE encryption/transmission. Missing, corrupt or exhausted state,
or a failed write, must prevent transmission. Restarting or waking from sleep
must not reset counters while retaining a key. Block reservation may reduce flash
wear only if the reservation high-water mark is durable before any counter in the
block is used. Skipped counters are valid. New keys permit new counter state;
restoring an old key with reset counters is forbidden. The physical adapter is
not implemented yet.

Retries reuse the identical serialized frame. Duplicate ACKs have identical bytes.
Do not re-encrypt changed content with an existing counter. Adding variable ACK
fields would require a revised nonce/counter design. The protocol provides no
forward secrecy and does not protect against RF jamming or physical extraction
of credentials from unprotected storage. Administration and storage protection
remain integration responsibilities. A revoked binding has `active=false`.

## Durable acceptance and replay handling

After authenticating and validating DATA, the receiver loads the highest accepted
counter and the complete last accepted frame for the binding. `Journal::load`
must fail closed if state cannot be read. Only provisioning may create an empty
receipt state.

- Lower counter: reject as replay without ACK.
- Equal counter, identical frame: acknowledge again without enqueuing again.
- Equal counter, different frame: reject as a conflict without ACK.
- Higher counter: atomically commit both the queued sample and receipt, with the
  previous counter as a concurrency precondition. Generate ACK only after success.

A failed/full/conflicting commit must leave queue and receipt unchanged and must
not produce an acceptance ACK. Failure after commit but before ACK is recovered
by retrying the same DATA. Queue draining must retain the replay receipt. Restoring
older receipt state under the same key is unsafe; use fresh credentials if state
continuity cannot be trusted. The real adapter must uphold these guarantees across
power loss. In-memory unit tests do not prove that behavior.

An initial integration target is a bounded global queue of 128 DATA frames, without
overwriting unforwarded samples. This capacity is proposed, not implemented in the
core. Forwarding must use a stable identity including network, node, credential
generation, sample counter, sensor and metric. Delete queued samples only after
server acceptance. The server adapter is pending; avoid representing u64 IDs as
JSON numbers in consumers that lose integer precision.

## Sender and radio scheduling

`Sender` permits one pending frame and at most three attempts. It accepts an ACK
only when binding, counter and original DATA tag match. Starting another sample
while one is pending returns a conflict. Exhausting attempts does not imply
success or silently discard the frame. The caller may explicitly `abandon` after
the final ACK window closes and account for an unconfirmed sample.

Initial scheduling parameters to implement and measure: channel activity detection
before transmission, 0–500 ms initial jitter, 1,500 ms ACK timeout measured from the
end of transmission, 100–500 ms backoff after the first failure, and 200–1,000 ms
after the second. A 10-second overall send-cycle deadline must include busy-channel
waiting. These timers are not implemented in this core. Adjust them against radio
airtime and durable storage latency when selecting a radio profile. Activity
detection reduces collisions but cannot eliminate hidden nodes or interference.

After a cycle is exhausted, the initial integration policy is to count the sample
as unconfirmed and sleep until the next measurement. There is no planned persistent
sample backlog on the first node implementation. Restarting may lose a RAM-only
sample, but must not reuse its counter. Delivery is not guaranteed through long
outages. Frequency, bandwidth, power and regulatory configuration are responsibilities
of the radio integration, not properties enforced by this codec.

## USB provisioning contract

A local administrator selects the intended receiver and node by stable device
identity, rather than trusting a changing serial port name. The tool prepares a
network/radio profile and a new per-node key, persists pending binding state on
the receiver and configuration/counter state on the node, verifies both, and then
validates an authenticated radio exchange. Configuration success and a validated
link are separate states.

Partial setup must be resumable or replaced with fresh credentials consistently
on both devices. Never offer a reset operation that erases counters while retaining
the key. Serial commands, the provisioning tool, durable adapters and radio tests
remain unimplemented. There are no over-the-air enrollment messages in v1.

## Validation and references

[Tests](testing.md) exercise the host core and compile the same suite for ESP32.
The public fixtures use fixed keys solely as test vectors. Hardware execution,
power-loss testing, RF coexistence and independent security review remain pending.

- [NIST SP 800-38D: GCM](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf)
- [OpenSSL EVP](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/)
- [mbedTLS GCM](https://mbed-tls.readthedocs.io/projects/api/en/v2.28.9/api/file/gcm_8h/)
- [Unity](https://www.throwtheswitch.org/unity)
