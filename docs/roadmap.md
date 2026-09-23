# Integration roadmap

The shared protocol core is implemented and tested on the host. The same suite
compiles for ESP32, but has not been executed on that target as part of CI.

Implemented in the current development branch: versioned atomic snapshots, an NVS
adapter, USB administration images, resumable local enrollment and a restart probe.
The authenticated codec and delivery code are separate, with a host-tested send
controller for channel checks, jitter, bounded retries and deadlines.
See [persistence](persistence.md) and [enrollment](provisioning.md) for constraints.

Next milestones:

1. Hardware power-cut and flash-endurance tests for the NVS adapter.
2. Concrete radio adapter and receiver loop, connecting the tested send controller
   to sensor reads and MCU sleep. Keep radio validation separate from USB setup.
3. Idempotent server forwarding and queue backpressure handling.
4. Optional local web administration and authenticated radio pairing.

TDMA, mesh routing, radio firmware updates, actuator control and LoRaWAN mode are
outside the first direct-LoRa version. No deadline or field-readiness claim is
attached to these milestones.
