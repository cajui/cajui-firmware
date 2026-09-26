# Home Assistant

A receiver announces itself and its transmitters to Home Assistant through
[MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery), so an
installation without Cajuí Central gets its entities without YAML. It needs only a broker
that both reach and the Home Assistant MQTT integration with its default discovery prefix,
`homeassistant`.

## What appears

| Device | Entity | Source |
| --- | --- | --- |
| Receiver | Wi-Fi signal, samples waiting, uptime (diagnostic) | its [management state](management-v1.md#state) |
| Transmitter | Temperature, humidity | forwarded [telemetry](radio-applications.md#forwarding-to-mqtt) |
| Transmitter | Signal strength, signal-to-noise ratio (diagnostic) | the receiver's measurement of each frame |

Transmitters are linked to their receiver (`via_device`). A transmitter's entities appear
after the broker acknowledged its first sample, with only the metrics that sample carried,
from the registry metrics temperature and humidity. Entities use the receiver's
availability topic, so they become unavailable when the receiver goes offline, and
`expire_after` of three expected intervals, as Central's silence alert. A reading with an
error, or a skipped one, is unknown, never zero.

## Topics

Configurations are retained on `homeassistant/sensor/<source_id>/<object_id>/config`, with
`<object_id>` built from the device ID and the entity, for example
`000048ca433c776c_temperature_1`. The receiver publishes them after each connection and
when a transmitter's interval changes. Removing a transmitter does not remove its
configurations; delete them in Home Assistant or clear the retained topics. The same
applies when a transmitter moves to another receiver or a receiver's broker user changes:
the configurations left under the old source share the entities' unique IDs, and Home
Assistant keeps whichever it reads first, so clear the old source's retained topics.

The source must use only letters, digits, `_` and `-`: Home Assistant rejects other
characters in this level, so a receiver whose broker user contains `.` or `:` publishes
no configurations (telemetry and state are unaffected).

Placing the source in Discovery's node level lets a broker confine each receiver to its
own configurations. For a receiver user `<u>`, next to the grants of the
[management channel](management-v1.md#permissions-and-trust):

```text
topic write homeassistant/sensor/<u>/+/config
```

Home Assistant's account needs `read` on `homeassistant/#`, `telemetry/v1/+/+/samples`,
`manage/v1/+/+/state` and `manage/v1/+/+/availability`. A broker without the write grant
drops the configurations (Mosquitto 2.0.22 acknowledges them anyway) and nothing else
changes.

## Limits

- Entity names are in English; rename them in Home Assistant.
- Only the registry's metrics are announced. Other metrics still arrive in telemetry.
- Checked against the Home Assistant MQTT Discovery documentation and on the bench: a
  receiver published all seven configurations to Mosquitto 2.0.22, and their value
  templates, rendered with Jinja2 over a real sample and state, gave the expected values.
  Not checked against a running Home Assistant installation.
