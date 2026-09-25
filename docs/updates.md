# Firmware updates

Two ways to install firmware: over USB from the web installer (any board, new or updated),
and a signed update file installed from the receiver's setup page (no computer needed).
Both keep the `cajui` storage partition: enrollment, counters and queued samples survive.

## Partition layout

`partitions.csv` has two application slots and `otadata`, which selects the one to boot:

| Name | Offset | Size |
| --- | --- | --- |
| `nvs` | 0x9000 | 0x5000 |
| `otadata` | 0xe000 | 0x2000 |
| `ota_0` | 0x10000 | 3 MiB |
| `cajui` | 0x310000 | 256 KiB |
| `ota_1` | 0x350000 | 3 MiB |

The `cajui` partition kept its offset from the earlier single-slot layout, so moving to
this layout over USB preserves it. The move was verified on two Heltec boards: both kept
their enrollment and the transmitter's counters continued. A USB install always writes
`ota_0` and resets `otadata`, so the board boots what was just written.

## Web installer (USB)

A tag `vX.Y.Z` runs `.github/workflows/release.yml`: it builds both images with that
version, merges bootloader, partition table, `otadata` and application into one image per
role, and publishes them on the GitHub release and the project's GitHub Pages site, where
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) 10.4.0 installs them from Chrome
or Edge over Web Serial. The page serves ESP Web Tools itself; the release job checks the
npm tarball against its published SHA-512.

Each role has two buttons. **Update** writes the merged image from offset 0: bootloader,
partition table, the default `nvs` (Wi-Fi driver data only), `otadata` and `ota_0`. It
stops well before `cajui`, so enrollment, counters, uplink settings and queued samples stay.
**Install on a new board** also writes an erased image over `cajui`, so leftovers of the
firmware a board shipped with are never read as corrupt storage; on an enrolled board it
discards its keys for good. Neither ever erases the whole chip
(`new_install_prompt_erase: false`).

## Signed updates (setup page)

The setup page's Firmware section accepts a `.cjfw` file:

```text
"CJFW" | format 1 | role 1 (1 tx, 2 rx) | reserved 2 (zero) | version u32 | size u32
       | signature length u8 | DER ECDSA P-256 signature, zero-padded to 72 | image
```

The signature covers `"cajui-firmware-v1"`, the first 16 bytes and the image. The receiver
writes the image into the slot that is not running while it arrives, hashing it, and only
when the whole file verified against the release key compiled into the firmware does it
validate the image (ESP-IDF checks format and checksum) and select it for the next boot.
It refuses another role, a file that is not canonical, and a version older than the running
one; a local build (version 0) accepts any signed version. After one install nothing else is
written until the receiver has restarted into it: the free slot is then the one selected for
boot, and writing into it again, even a file that fails verification, would leave those
bytes bootable. A write that fails also returns the boot selection to the running image.
The receiver restarts as soon as the install completes, even if the page's response never
reached the phone. Samples keep queueing during the
upload, though flash writes can delay a few acknowledgements. After the page confirms, the
receiver restarts into the update once its radio is idle.

Numeric versions are `major*10000 + minor*100 + patch` (`CAJUI_FIRMWARE_VERSION`, set by
`scripts/firmware_version.py` from the environment; releases set it from the tag).

### Rollback

A new image boots on probation. The receiver confirms it after one minute of healthy
operation, and only if its setup page started, since the page is its only update channel
without a cable; the transmitter confirms after its first complete delivery cycle. A
confirmation that fails is retried every minute. A restart before that, from a crash, the watchdog or a fault, makes the
bootloader return to the previous image. An image that boots into admin mode is not
confirmed either, so the next restart returns to the previous one. Boot logs show
`CJAPP FIRMWARE version=<n> slot=<ota_0|ota_1> state=<flashed|pending|valid>
rolled_back=<0|1>`.

Storage formats only move forward: if an update migrated records to a newer version
before being rolled back, the previous image refuses them (see
[persistence](persistence.md#records-layout-v2)).

### Keys

`src/board/release_key.h` holds the public key (DER SubjectPublicKeyInfo) that release
images must be signed with. The private key, `FIRMWARE_SIGNING_KEY` (PEM), is a secret of
the `release` environment, which only `v*.*.*` tags can use. Only the sign job reads it: it
runs nothing but openssl and the standard-library packaging tool, then checks every signed
file against `release_key.h`, so a mismatched secret fails the release instead of
publishing updates no board accepts. The build job, which runs third-party build code, has
neither the key nor a write token. Nobody else can produce an update the receiver accepts. Keep an offline backup: without it, boards can only be updated
over USB. To replace the key, generate a pair, update the header and the secret, and
release; boards accept keys only through a firmware that already carries them.

Repository setup, once: the `release` environment with a `v*.*.*` tag rule and the secret,
and GitHub Pages set to "GitHub Actions" with a `v*.*.*` tag rule on the `github-pages`
environment (by default it only accepts the default branch).

```sh
python3 tools/package_firmware.py generate-key --private key.pem --public key.der
python3 tools/package_firmware.py key-header --public key.der --output src/board/release_key.h
python3 tools/package_firmware.py package --image .pio/build/runtime_rx/firmware.bin \
  --role rx --version 1.2.3 --key key.pem --output cajui-receiver-1.2.3.cjfw
python3 tools/package_firmware.py verify --key-header src/board/release_key.h \
  cajui-receiver-1.2.3.cjfw
```

Version `0.0.0` is refused: zero marks a local build.

## Not provided

- Updates over Wi-Fi for the transmitter, which never starts Wi-Fi in normal use; update it
  over USB. A button-triggered update mode is a possible extension.
- Download from Cajuí Central or the internet; the receiver installs only uploaded files.
- Updates over LoRa.
- Anti-rollback in eFuses or Secure Boot: a signed older release cannot be installed from
  the page, but physical USB access can always write any image.
