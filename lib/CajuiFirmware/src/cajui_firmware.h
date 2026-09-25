// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "cajui_protocol.h"
#include <cstddef>
#include <cstdint>

// Signed firmware updates, docs/updates.md. Board-independent: the image goes to an
// ImageSink, and nothing is made bootable until the whole file verified.
namespace cajui {
// magic "CJFW" 4, format 1, role 1, reserved 2 (zero), version 4, image size 4 (these 16
// bytes are signed), signature length 1, DER signature padded to 72, then the image.
constexpr size_t SignedHeaderSize = 16, SignatureCapacity = 72;
constexpr size_t UpdateHeaderSize = SignedHeaderSize + 1 + SignatureCapacity;
enum class UpdateRole : uint8_t { Transmitter = 1, Receiver = 2 };
// Numeric version: major*10000 + minor*100 + patch. Zero marks a local build.
uint32_t firmwareVersionCode(unsigned major, unsigned minor, unsigned patch);

class ImageSink {
public:
    virtual ~ImageSink() = default;
    virtual bool begin(size_t size) = 0;
    virtual bool write(const uint8_t* data, size_t size) = 0;
    // Called only after the signature verified: validate the image and make it bootable.
    virtual bool commit() = 0;
    virtual void abort() = 0;
};
enum class UpdateStatus {
    Receiving,
    Installed,
    BadHeader,
    WrongRole,
    Downgrade,
    TooLarge,
    Truncated,
    BadSignature,
    WriteError
};
// Consumes an update file in chunks of any size. The image is written while it arrives,
// hashed together with the signed header, and committed only when the signature over
// both matches the release key. Any error aborts the sink and is final.
class UpdateReceiver {
public:
    UpdateReceiver(ImageSink&, const uint8_t* publicKey, size_t keySize, UpdateRole role,
                   uint32_t runningVersion, size_t maxImage);
    UpdateReceiver(const UpdateReceiver&) = delete;
    UpdateReceiver& operator=(const UpdateReceiver&) = delete;
    UpdateStatus feed(const uint8_t* data, size_t size);
    // All bytes were delivered: verify and commit.
    UpdateStatus finish();
    // The transfer stopped (client gone, page closed): abort unless already installed.
    void cancel();
    UpdateStatus status() const { return status_; }
    uint32_t version() const { return version_; }

private:
    ImageSink& sink_;
    const uint8_t* key_;
    size_t keySize_, maxImage_;
    UpdateRole role_;
    uint32_t running_, version_ = 0;
    uint8_t header_[UpdateHeaderSize]{};
    size_t headerUsed_ = 0, imageSize_ = 0, written_ = 0;
    Sha256 hash_;
    UpdateStatus status_ = UpdateStatus::Receiving;
    UpdateStatus fail(UpdateStatus);
    UpdateStatus parseHeader();
};
const char* updateStatusName(UpdateStatus);
} // namespace cajui
