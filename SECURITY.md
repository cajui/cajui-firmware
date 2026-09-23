# Security

This is experimental protocol code, not an audited production firmware release.
No released version is currently recommended for security-critical deployment.

The protocol uses authenticated encryption, but end-to-end security also requires
correct credential provisioning, durable monotonic counters, atomic receipt/queue
storage and protected administration. Those runtime integrations remain pending.
Never reset counters while retaining a key, reuse a key across device bindings,
or treat a device ID as proof of identity. The protocol does not prevent jamming
or physical extraction of unprotected device storage.

Please use GitHub private vulnerability reporting through the repository Security
tab. Do not post credentials or exploitable vulnerability details in public issues.
