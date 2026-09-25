// SPDX-License-Identifier: Apache-2.0
#include "cajui_firmware.h"
#include <cstring>

namespace cajui {
namespace {
constexpr uint8_t Magic[4] = {'C', 'J', 'F', 'W'};
constexpr uint8_t Format = 1;
constexpr char Context[] = "cajui-firmware-v1";
constexpr size_t FormatAt = 4, RoleAt = 5, ReservedAt = 6, VersionAt = 8, SizeAt = 12,
                 SignatureLengthAt = 16, SignatureAt = 17;
uint32_t read32(const uint8_t* p) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value = (value << 8) | p[i];
    return value;
}
} // namespace

uint32_t firmwareVersionCode(unsigned major, unsigned minor, unsigned patch) {
    constexpr unsigned Part = 100;
    return major * Part * Part + minor * Part + patch;
}
UpdateReceiver::UpdateReceiver(ImageSink& sink, const uint8_t* publicKey, size_t keySize,
                               UpdateRole role, uint32_t runningVersion, size_t maxImage)
    : sink_(sink), key_(publicKey), keySize_(keySize), maxImage_(maxImage), role_(role),
      running_(runningVersion) {}
UpdateStatus UpdateReceiver::fail(UpdateStatus status) {
    if (status_ == UpdateStatus::Receiving && headerUsed_ == UpdateHeaderSize) sink_.abort();
    status_ = status;
    return status_;
}
UpdateStatus UpdateReceiver::parseHeader() {
    const uint8_t* h = header_;
    const size_t signatureSize = h[SignatureLengthAt];
    if (std::memcmp(h, Magic, sizeof(Magic)) != 0 || h[FormatAt] != Format || h[ReservedAt] ||
        h[ReservedAt + 1] || !signatureSize || signatureSize > SignatureCapacity) {
        status_ = UpdateStatus::BadHeader;
        return status_;
    }
    // The padding after the signature is zero: one encoding per update file.
    for (size_t i = SignatureAt + signatureSize; i < UpdateHeaderSize; ++i)
        if (h[i]) {
            status_ = UpdateStatus::BadHeader;
            return status_;
        }
    if (h[RoleAt] != uint8_t(role_)) {
        status_ = UpdateStatus::WrongRole;
        return status_;
    }
    version_ = read32(h + VersionAt);
    imageSize_ = read32(h + SizeAt);
    // A local build (version 0) accepts any signed image; a release refuses older ones.
    if (running_ && version_ < running_) {
        status_ = UpdateStatus::Downgrade;
        return status_;
    }
    if (!imageSize_ || imageSize_ > maxImage_) {
        status_ = UpdateStatus::TooLarge;
        return status_;
    }
    if (!hash_.update(reinterpret_cast<const uint8_t*>(Context), sizeof(Context) - 1) ||
        !hash_.update(h, SignedHeaderSize) || !sink_.begin(imageSize_)) {
        status_ = UpdateStatus::WriteError;
        return status_;
    }
    return status_;
}
UpdateStatus UpdateReceiver::feed(const uint8_t* data, size_t size) {
    if (status_ != UpdateStatus::Receiving) return status_;
    if (size && !data) return fail(UpdateStatus::WriteError);
    while (size && headerUsed_ < UpdateHeaderSize) {
        header_[headerUsed_++] = *data++;
        --size;
        if (headerUsed_ == UpdateHeaderSize && parseHeader() != UpdateStatus::Receiving)
            return status_;
    }
    if (!size) return status_;
    if (size > imageSize_ - written_) return fail(UpdateStatus::TooLarge);
    if (!hash_.update(data, size) || !sink_.write(data, size))
        return fail(UpdateStatus::WriteError);
    written_ += size;
    return status_;
}
UpdateStatus UpdateReceiver::finish() {
    if (status_ != UpdateStatus::Receiving) return status_;
    if (headerUsed_ < UpdateHeaderSize || written_ != imageSize_)
        return fail(UpdateStatus::Truncated);
    uint8_t digest[Sha256Size]{};
    if (!hash_.finish(digest)) return fail(UpdateStatus::WriteError);
    if (!verifyP256(key_, keySize_, digest, header_ + SignatureAt, header_[SignatureLengthAt]))
        return fail(UpdateStatus::BadSignature);
    if (!sink_.commit()) return fail(UpdateStatus::WriteError);
    status_ = UpdateStatus::Installed;
    return status_;
}
void UpdateReceiver::cancel() {
    if (status_ == UpdateStatus::Receiving) fail(UpdateStatus::Truncated);
}
const char* updateStatusName(UpdateStatus status) {
    switch (status) {
    case UpdateStatus::Receiving: return "receiving";
    case UpdateStatus::Installed: return "installed";
    case UpdateStatus::BadHeader: return "bad_header";
    case UpdateStatus::WrongRole: return "wrong_role";
    case UpdateStatus::Downgrade: return "downgrade";
    case UpdateStatus::TooLarge: return "too_large";
    case UpdateStatus::Truncated: return "truncated";
    case UpdateStatus::BadSignature: return "bad_signature";
    case UpdateStatus::WriteError: return "write_error";
    }
    return "unknown";
}
} // namespace cajui
