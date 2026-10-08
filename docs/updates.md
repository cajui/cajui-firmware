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

A maintainer runs `.github/workflows/release.yml` from `main`, supplying an existing
`vX.Y.Z` tag. Creating a tag alone does not publish a release. The workflow resolves
it to a commit, requires that commit to belong to the selected main revision, and
checks the latest main-branch CI run for that exact SHA. All five required jobs must
have succeeded; pending, failed, cancelled or skipped checks block the release.
The trusted main revision must also have passing CI if it differs from the tag.
The gate checks the latest execution of each job across attempts, so rerunning only
failed jobs preserves earlier successes without hiding a newer failure. It repeats
before signing, publishing and deploying Pages.

The firmware build checks out the validated SHA and exports only binary inputs:
bootloader, partition table, application and `boot_app0`. A separate, fresh assembly
runner checks out the trusted main SHA, validates the built table against its CSV,
and merges those inputs into an image per role. No tagged scripts execute there;
the installer template, assembly tools and dependency checksum come from main.
Images are published on GitHub Releases and GitHub Pages. [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
10.4.0 installs them from Chrome or Edge over Web Serial. The page serves ESP Web Tools
itself; the release job checks the npm tarball against its published SHA-512.

Release tooling reads `partitions.csv` for application limits, merge offsets and the
storage erase image. The built `partitions.bin`, including its format checksum, must
match that CSV. Invalid bounds, overlap, unsupported types/flags, or an image reaching
persistent storage stop assembly. The supported ESP32-S3 profile has 8 MiB of flash
and a partition table at 0x8000; these chip/profile parameters are not CSV partitions.
Offsets must be explicit; sizes accept hexadecimal, decimal, K and M notation.

```sh
gh workflow run release.yml --ref main -f tag=v1.2.3
```

Wait for CI on the main commit before dispatching. A failed gate exits without using
the signing key; rerun the release after the checks pass. Release tags must be immutable.
The tag's partition CSV and public key header must match the trusted main revision;
older layouts or keys require an explicit migration/release plan rather than mixing
old binaries with a new installer layout.

### Publication and recovery

A new dispatch refuses any tag that already has a release, including a draft, before
signing. The trusted preflight job has `contents: write` because the GitHub API
[only lists drafts for callers with push access](https://docs.github.com/en/rest/releases/releases#list-releases).
Build, assembly and signing retain read-only repository permissions. Its numeric version must exceed every published stable version; the `latest`
label is not the authority. Drafts and prereleases of other tags do not advance that
floor. A stable release with a noncanonical tag blocks publication for manual review.

Publication creates a draft with a marker identifying the workflow run and validated
commit. It uploads the four firmware assets and `SHA256SUMS`, then downloads them to
verify exact names, sizes and SHA-256 hashes before publishing and marking it latest.
Only the original run may resume its draft. Already published files are verified,
never replaced. This marker establishes workflow ownership, not cryptographic proof
against a maintainer who can edit releases.

- If upload or verification fails, use **Re-run failed jobs** on the original release
  workflow. Its publish job replaces incomplete draft assets using the same signed
  artifacts; it does not rebuild or sign again.
- If publication succeeded but the Pages artifact upload failed, rerun the failed job.
  It verifies the existing published files and retries the Pages artifact upload.
- If only Pages deployment failed, rerun that job. Its gate rechecks version eligibility,
  preventing an older run from replacing the installer after a newer stable release.
- Do not start another dispatch or rerun all jobs to repair an existing draft: the
  initial/signing gates deliberately refuse it. If original artifacts have expired,
  review and remove only the unpublished draft before starting a new dispatch.
  Published releases are not automatically deleted or overwritten.

These recovery paths are covered with simulated API and upload failures. They have
not been exercised against a production release or the real signing secret.

Each role has two buttons. **Update** writes the merged image from offset 0: bootloader,
partition table, the default `nvs` (Wi-Fi driver data only), `otadata` and `ota_0`. It
stops well before `cajui`, so enrollment, counters, uplink settings and queued samples stay.
**Install on a new board** also writes an erased image over `cajui`, so leftovers of the
firmware a board shipped with are never read as corrupt storage; on an enrolled board it
discards its keys for good. The erase image contains `0xff`, the erased-flash value
that NVS treats as empty pages. Neither ever erases the whole chip
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
without a cable; the transmitter confirms only after an authenticated ACK, the one proof
that its radio, keys and receiver work. Transmitter images are installed over USB, which
leaves them flashed rather than on probation, so this matters only for a future
over-the-air path. Any radio or randomness failure during a delivery cycle counts as a
fault and backs off, and never confirms. A
confirmation that fails is retried every minute. A restart before that, from a crash, the watchdog or a fault, makes the
bootloader return to the previous image. An image that boots into admin mode is not
confirmed either, so the next restart returns to the previous one. Boot logs show
`CJAPP FIRMWARE version=<n> slot=<ota_0|ota_1> state=<flashed|pending|valid>
rolled_back=<0|1>`.

Storage formats only move forward: if an update migrated records to a newer version
before being rolled back, the previous image refuses them (see
[persistence](persistence.md#records-layout-v2)).

### Keys

`src/board/release_key.h` holds the public key (DER SubjectPublicKeyInfo). The private
key, `FIRMWARE_SIGNING_KEY` (PEM), belongs only to the `release` environment, restricted
to the **main branch**, not release tags. Do not create a repository-level copy.

Signing runs on a fresh runner. Its checkout is pinned to the main SHA that dispatched
the workflow, independently of the release tag. It executes only that trusted revision's
packager and OpenSSL; downloaded firmware images are data. No tagged script executes in
the signing job. The key is exposed only to the signing step, removed from the child
process environment after writing a private temporary file, and cleaned up on failure.
Signed files are checked against the trusted public key before publication; a secret
that does not match the embedded public key would produce updates boards cannot accept. Build jobs
have neither the key nor a write token.

This trusts maintainers who can change main or environment settings. Passing CI does not
prove firmware is benign, and environment restrictions do not protect against a malicious
administrator. Protect main through review and restrict release dispatch privileges.
GitHub's [environment branch rules](https://docs.github.com/en/actions/reference/workflows-and-actions/deployments-and-environments)
are part of the security boundary: a YAML condition alone cannot stop an older or modified
tag workflow from requesting a secret if its ref is still allowed by the environment.

Repository setup:

- `release`: selected branches/tags, with exactly one **branch** rule `main`, plus the
  signing secret. Remove the previous `v*.*.*` tag rule before using this workflow.
- `github-pages`: branch rule `main`; remove the previous tag rule. Pages uses GitHub Actions.
- Release tags: prevent updates/deletions for `refs/tags/v*.*.*` with an active ruleset.
- Main: restrict writes/review workflow and signer changes as trusted release code.

Keep an offline key backup: without it, boards can only be updated over USB. To rotate
keys, plan the transition through firmware carrying the new key; changing a GitHub secret
does not change the keys already installed on devices.

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
