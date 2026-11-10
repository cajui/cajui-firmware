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
  loop and a five-minute transmitter sleep schedule.
- One transmitter image per board carrying every sensor driver (SHT4x, DHT22, analog
  voltage), with the attached sensors detected after power-on or reset and several
  sensors per transmitter ([sensors](radio-applications.md#sensors)).
- Receiver forwarding to an MQTT broker over Wi-Fi in Cajuí Central's JSON contract,
  removing each queued sample only after its PUBACK, with USB-provisioned settings.
- A receiver setup page on a temporary access point opened by holding the PRG button:
  Wi-Fi, MQTT broker (with mDNS discovery), status and transmitter revocation, applied
  without a reboot. The OLED shows a QR code to join the setup network.
- [Radio pairing](radio-pairing.md): add a transmitter from the setup page with an
  X25519/HKDF key exchange; the transmitter joins after a long PRG press or `CJ1 PAIR`.
- Unity tests, Python client tests, ASan/UBSan and coverage checks.

- Experimental XIAO ESP32-S3 + Wio-SX1262 B2B transmitter profile with the shared
  sensor drivers and explicit sleep-pin handling. See [XIAO](xiao-s3.md).

## Tooling validation (2026-11-09)

- Coverage tooling preserves Python 3.10 with coverage.py 7.16.2 and uses 7.17.0
  on Python 3.11 or newer. Regression tests keep the runner and requirement locks
  synchronized; unrelated dependency versions remain unchanged.
- Local validation passed: full lint, 98 Python tests both with and without coverage
  on Python 3.10.20 and 3.12.13, and 206 native tests with ASan/UBSan and LLVM coverage.
  All per-file coverage gates passed. The ESP32 test target compiled without upload
  or test execution; no physical devices were accessed.
- Integration with main `1e741ac` passed local lint with clang-tidy 23.1.4 and
  ShellCheck 0.11.0, 99 Python tests with coverage on both interpreters, and 206 native
  tests with sanitizers and all per-file coverage gates. The ESP32 protocol target
  and all five runtime profiles compiled; board lint passed for all five profiles.
  Builds did not upload firmware or execute tests on devices.
- Remote CI passed on correction `da343aa`: lint, native coverage, Python 3.10,
  fuzzing and ESP32 builds, including real release-image assembly. See
  [the CI run](https://github.com/cajui/cajui-firmware/actions/runs/42669406004).
  These checks do not publish releases or validate physical devices; the hardware
  milestones below remain unchanged.

## Pending and unvalidated

This page records implementation status and validation limits for the development
branch. Released images may have an earlier feature set.

- Run-time sensor detection is covered by host tests with driver doubles and compiles
  for every board. On one XIAO it detected a capacitive soil moisture sensor on the analog
  input after a reset, read it again after a deep-sleep wake and delivered through the
  receiver; SHT4x and DHT22 detection, a disconnected analog input and the other boards
  have not run on hardware. Analog readings are uncalibrated.
  Home Assistant Discovery still announces only the first temperature/humidity sensor of
  a transmitter.
- XIAO has passed initial pairing and acknowledged delivery to Heltec, but sensor
  integration and sleep current remain unvalidated. Battery voltage is unknown; low-battery scheduling cannot
  act on it. This profile is USB-only and excluded from signed releases.
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
