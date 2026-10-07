// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include "cajui_sht4x.h"
#include "cajui_application.h"

namespace {
class Bus final : public cajui::SensorBus {
public:
    uint8_t response[6] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xa2};
    bool commandOk = true;
    size_t length = 6;
    unsigned writes = 0, reads = 0;
    uint32_t waited = 0;
    uint8_t destination = 0;
    bool command(uint8_t address, uint8_t value) override {
        TEST_ASSERT_EQUAL_HEX8(0xfd, value);
        destination = address;
        ++writes;
        return commandOk;
    }
    void waitMs(uint32_t milliseconds) override { waited += milliseconds; }
    size_t read(uint8_t address, uint8_t* bytes, size_t capacity) override {
        TEST_ASSERT_EQUAL_UINT8(destination, address);
        TEST_ASSERT_EQUAL_UINT32(10, waited);
        TEST_ASSERT_EQUAL_UINT32(6, capacity);
        ++reads;
        std::memcpy(bytes, response, length < capacity ? length : capacity);
        return length;
    }
};
void test_sht4x_valid_response_and_addresses() {
    for (uint8_t addr = 0x44; addr <= 0x46; ++addr) {
        Bus bus;
        float t = 0, h = 0;
        TEST_ASSERT_TRUE(cajui::readSht4x(bus, addr, t, h));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, t);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 56.50095f, h);
        TEST_ASSERT_EQUAL_UINT32(1, bus.writes);
        TEST_ASSERT_EQUAL_UINT32(1, bus.reads);
    }
}
void test_sht4x_crc_corruption_never_becomes_measurement() {
    for (size_t i = 0; i < 6; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            Bus bus;
            bus.response[i] ^= uint8_t(1u << bit);
            float t = 25, h = 50;
            TEST_ASSERT_FALSE(cajui::readSht4x(bus, 0x44, t, h));
            TEST_ASSERT_TRUE(std::isnan(t) && std::isnan(h));
            const auto sample = cajui::climateSample(t, h, 300, 0);
            TEST_ASSERT_EQUAL_UINT8(unsigned(cajui::Status::Error),
                                    unsigned(sample.readings[0].status));
            TEST_ASSERT_EQUAL_UINT8(unsigned(cajui::Status::Error),
                                    unsigned(sample.readings[1].status));
        }
    }
}
void test_sht4x_bus_failures_and_retry() {
    Bus bus;
    float t = 1, h = 1;
    bus.commandOk = false;
    TEST_ASSERT_FALSE(cajui::readSht4x(bus, 0x44, t, h));
    TEST_ASSERT_TRUE(std::isnan(t) && std::isnan(h));
    TEST_ASSERT_EQUAL_UINT32(0, bus.waited);
    TEST_ASSERT_EQUAL_UINT32(0, bus.reads);
    bus.commandOk = true;
    for (size_t length = 0; length <= 7; ++length) {
        if (length == 6) continue;
        bus.length = length;
        bus.waited = 0;
        t = h = 1;
        TEST_ASSERT_FALSE(cajui::readSht4x(bus, 0x44, t, h));
        TEST_ASSERT_TRUE(std::isnan(t) && std::isnan(h));
    }
    bus.waited = 0;
    bus.length = 6;
    TEST_ASSERT_TRUE(cajui::readSht4x(bus, 0x44, t, h));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, t);
}
void test_sht4x_rejects_addresses_without_accessing_bus() {
    for (uint8_t address : {uint8_t(0), uint8_t(0x43), uint8_t(0x47), uint8_t(0xff)}) {
        Bus bus;
        float t = 1, h = 1;
        TEST_ASSERT_FALSE(cajui::readSht4x(bus, address, t, h));
        TEST_ASSERT_TRUE(std::isnan(t) && std::isnan(h));
        TEST_ASSERT_EQUAL_UINT32(0, bus.writes);
    }
}
void test_sht4x_endpoints_and_humidity_clamping() {
    Bus bus;
    float t = 0, h = 0;
    const uint8_t low[6] = {0, 0, 0x81, 0, 0, 0x81};
    std::memcpy(bus.response, low, sizeof(low));
    TEST_ASSERT_TRUE(cajui::readSht4x(bus, 0x44, t, h));
    TEST_ASSERT_EQUAL_FLOAT(-45.0f, t);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, h);
    const uint8_t high[6] = {0xff, 0xff, 0xac, 0xff, 0xff, 0xac};
    std::memcpy(bus.response, high, sizeof(high));
    bus.waited = 0;
    TEST_ASSERT_TRUE(cajui::readSht4x(bus, 0x44, t, h));
    TEST_ASSERT_EQUAL_FLOAT(130.0f, t);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, h);
}
} // namespace
void runSensorTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_sht4x_valid_response_and_addresses);
    RUN_TEST(test_sht4x_crc_corruption_never_becomes_measurement);
    RUN_TEST(test_sht4x_bus_failures_and_retry);
    RUN_TEST(test_sht4x_rejects_addresses_without_accessing_bus);
    RUN_TEST(test_sht4x_endpoints_and_humidity_clamping);
}
