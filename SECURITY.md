# Security

This is experimental protocol code, not an audited production firmware release.
No released version is currently recommended for security-critical deployment.

The protocol uses authenticated encryption, but end-to-end security also requires
correct credential provisioning, durable monotonic counters, atomic receipt/queue
storage and protected administration. See the README for what remains unvalidated
before trusting any of these properties in the field. USB is trusted local
administration; keys are not encrypted at rest by this development build. Recovery
files must remain private. Never reset counters while retaining a key, reuse a key
across device bindings, or treat a device ID as proof of identity. The protocol does
not prevent jamming or physical extraction of unprotected device storage.

Please use GitHub private vulnerability reporting through the repository Security
tab. Do not post credentials or exploitable vulnerability details in public issues.

The send controller bounds each ACK window and the complete cycle, including busy
channel time. Invalid packets cannot extend those deadlines. Driver/clock/jitter
adapters remain a trusted integration boundary; the current tests use fakes.
A failed radio shutdown blocks another cycle and requires explicit driver recovery.
See [adapter contracts](docs/runtime.md) before integrating hardware.
