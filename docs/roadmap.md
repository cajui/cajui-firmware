# Integration roadmap

The shared protocol core is implemented and tested on the host. The same suite
compiles for ESP32, but has not been executed on that target as part of CI.

Next milestones:

1. Durable counter reservations and transactional receiver queue/receipt storage,
   including failure and restart tests against real adapters.
2. Local USB provisioning with unique credentials, explicit partial-setup recovery
   and revocation. Device IDs and port names are separate concerns.
3. Transmitter/receiver applications with radio access, timing, bounded retries,
   sensor reads and sleep. Add hardware-in-the-loop tests separately from host CI.
4. Idempotent forwarding to a monitoring server and queue backpressure handling.
5. Optional local web administration and authenticated radio pairing.

TDMA, mesh routing, radio firmware updates, actuator control and LoRaWAN mode are
outside the first direct-LoRa version. No deadline or field-readiness claim is
attached to these milestones.
