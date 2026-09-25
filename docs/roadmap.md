# Integration roadmap

The shared protocol core is implemented and tested on the host. The same suite
compiles for ESP32, but has not been executed on that target as part of CI.

Implemented in the current development branch: versioned per-record storage with
bounded writes per sample, an NVS adapter, a USB administration mode in each image, resumable local enrollment and a restart probe.
The authenticated codec and delivery code are separate, with a host-tested send
controller for channel checks, jitter, bounded retries and deadlines.
See [persistence](persistence.md) and [enrollment](provisioning.md) for constraints.

Next milestones:

1. Hardware power-cut and flash-endurance tests for the NVS adapter.
2. Validate the experimental radio applications on hardware: IRQ/ACK timing, sensor
   reads, sleep, interference and battery policy. Keep RF validation separate from USB setup.
3. Broker TLS for receiver forwarding and field validation of the MQTT uplink.
   Idempotent forwarding with PUBACK-gated queue removal is implemented.
4. Authenticating radio pairing with a per-device label code, and a per-device password
   for the setup access point. Radio pairing itself and the setup page are implemented.

TDMA, mesh routing, radio firmware updates, actuator control and LoRaWAN mode are
outside the first direct-LoRa version. No deadline or field-readiness claim is
attached to these milestones.
