# MQTT management channel v1

Draft contract, not implemented yet. It lets a receiver report its own state and the
state of its transmitters, and lets an authorized MQTT client ask it for an action. It
sits next to the [telemetry contract](radio-applications.md#forwarding-to-mqtt), which it
does not change: a consumer that reads only telemetry keeps working.

Every function reachable through this channel stays available without it: Wi-Fi, broker
and pairing on the [setup page](radio-applications.md#receiver-setup-page), pairing by the
transmitter's button, and administration over [USB](provisioning.md). Any MQTT client with
the right permissions can use the channel; Cajuí Central is one of them.

## Topics

`<source_id>` is the receiver's MQTT username, as in telemetry. `<device_id>` is a device
ID as 16 lowercase hexadecimal digits: the receiver's own ID, or the node ID of one of
its transmitters.

| Topic | Publisher | QoS | Retain | Payload |
| --- | --- | --- | --- | --- |
| `manage/v1/<source_id>/<device_id>/availability` | receiver, for itself | 1 | yes | `online` or `offline` |
| `manage/v1/<source_id>/<device_id>/state` | receiver, for itself and each transmitter | 1 | yes | [state](#state) |
| `manage/v1/<source_id>/<device_id>/commands` | authorized client | 1 | no | [command](#commands) |
| `manage/v1/<source_id>/<device_id>/results` | receiver | 1 | no | [result](#results) |

The receiver sets `offline` as its MQTT last will and publishes `online` right after
connecting, before any state. These are the default payloads of Home Assistant's
`availability_topic`. A transmitter has no availability topic: its reachability is
judged from its samples, as today.

The receiver connects with a clean session and subscribes to
`manage/v1/<source_id>/+/commands` on every connection. Commands published while it is
offline are therefore dropped by the broker, never executed late; a client that gets no
result treats the command as not delivered.

## State

A retained JSON object that describes one device. The receiver publishes it after
connecting, whenever a discrete field changes, and at most once every 60 seconds for
counters (`uptime_s`, `wifi`, `queue`, `forwarding`). Node state is published after each
accepted DATA frame and when a binding is added or revoked. State lives in RAM: publishing
it never writes flash, and a publication lost while offline is simply replaced by the
next one.

Receiver:

```json
{
  "version": 1,
  "source_id": "receiver-1",
  "device_id": "0000000000005e10",
  "role": "receiver",
  "model": "heltec-wifi-lora-32-v3",
  "firmware": { "version": "0.2.0", "slot": "ota_0", "state": "valid" },
  "radio": { "profile": 1, "power_dbm": -9 },
  "uptime_s": 3600,
  "reset_reason": "power_on",
  "wifi": { "rssi_dbm": -61 },
  "queue": { "depth": 0, "capacity": 128 },
  "forwarding": { "published": 42, "retries": 1 },
  "pairing": { "open": false, "remaining_s": 0, "requests": [] },
  "capabilities": ["pairing", "revoke"]
}
```

Transmitter, published by its receiver:

```json
{
  "version": 1,
  "source_id": "receiver-1",
  "device_id": "00000000000000a1",
  "role": "transmitter",
  "receiver_id": "0000000000005e10",
  "binding": "active",
  "model": null,
  "firmware": null,
  "last_frame": { "counter": 124, "rssi_dbm": -82, "snr_db": 9.5, "receiver_uptime_s": 3590 },
  "parameters": { "interval_s": null, "power_dbm": null },
  "pending": []
}
```

Field rules:

- `null` means unknown, never zero. A transmitter's `model`, `firmware` and `parameters`
  stay `null` until the radio protocol carries them.
- `firmware.version` is `major.minor.patch`, or `0.0.0` for a local build. `slot` and
  `state` are those of the `CJAPP FIRMWARE` boot line.
- `reset_reason` is one of `power_on`, `software`, `panic`, `watchdog`, `brownout`,
  `deep_sleep`, `external` or `other`.
- `pairing.requests` lists the nodes asking to join during an open window, each as
  `{"node_id": "...", "rssi_dbm": -70, "conflict": false}`; see
  [radio pairing](radio-pairing.md#model-and-security).
- `binding` is `active` or `revoked`. A revoked transmitter keeps its retained state with
  `revoked` so that consumers learn about it; publishing an empty retained message later
  clears it.
- `capabilities` lists the command families the firmware accepts. A consumer offers only
  those, so an older firmware degrades instead of failing.
- The receiver has no wall clock, so state carries no timestamps. A consumer stamps its own
  receipt time; a retained message delivered on subscription (MQTT retain flag set) is
  last known state of unknown age. `uptime_s` and `receiver_uptime_s` reveal restarts.

Consumers must ignore unknown fields: new fields are added without changing `version`.
Removing or changing the meaning of a field requires `version` 2 on a new `manage/v2`
prefix.

## Commands

```json
{ "version": 1, "command_id": "c-7f3a", "type": "pairing.open", "params": {} }
```

`command_id` is 1–64 characters of `[A-Za-z0-9._:-]`, starting with a letter or digit,
unique per command. `params` is required, possibly empty. The payload is at most 512
bytes. The receiver parses strictly, unlike state consumers: unknown fields, duplicate
keys, wrong types and trailing data are rejected with `invalid`.

| Type | Topic device | Params | Effect |
| --- | --- | --- | --- |
| `pairing.open` | receiver | none | Opens the two-minute pairing window, as the setup page's "Search for transmitters". |
| `pairing.accept` | receiver | `node_id` | Sends the offer to a listed request, as the page's "Add". |
| `pairing.close` | receiver | none | Closes the window. |
| `node.revoke` | receiver | `node_id` | Revokes an enrolled transmitter, as the page's confirmed revoke. |

Reserved for later versions, rejected with `unsupported` until implemented:
`parameters.set` on a transmitter (`interval_s`, `power_dbm`), relayed in the receiver's
ACK as the [version 2 power command](protocol-v1.md#version-2-power-command) is, and
`firmware.install` on the receiver, which downloads a signed `.cjfw` file. Both are
specified with the protocol and update changes that carry them.

The receiver executes commands only while it is listening in operation, under the same
lock the setup page uses, so page and channel actions never interleave. A command that
arrives in admin mode is not received at all, because admin mode has no MQTT client.

## Results

```json
{ "version": 1, "command_id": "c-7f3a", "status": "applied", "reason": null }
```

| Status | Meaning |
| --- | --- |
| `applied` | Done. |
| `pending` | Accepted; a later result with the same `command_id` reports the outcome (`pairing.accept` while the node confirms). |
| `rejected` | Not executed; `reason` says why. |

Reasons: `invalid` (malformed command), `unsupported` (type not implemented),
`busy` (another pairing is in progress), `unknown_node`, `not_requested` (node not in
the window's requests), `conflict` (pinned key conflict), `timeout` (node did not
confirm), `storage` (the receiver could not persist the change).

The receiver keeps the results of its last 8 commands in RAM and answers a repeated
`command_id` with the stored result instead of executing it again, so a QoS 1 duplicate
is harmless. A command whose `command_id` cannot be read gets no result.

## Permissions and trust

MQTT 3.1.1 does not tell a subscriber who published a message. The broker's
authentication and ACL are therefore the whole authorization of a command: any client
allowed to write a receiver's `commands` topic can open pairing or revoke its
transmitters. Grant that write only to trusted operator clients.

ACL for a receiver user `<u>`, next to its telemetry grant:

```text
user <u>
topic write telemetry/v1/<u>/+/samples
topic write manage/v1/<u>/+/availability
topic write manage/v1/<u>/+/state
topic write manage/v1/<u>/+/results
topic read manage/v1/<u>/+/commands
```

A read-only consumer, such as a dashboard or Home Assistant account, gets `read` on
`manage/v1/+/+/availability` and `manage/v1/+/+/state`. An operator client adds
`read` on `manage/v1/+/+/results` and `write` on `manage/v1/+/+/commands`.

What a command cannot do bounds the damage of a leaked operator credential:

- It cannot read keys or credentials: no state field carries them.
- Opening pairing remotely removes the physical action on the receiver side, but a node
  still joins only after its own button is held, and appears in `pairing.requests` with
  its ID and signal strength until an operator accepts it. The
  [pairing security model](radio-pairing.md#model-and-security) otherwise applies
  unchanged, including its lack of authentication against an active attacker.
- Revoking is a denial of service that the owner undoes by pairing again.
- `firmware.install`, when added, still requires the release signature: the channel
  cannot install unsigned code.

Until broker TLS is provisioned, the channel crosses the local network in plain text
like telemetry; use a trusted network.

## Implementation order

1. The receiver publishes availability and state (read-only).
2. Consumers show it; Cajuí Central is the reference.
3. The receiver executes `pairing.*` and `node.revoke`; brokers add the operator grant.
4. The radio protocol carries transmitter descriptor and parameters; `parameters.set`.
5. `firmware.install` on the receiver.

Receiver onboarding without typing a broker credential is a separate design: the receiver
cannot reach the broker before it has a credential, so that path cannot run over this
channel.
