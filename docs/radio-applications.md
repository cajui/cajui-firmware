# Experimental radio applications

`runtime_tx` and `runtime_rx` target the Heltec WiFi LoRa 32 V3 (ESP32-S3/SX1262).
They are development images, not a Wireless Stick Lite port or a production release.
See the [current implementation status](../README.md#pending-and-unvalidated).

```sh
pio run -e runtime_tx -e runtime_rx
```

Building does not access devices. Uploads and RF tests are separate manual operations.
Identify each board by its stable device ID before choosing a role or upload port.
Both roles transmit: the receiver sends ACKs. Profile 1 uses 915.2 MHz, 125 kHz,
SF7, CR4/5, an eight-symbol preamble, private sync word 0x12, explicit headers and CRC.
Transmit power defaults to **−9 dBm**, a bench value; `CJ1 POWER` (or `provision.py
power`) stores another one per device, from −9 to +22 dBm, the SX1262 range, applied at
the next restart. Keep it within what the region and antenna allow: the firmware does not
know either. This is not regional auto-configuration, and minimum power does not make
operation without a suitable antenna safe.

Transmitters send version 2 DATA, so the receiver's ACK can command a transmit power
([protocol](protocol-v1.md#version-2-power-command)). The configured power is the
ceiling; a commanded power is kept in RTC memory across deep sleep, and three cycles
without an ACK, or a power cycle, return the node to the configured power. The receiver
does not command any change yet: the steps of a power policy are still to be defined
from field measurements. Update the receiver before its transmitters, and do not downgrade
a receiver without reading [the storage note](persistence.md#records-layout-v2).

## Enrollment and image changes

Load `runtime_tx` or `runtime_rx` and enroll with the [USB tool](provisioning.md) or by
[radio pairing](radio-pairing.md); a transmitter without enrollment boots in admin mode,
with the radio in reset, and a receiver without bindings starts listening. Holding PRG
for three seconds on a transmitter (at boot, when waking it from sleep, or in admin mode)
or sending `CJ1 PAIR` starts pairing: its LED blinks fast for up to two minutes, then it
restarts. PRG is a deep-sleep wake source for this reason. Both images share
`partitions.csv` and the same [storage records](persistence.md). Preserve the `cajui` NVS partition; never erase or restore older
counter/receipt state under an existing key. Keep the local recovery file private.
Radio operation requires healthy storage, enrollment and profile 1; the transmitter also
requires its own active binding. Otherwise the device boots in admin mode
(`CJAPP ADMIN reason=storage|not_enrolled|requested`).

The entry points are `src/transmitter_main.cpp` and `src/receiver_main.cpp`, one small
application class each; boot-mode selection and fault recovery are the unit-tested
functions of `lib/CajuiDevice`.

In operation the serial port carries diagnostic `CJAPP` text and a restricted CJ1
console. `CJ1 ADMIN` restarts into admin mode once the radio is idle: after the
transmitter's delivery cycle, or while the receiver is listening. There is no
concurrent USB credential mutation while a frame is in flight. ESP-IDF component logs are disabled at startup because
they are written from other tasks and can split a console reply. Only one `PersistentStore` owns
its backing store.

## Transmitter

The board application powers the DHT22 from Vext, allows 2.2 seconds to settle and
reads its data pin on GPIO47. The OLED is kept in reset. Sensor 1 supplies metric 1
(temperature, unit 1 = degrees Celsius) and metric 2 (humidity, unit 2 = percent).
Wire values are thousandths of those units. Invalid/non-finite/out-of-range readings
carry `Error` and zero payload, distinguishable from a valid zero measurement.
This is the initial application metric registry, not a restriction to those sensors
in the protocol.

Battery voltage is currently unknown (`batteryMv=0`). No battery cutoff policy is
implemented in this application; use USB while validating it. The schedule is
300 seconds from boot to the next wake, including sensor and delivery time. Each
wake mounts existing counters, reserves a new one and runs one bounded send cycle.
Unconfirmed samples are logged and not backlogged. Radio shutdown, Vext off and
GPIO holds precede deep sleep; a failed radio shutdown uses reset as a fallback.
A radio or storage fault during operation never erases enrollment and never leaves the
node awake: it
logs `CJAPP STOP <reason> faults=<n> retry_s=<s>` and deep-sleeps for 10 seconds,
doubling with each consecutive fault up to 15 minutes; the next wake retries. A
completed delivery cycle clears the count. Storage that cannot be mounted at boot is
different: the node stays awake in admin mode (`reason=storage`) for diagnosis over USB,
and never retries on its own, since remounting cannot repair it. The loop task runs under the ESP-IDF task
watchdog (5 seconds), so a hang restarts the node.

`CJAPP SAMPLE ... power=<dBm>` shows the power used for the cycle, and `CJAPP DELIVERY
... power_command=<dBm>` the command of the ACK (127: keep). `CJAPP DELIVERY completion=1`
means an authenticated matching ACK; other completion
values follow `Completion` in `cajui_runtime.h` and are unconfirmed. Power consumption,
GPIO hold behavior and actual sampling cadence require hardware measurement.

## Receiver

`ReceiverController` consumes at most one frame per poll. `untrustedDataNode` is only
a bounded routing hint to an already enrolled binding, never proof of identity.
The existing authenticated receive path commits the sample and replay receipt before
starting the ACK. The radio's RSSI and SNR of each accepted frame are logged
(`CJAPP ACCEPT ... rssi=<dBm> snr=<dB>`) and stored with the queued sample. Unknown,
revoked, corrupted, replayed or full-queue input receives
no acceptance ACK. A duplicate of the last committed sample gets the same ACK without
another queue entry, even if the queue is full, at most three times per node per minute:
a genuine node repeats a sample twice at most when its ACK is lost, and the bound keeps
a replayed frame from making the receiver transmit on demand.

ACK TX has a three-second watchdog; a completion that is polled late still counts.
Driver or storage failures latch the controller and stop the radio. The receiver then
logs `CJAPP STOP <reason> faults=<n> restart_s=<s>`, keeps the USB console available and
restarts after 10 seconds, doubling with each consecutive fault up to 15 minutes; ten
minutes of healthy operation clear the count. Restarting remounts storage, which fails
closed. The loop task runs under the ESP-IDF task watchdog (5 seconds); a watchdog or
panic restart happens at once but counts as a fault.
Queue entries survive image changes and resets. At five-minute intervals, an initially
empty queue holds 128 samples (about 10 hours 40 minutes from one transmitter). Never
interpret receiver ACK as delivery to Cajuí Central.

## Forwarding to MQTT

With [uplink settings](provisioning.md#receiver-uplink-settings) stored, `runtime_rx`
joins Wi-Fi and publishes the oldest queued sample to
`telemetry/v1/<username>/<node>/samples` with QoS 1 and retain off, using Cajuí Central's
JSON contract version 1. `device_id` is the node ID and `sample_id` is
`<generation>.<counter>`, so a republished sample keeps its identity and Central
deduplicates it. Metric 1/unit 1 map to `temperature`/`degC`, metric 2/unit 2 to
`humidity`/`%`; other registry entries are sent as `metric-<n>`/`unit-<n>`. Error and
skipped readings carry no value. The receiver's measurement of the frame follows as two
readings of a `radio` sensor, `rssi` in `dBm` and `snr` in `dB`, so Central's version 1
contract carries it unchanged; they are absent for samples queued before this was
measured. `measured_at` is omitted: the receiver does not know
when a queued sample was measured, so Central's receipt time for a backlog is the
forwarding time.

One publication is in flight at a time. The queue front is removed durably only after
the broker's PUBACK for that exact message. A missing PUBACK within 15 seconds or a
broker disconnection abandons the attempt and retries after five seconds; a late
PUBACK from an abandoned attempt is ignored. The ESP-IDF client enqueues the
publication so the radio loop does not wait for network I/O; it can wait for the client's
lock for at most the 2.5-second network timeout, below the loop watchdog, and forwarding runs only
while the receiver is listening, never during an ACK transmission. A storage failure
stops the radio and restarts the receiver after the fault delay described above. Without stored settings the receiver logs `CJAPP UPLINK disabled`
and keeps queueing as before.

PUBACK is the broker's boundary, not proof that Central stored the sample. The client
uses MQTT 3.1.1, where an ACL-denied publication is still acknowledged: a username
without write permission on its namespace would silently discard samples. Transport
is plain TCP, so Wi-Fi and broker credentials and samples are readable on the local
network; use a trusted network until broker TLS is provisioned. Each removal writes
only the 14-byte queue head record.
Diagnostic lines: `CJAPP UPLINK online|offline`, `CJAPP FORWARD puback total=<n>
queued=<n>` and `CJAPP FORWARD retry total=<n>`.

The same connection carries the [management channel](management-v1.md): a retained
`online`/`offline` availability (the last will, plus an explicit `offline` before new
broker settings or an update restart) and retained state for the receiver and each of its
transmitters. State is built in RAM and queued to the client without waiting, only while
the receiver is listening, so it never writes flash or delays an acknowledgement; its
PUBACKs are kept apart from the forwarding queue's. A transmitter's `last_frame` covers
frames accepted since the receiver started. A broker whose ACL lacks the management
topics drops them (Mosquitto 2.0.22) or refuses the connection; on a refusal as not
authorized a separate task reconnects without the will, and management publications stay
off until the next restart. Grant the receiver's user the topics listed in the contract.
Observed on a Heltec WiFi LoRa 32 V3 with Mosquitto 2.0.22: retained state for the receiver
and its transmitter after connecting, receiver state again a minute later, and the will's
`offline` about 90 seconds after the receiver was held in reset, then `online` on return.

## Receiver setup page

Holding the PRG button (GPIO0) for three seconds opens an access point named
`Cajui-XXXX`, from the last two bytes of the device ID, and a page at `192.168.4.1`.
A captive-portal DNS makes phones open it automatically; the OLED shows a Wi-Fi QR code
plus the network name and address, and the LED blinks while setup is open, for boards
without a display. The address is always `192.168.4.1`, the ESP32 access point default.
Holding the button again, the page's close button, ten minutes without requests, or 30
minutes after opening, whatever happens first, closes it. Final hardware can wire an external button to any
GPIO with a pull-up by changing `board::SetupButton`.

The page configures Wi-Fi (scanned 2.4 GHz networks or typed name) and the MQTT broker
(address discovered through mDNS `_mqtt._tcp`, or typed, plus username and password),
shows Wi-Fi, broker and queue status, and lists enrolled transmitters with their last
accepted counter and a confirmed revoke action. Settings are staged and saved to the same
uplink record as the USB commands only once both sections are complete **and** the
station has connected with the staged Wi-Fi credentials: a mistyped password is never
stored, and if it does not connect within 20 seconds the station returns to the saved
network and the page says so. Saving applies the settings without a reboot: forwarding
pauses, MQTT restarts with the new identity, and an in-flight publication is republished.
Saved passwords are never shown. An empty Wi-Fi password keeps the saved one for the same
network; an empty broker password keeps the saved one only for the same Wi-Fi network,
host, port and username, so the page can never send a stored password to another broker
or through another network, where the same host name could resolve elsewhere.

The page also adds transmitters by [radio pairing](radio-pairing.md): "Search for
transmitters" opens a two-minute window, requesting nodes are listed with their ID and
signal strength, and "Add" sends the offer; the result appears on the page. With
JavaScript, the transmitters section refreshes itself every second while the window is
open (spinners while searching and while waiting for confirmation) and the pairing buttons
act in place; without it, the forms reload the page.

The page answers only on the access point interface and only for its own address: a
request through the receiver's station address on the home network gets 404, for any
path, and a
request naming another host (a DNS-rebinding name, a phone's captive-portal probe) is
redirected to `192.168.4.1`. Each opening creates a random 128-bit session token that
every form carries; a POST without it, or with a browser `Origin` other than the page,
is refused, so another web page cannot submit the forms (CSRF). Scans and broker
discovery are POST actions too. Messages after an action are fixed texts chosen by a
code in the redirect, never text from a request.

**TODO (security): the access point is open, without a password.** While it is open,
anyone in range who joins it can use the page. It opens only by physical action, closes
after at most 30 minutes, and a per-device password on a label/QR is planned. HTTP is not
encrypted.

The Firmware section shows the installed version and installs a signed `.cjfw` update;
see [firmware updates](updates.md).

The page runs in its own FreeRTOS task, so a slow or idle HTTP client never delays radio
processing. It uses the store, pairing host and uplink record only while holding the
lock the radio loop holds around each poll; storage writes additionally wait until the
receiver is listening, so they never delay an acknowledgement being transmitted, and the
MQTT client is replaced outside the lock. Wi-Fi scans and mDNS discovery never overlap, because a scan
hops channels and drops mDNS traffic; a scan that starts while the station connects
aborts the connection, so scans wait for it. With the access point active, a scan takes
longer than the Arduino library's 6-second limit, so results are awaited up to 15 seconds.
These behaviors were observed on a Heltec WiFi LoRa 32 V3, not derived from documentation.
Diagnostic lines start with `CJAPP SETUP`.

## Radio adapter

The adapter pins RadioLib 7.1.2. A DIO1 ISR records the millisecond timestamp and wakes
one FreeRTOS service task. SPI runs only in task context, serialized by a mutex.
TX-done handling finishes TX and immediately starts continuous RX before publishing
completion to the controller. The controller's ACK window uses the ISR timestamp,
not the time at which the application polls. This avoids depending on application
polling to arm RX; task/SPI latency still requires measurement on hardware.

Incoming packets are bounded to `MaxFrame`. RX is stopped before querying length and
reading the FIFO to prevent length changes during a read. Oversized/empty/CRC-failed
packets are discarded. One pending frame is buffered; extra arrivals are dropped
and rely on sender retries. Operations clear stale hardware flags through RadioLib;
a queued notification is accepted only when the IRQ matches the current operation.

CAD and transmission are asynchronous. SPI command setup and mutex acquisition can
still wait for the driver's bounded BUSY handling; this is not a hard real-time
latency guarantee. Host deadline tests do not simulate that delay, flash cache stalls,
IRQ latency or packet collisions. Driver failures remain latched until reboot.

Implementation reference: [RadioLib SX126x at the pinned 7.1.2 release](https://github.com/jgromes/RadioLib/blob/7.1.2/src/modules/SX126x/SX126x.cpp).
