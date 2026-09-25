// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cstring>
#include <vector>
#include "assertions.h"
#include "cajui_firmware.h"
#include "firmware_fixture.h"

namespace {
using namespace cajui;
using fixtures::FirmwareTestKey;
using fixtures::FirmwareTestUpdate;
constexpr size_t ImageSize = 300, MaxImage = 0x300000;

class MemorySink final : public ImageSink {
public:
    std::vector<uint8_t> image;
    size_t expected = 0;
    bool begun = false, committed = false, aborted = false, failBegin = false, failWrite = false,
         failCommit = false;
    bool begin(size_t size) override {
        begun = true;
        expected = size;
        return !failBegin;
    }
    bool write(const uint8_t* data, size_t size) override {
        image.insert(image.end(), data, data + size);
        return !failWrite;
    }
    bool commit() override {
        committed = !failCommit;
        return committed;
    }
    void abort() override { aborted = true; }
};
std::vector<uint8_t> update() {
    return std::vector<uint8_t>(FirmwareTestUpdate,
                                FirmwareTestUpdate + sizeof(FirmwareTestUpdate));
}
UpdateStatus deliver(UpdateReceiver& receiver, const std::vector<uint8_t>& bytes, size_t chunk) {
    for (size_t at = 0; at < bytes.size(); at += chunk) {
        const size_t size = bytes.size() - at < chunk ? bytes.size() - at : chunk;
        if (receiver.feed(bytes.data() + at, size) != UpdateStatus::Receiving)
            return receiver.status();
    }
    return receiver.finish();
}

void test_signed_update_installs_in_any_chunking() {
    TEST_ASSERT_EQUAL_size_t(UpdateHeaderSize + ImageSize, sizeof(FirmwareTestUpdate));
    for (size_t chunk : {size_t(1), size_t(7), size_t(89), size_t(1024)}) {
        SCENARIO(chunk);
        MemorySink sink;
        UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey),
                                UpdateRole::Receiver, firmwareVersionCode(1, 2, 3), MaxImage);
        EXPECT_RESULT(UpdateStatus::Installed, deliver(receiver, update(), chunk));
        TEST_ASSERT_TRUE(sink.committed);
        TEST_ASSERT_FALSE(sink.aborted);
        TEST_ASSERT_EQUAL_size_t(ImageSize, sink.expected);
        TEST_ASSERT_EQUAL_MEMORY(FirmwareTestUpdate + UpdateHeaderSize, sink.image.data(),
                                 ImageSize);
        TEST_ASSERT_EQUAL_UINT32(10203, receiver.version());
        EXPECT_RESULT(UpdateStatus::Installed, receiver.feed(update().data(), 1)); // Final.
        receiver.cancel();
        EXPECT_RESULT(UpdateStatus::Installed, receiver.status());
    }
    TEST_ASSERT_EQUAL_STRING("installed", updateStatusName(UpdateStatus::Installed));
}
void test_every_signed_byte_is_protected() {
    // Header fields, signature and image: any change is refused and nothing is committed.
    const size_t offsets[] = {0, 4, 5, 6, 8, 11, 12, 15, 16, 17, 40, 88, 89, 200, 388};
    for (size_t offset : offsets) {
        SCENARIO(offset);
        auto bytes = update();
        bytes[offset] ^= 0x01;
        MemorySink sink;
        UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey),
                                UpdateRole::Receiver, 0, MaxImage);
        const auto status = deliver(receiver, bytes, 64);
        TEST_ASSERT_TRUE(status != UpdateStatus::Installed);
        TEST_ASSERT_FALSE(sink.committed);
        TEST_ASSERT_EQUAL(sink.begun, sink.aborted); // A started image is always aborted.
    }
}
void test_role_version_size_and_transport_failures() {
    struct Case {
        UpdateRole role;
        uint32_t running;
        size_t maxImage;
        UpdateStatus expected;
    };
    const Case cases[] = {
        {UpdateRole::Transmitter, 0, MaxImage, UpdateStatus::WrongRole},
        {UpdateRole::Receiver, firmwareVersionCode(1, 2, 4), MaxImage, UpdateStatus::Downgrade},
        {UpdateRole::Receiver, firmwareVersionCode(1, 2, 3), MaxImage, UpdateStatus::Installed},
        {UpdateRole::Receiver, 0, ImageSize - 1, UpdateStatus::TooLarge},
    };
    for (const auto& c : cases) {
        SCENARIO(int(c.expected));
        MemorySink sink;
        UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey), c.role, c.running,
                                c.maxImage);
        EXPECT_RESULT(c.expected, deliver(receiver, update(), 50));
        TEST_ASSERT_EQUAL(c.expected == UpdateStatus::Installed, sink.begun);
    }
    // Truncated, extended, cancelled mid-way, and a sink that fails at each step.
    for (int scenario = 0; scenario < 7; ++scenario) {
        SCENARIO(scenario);
        auto bytes = update();
        MemorySink sink;
        sink.failBegin = scenario == 3;
        sink.failWrite = scenario == 4;
        sink.failCommit = scenario == 5;
        UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey),
                                UpdateRole::Receiver, 0, MaxImage);
        UpdateStatus status = UpdateStatus::Receiving;
        if (scenario == 0) {
            bytes.pop_back();
            status = deliver(receiver, bytes, 64);
            EXPECT_RESULT(UpdateStatus::Truncated, status);
        } else if (scenario == 1) {
            bytes.push_back(0);
            EXPECT_RESULT(UpdateStatus::TooLarge, deliver(receiver, bytes, 64));
        } else if (scenario == 2) {
            receiver.feed(bytes.data(), 150);
            receiver.cancel();
            EXPECT_RESULT(UpdateStatus::Truncated, receiver.status());
            TEST_ASSERT_TRUE(sink.aborted);
        } else if (scenario == 6) {
            receiver.feed(bytes.data(), 20); // Header incomplete.
            EXPECT_RESULT(UpdateStatus::Truncated, receiver.finish());
            TEST_ASSERT_FALSE(sink.begun);
            EXPECT_RESULT(UpdateStatus::Truncated, receiver.feed(nullptr, 3));
        } else {
            EXPECT_RESULT(UpdateStatus::WriteError, deliver(receiver, bytes, 64));
            TEST_ASSERT_FALSE(sink.committed);
        }
    }
    MemorySink sink;
    UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey), UpdateRole::Receiver, 0,
                            MaxImage);
    EXPECT_RESULT(UpdateStatus::WriteError, receiver.feed(nullptr, 5));
    const UpdateStatus all[] = {
        UpdateStatus::Receiving,    UpdateStatus::BadHeader,  UpdateStatus::WrongRole,
        UpdateStatus::Downgrade,    UpdateStatus::TooLarge,   UpdateStatus::Truncated,
        UpdateStatus::BadSignature, UpdateStatus::WriteError, UpdateStatus(99)};
    for (auto status : all) TEST_ASSERT_GREATER_THAN(0, std::strlen(updateStatusName(status)));
}
void test_header_shape_is_checked_before_anything_is_written() {
    for (int scenario = 0; scenario < 4; ++scenario) {
        SCENARIO(scenario);
        auto bytes = update();
        if (scenario == 0) bytes[0] = 'X'; // Magic.
        if (scenario == 1) bytes[4] = 2;   // Format.
        if (scenario == 2) bytes[7] = 1;   // Reserved.
        if (scenario == 3) bytes[16] = 73; // Signature longer than the field.
        MemorySink sink;
        UpdateReceiver receiver(sink, FirmwareTestKey, sizeof(FirmwareTestKey),
                                UpdateRole::Receiver, 0, MaxImage);
        EXPECT_RESULT(UpdateStatus::BadHeader, deliver(receiver, bytes, 100));
        TEST_ASSERT_FALSE(sink.begun);
    }
}
void test_signature_verification_uses_only_the_given_key() {
    uint8_t digest[Sha256Size]{};
    const uint8_t signature[8] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01};
    TEST_ASSERT_FALSE(verifyP256(FirmwareTestKey, sizeof(FirmwareTestKey), digest, signature, 8));
    TEST_ASSERT_FALSE(verifyP256(nullptr, 0, digest, signature, 8));
    TEST_ASSERT_FALSE(
        verifyP256(FirmwareTestKey, sizeof(FirmwareTestKey) - 1, digest, signature, 8));
    TEST_ASSERT_FALSE(verifyP256(FirmwareTestKey, sizeof(FirmwareTestKey), digest, nullptr, 0));
    // SHA-256 known answer (FIPS 180-2, "abc") through the streaming interface.
    Sha256 hash;
    TEST_ASSERT_TRUE(hash.update(reinterpret_cast<const uint8_t*>("a"), 1));
    TEST_ASSERT_TRUE(hash.update(reinterpret_cast<const uint8_t*>("bc"), 2));
    TEST_ASSERT_TRUE(hash.update(nullptr, 0));
    TEST_ASSERT_TRUE(hash.finish(digest));
    const uint8_t expected[Sha256Size] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                          0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                          0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                          0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, digest, Sha256Size);
    TEST_ASSERT_FALSE(hash.finish(digest)); // Single use.
    TEST_ASSERT_FALSE(hash.update(reinterpret_cast<const uint8_t*>("x"), 1));
    Sha256 other;
    TEST_ASSERT_FALSE(other.update(nullptr, 3));
}
} // namespace

void runFirmwareTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_signed_update_installs_in_any_chunking);
    RUN_TEST(test_every_signed_byte_is_protected);
    RUN_TEST(test_role_version_size_and_transport_failures);
    RUN_TEST(test_header_shape_is_checked_before_anything_is_written);
    RUN_TEST(test_signature_verification_uses_only_the_given_key);
}
