# Radio pairing

Draft extension of [protocol v1](protocol-v1.md); it has not undergone an independent
security audit. It enrolls a node over the radio instead of USB. USB enrollment remains
available and produces the same bindings.

## Model and security

Pairing follows the "permit join" model: the receiver accepts join requests only while
an administrator has opened a two-minute window on its setup page, and the node sends
them only after its button is held. The administrator sees each requesting node's ID and
signal strength and explicitly adds one.

Both sides generate ephemeral X25519 key pairs and derive the binding key from the shared
secret. A passive listener cannot compute the key. The exchange is **not authenticated
against an active attacker** in radio range during the window: the node has no display
on which to compare a code, so an attacker who answers first could pair with either side.
Mitigations are the physical action on both devices, the short window, the displayed node
ID and signal strength, radio proximity, and key pinning: the first key heard for a node ID
is kept for the whole window, and a second key or attempt nonce for that ID blocks adding
it (see step 2). An attacker can therefore make pairing fail, as jamming would, but cannot
silently take the place of a listed node once the node itself has been heard. A per-device secret printed on a label can
later authenticate the exchange; it is not part of this version.

Cryptography uses established libraries: X25519 through mbedTLS on ESP32 and OpenSSL on
the host, HKDF-SHA256 through OpenSSL on the host, and the existing AES-128-GCM adapter.
The prebuilt Arduino-ESP32 mbedTLS is compiled without HKDF (neither `mbedtls_hkdf` nor
PSA HKDF is available), so on ESP32 the RFC 5869 extract and expand steps are composed
from the library's HMAC-SHA256. Both X25519 and HKDF were checked against the RFC 7748 and
RFC 5869 test vectors on a Heltec board; the shared test suite checks them on the host.

## Wire format

Pairing frames reuse the 32-byte v1 header. `counter` carries the **attempt nonce**, a
random nonzero u64 chosen by the node for one pairing attempt; it is not a sample counter.

| Type | Name | Direction | network | Payload | Tag | Size |
| --- | --- | --- | --- | --- | --- | --- |
| 3 | JOIN_REQUEST | node → receiver | 0 | node public key (32) | none | 64 |
| 4 | JOIN_OFFER | receiver → node | receiver network | receiver public key (32), receiver ID (8), generation (8), profile (2) | yes | 98 |
| 5 | JOIN_CONFIRM | node → receiver | receiver network | empty | yes | 48 |
| 6 | JOIN_DONE | receiver → node | receiver network | empty | yes | 48 |

`node` is always the joining node's ID. JOIN_REQUEST is unauthenticated and must be
treated as untrusted input. The tagged frames are AES-128-GCM with an empty plaintext:
the tag authenticates the header and payload as additional data and proves possession of
the derived key. Their nonce is `43 4a <type> 01` followed by the attempt nonce. Types 4–6
never occur under a binding key's DATA (1) or ACK (2) nonces, and each key is fresh per
attempt, so nonces are never reused.

## Key derivation

```text
shared = X25519(own private key, peer public key)      -- rejected if all zero
key    = HKDF-SHA256(ikm = shared, salt = "cajui-pair-v1",
                     info = network || receiver || node || generation
                            || node public key || receiver public key)  -- 16 bytes
```

Identifiers are big-endian u64. Binding the identifiers and both public keys into the
key means a changed offer cannot be confirmed. The derived key becomes the binding key
of a new credential generation, exactly as a USB enrollment would store it.

## Exchange

1. The node sends JOIN_REQUEST, then listens 1.5 s for an offer; it repeats every 2 s with
   jitter for up to two minutes.
2. While the window is open, the receiver lists requesting nodes: the first four of the
   window, with the latest signal strength. Later requesters are ignored rather than
   evicting a listed node. The first public key and attempt nonce heard for a node ID are
   pinned; a request for that ID with another key or nonce marks it as a **conflict**,
   and the page shows it without an Add button until the window is opened again. When
   the administrator adds a node, the receiver refuses if the node is in conflict or no
   enrollment slot is free, chooses a random generation (and a random network if it has
   none), derives the key and keeps the offer **in memory only**, answering the node's
   next request with JOIN_OFFER. A repeated request with the pinned nonce receives the
   identical offer. A conflicting request for the offered node withdraws the offer before
   either device can confirm it. A node that restarted its attempt has a new key and
   nonce, so it conflicts with itself: the administrator stops and searches again.
3. The node validates the offer (its own ID and attempt nonce, nonzero identifiers,
   profile 1, a receiver ID different from its own, a valid tag under the derived key) and
   checks that its storage can accept it (a free slot, and the same network and receiver
   if it already has one). It then sends JOIN_CONFIRM and listens 1.5 s for JOIN_DONE, up
   to five times, still without storing anything.
4. On a valid confirmation the receiver stores the binding already active and answers
   JOIN_DONE. It keeps that reply apart from any new offer, so a repeated confirmation
   receives the identical reply even after the window closed or another node was added,
   for 15 seconds and five replies in total: enough for a node's five confirmations, and
   no more for someone replaying a recorded one.
   On JOIN_DONE the node stores and activates its binding and restarts into operation.

Enrollment slots are never freed, so nothing is stored for an attempt that has not been
confirmed: abandoned, expired, stopped or spoofed attempts cost no slot. Each successful
pairing uses one slot on each side, like a USB rotation. If JOIN_DONE is lost for all five
confirmations, the receiver holds an active binding the node never stored; the node keeps
its previous binding and the administrator pairs it again or revokes the stale one on the
setup page.

A node that already belongs to a network can only pair again within that network:
storage holds a single network per device, and there is no reset that would keep keys.
Pairing again within the network creates a new generation and revokes the previous one
on both sides, as USB rotation does.

## Devices

The node enters pairing mode when its button (PRG) is held for three seconds, at boot,
while waking, or in admin mode; its LED blinks quickly while pairing. The receiver
handles pairing frames in its normal receive loop, so it keeps accepting DATA from other
nodes during the window. See [radio applications](radio-applications.md).
