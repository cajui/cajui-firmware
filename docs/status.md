# Implementation status

[Back to Cajuí Firmware](../README.md)

Experimental development firmware. Host tests and compilation do not establish
field reliability. No production image is released.

## Implemented

- Bounded DATA/ACK frames with up to eight sensor metrics and AES-128-GCM.
- Per-device credentials, replay/duplicate handling and bounded sender attempts.
- Versioned per-record storage with an ESP32 NVS adapter, durable counter reservation,
  a 128-frame receiver queue committed before ACK in two small writes per sample,
  reusable enrollment slots and migration from the earlier single-snapshot layout.
- Wire version 2: the ACK can command the transmitter's power (clamped to its configured
  maximum, with a fallback after missed ACKs); version 1 nodes keep working. No power
  policy is enabled yet. Transmit power is configurable per device over USB.
- Radio RSSI/SNR of each accepted frame, logged and forwarded as `radio` readings.
- Two application slots with rollback: a browser installer over USB for released images,
  and signed updates installed from the receiver's setup page ([updates](updates.md)).
- Two-phase USB enrollment, resumable setup, key rotation, revocation and leaving a
  network (retired keys can never return).
- A local Python tool with private recovery files and a software-restart check.
- A nonblocking send controller with injected radio, clock and jitter, bounded
  channel waits, ACK deadlines and cancellation.
- SX1262 CAD/TX/RX adapter with task-driven interrupt handling, a durable receiver
  loop, DHT22 sampling and a five-minute transmitter sleep schedule.
- Receiver forwarding to an MQTT broker over Wi-Fi in Cajuí Central's JSON contract,
  removing each queued sample only after its PUBACK, with USB-provisioned settings.
- A receiver setup page on a temporary access point opened by holding the PRG button:
  Wi-Fi, MQTT broker (with mDNS discovery), status and transmitter revocation, applied
  without a reboot. The OLED shows a QR code to join the setup network.
- [Radio pairing](radio-pairing.md): add a transmitter from the setup page with an
  X25519/HKDF key exchange; the transmitter joins after a long PRG press or `CJ1 PAIR`.
- Unity tests, Python client tests, ASan/UBSan and coverage checks.

## Pending and unvalidated

This page records implementation status and validation limits for the development
branch. Released images may have an earlier feature set.

- Battery thresholds and the divider correction are provisional until checked with a real
  LiPo 1S; on USB without a battery the bench board read the charger's 4.2–4.3 V.
- Without uplink settings, or while the broker is unreachable,
  the receiver keeps its newest 128 samples and gives up the oldest ones (counted as
  `queue.dropped` in its state).
- **TODO (security): the setup access point is open.** Anyone within Wi-Fi range who
  joins it while it is open (at most 30 minutes, after a button press) can change the
  uplink settings, pair or revoke transmitters. The page itself refuses other origins,
  other hosts and the home-network interface, and never reveals or re-sends a stored
  password. A per-device password on a label/QR is planned.
- **Radio pairing is not authenticated against an active attacker** in radio range during
  the two-minute window; a per-device label code is planned. Signal strength is shown,
  not enforced.
- Broker TLS is not implemented. Forwarding uses plain MQTT 3.1.1 on a trusted
  network; PUBACK proves broker acceptance only. Central sends no application receipt.
- Physical validation is limited: a manual bench exchange achieved durable acceptance
  and authenticated ACKs on the first attempt, including sensor-error telemetry followed
  by valid climate readings on a subsequent boot. The queue survived that restart;
  one automatic five-minute wake also delivered valid readings and received an ACK.
  IRQ timing, loss/interference, sustained cadence and power use need measurement. CI only
  compiles the ESP32 targets; USB enrollment alone does not validate RF.
- Arbitrary power-loss behavior and flash endurance of the NVS adapter. Host tests inject
  storage failures; the adapter relies on NVS atomic blob replacement.
- RF coexistence, regulatory configuration and an independent security review.
