// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include "cajui_registry.h"
#include "cajui_sensors.h"
#include "cajui_sht4x.h"

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
// A driver double: answers with fixed values, or not at all.
class Fake final : public cajui::Sensor {
public:
    cajui::SensorInfo description;
    float values[cajui::MaxChannels] = {0, 0, 0, 0};
    bool attached = true;
    unsigned reads = 0, probes = 0, offs = 0;
    Fake(uint16_t id, uint16_t first, uint16_t second = 0)
        : description{"fake", id, uint8_t(second ? 2 : 1), {{first, -40, 80}, {second, 0, 100}}} {}
    const cajui::SensorInfo& info() const override { return description; }
    bool read(float* output, bool probing) override {
        ++reads;
        if (probing) ++probes;
        if (!attached) return false;
        std::memcpy(output, values, sizeof(values));
        return true;
    }
    void off() override { ++offs; }
};
cajui::Detection powerOn() {
    cajui::Detection state;
    std::memset(&state, 0xff, sizeof(state)); // RTC memory is arbitrary before reset().
    cajui::reset(state);
    return state;
}
void test_registry_names_known_metrics_and_units_only() {
    using namespace cajui;
    TEST_ASSERT_EQUAL_STRING("temperature", quantity(metric::Temperature)->name);
    TEST_ASSERT_EQUAL_UINT8(unit::Celsius, quantity(metric::Temperature)->unit);
    TEST_ASSERT_EQUAL_STRING("humidity", quantity(metric::Humidity)->name);
    TEST_ASSERT_EQUAL_UINT8(unit::Percent, quantity(metric::Humidity)->unit);
    TEST_ASSERT_EQUAL_STRING("voltage", quantity(metric::Voltage)->name);
    TEST_ASSERT_EQUAL_UINT8(unit::Volt, quantity(metric::Voltage)->unit);
    TEST_ASSERT_NULL(quantity(0));
    TEST_ASSERT_NULL(quantity(4));
    TEST_ASSERT_EQUAL_STRING("degC", unitName(unit::Celsius));
    TEST_ASSERT_EQUAL_STRING("%", unitName(unit::Percent));
    TEST_ASSERT_EQUAL_STRING("V", unitName(unit::Volt));
    TEST_ASSERT_NULL(unitName(0));
    TEST_ASSERT_NULL(unitName(4));
}
void test_detection_keeps_only_the_sensors_that_answer() {
    Fake climate(1, cajui::metric::Temperature, cajui::metric::Humidity);
    Fake absent(2, cajui::metric::Temperature, cajui::metric::Humidity);
    Fake analog(3, cajui::metric::Voltage);
    climate.values[0] = -12.345f;
    climate.values[1] = 0;
    analog.values[0] = 2.134f;
    absent.attached = false;
    cajui::Sensor* sensors[] = {&climate, &absent, &analog};
    auto state = powerOn();
    auto sample = cajui::collectSample(sensors, 3, state, 300, 3912);
    TEST_ASSERT_EQUAL_UINT16(3912, sample.batteryMv);
    TEST_ASSERT_EQUAL_UINT32(300, sample.nextSeconds);
    TEST_ASSERT_EQUAL_UINT8(0x05, state.present);
    TEST_ASSERT_EQUAL_UINT(3, sample.count);
    TEST_ASSERT_EQUAL_UINT16(1, sample.readings[0].sensor);
    TEST_ASSERT_EQUAL_UINT16(cajui::metric::Temperature, sample.readings[0].metric);
    TEST_ASSERT_EQUAL_UINT8(cajui::unit::Celsius, sample.readings[0].unit);
    TEST_ASSERT_EQUAL_INT32(-12345, sample.readings[0].milliValue);
    // Zero is a measurement, not a missing value.
    TEST_ASSERT_EQUAL_INT(int(cajui::Status::Ok), int(sample.readings[1].status));
    TEST_ASSERT_EQUAL_INT32(0, sample.readings[1].milliValue);
    TEST_ASSERT_EQUAL_UINT16(3, sample.readings[2].sensor);
    TEST_ASSERT_EQUAL_UINT8(cajui::unit::Volt, sample.readings[2].unit);
    TEST_ASSERT_EQUAL_INT32(2134, sample.readings[2].milliValue);
    // Each driver was asked once, as a probe.
    TEST_ASSERT_EQUAL_UINT(1, climate.probes);
    TEST_ASSERT_EQUAL_UINT(1, absent.probes);
    // Later samples read the known sensors only, without probing.
    sample = cajui::collectSample(sensors, 3, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT(3, sample.count);
    TEST_ASSERT_EQUAL_UINT(0, sample.batteryMv);
    TEST_ASSERT_EQUAL_UINT(2, climate.reads);
    TEST_ASSERT_EQUAL_UINT(1, climate.probes);
    TEST_ASSERT_EQUAL_UINT(1, absent.reads);
    TEST_ASSERT_EQUAL_UINT(1, analog.probes);
}
void test_invalid_values_become_errors_never_measurements() {
    Fake climate(1, cajui::metric::Temperature, cajui::metric::Humidity);
    cajui::Sensor* sensors[] = {&climate};
    auto state = powerOn();
    const float outside[] = {NAN, INFINITY, -INFINITY, -41, 81};
    for (float value : outside) {
        climate.values[0] = value;
        climate.values[1] = 50;
        const auto sample = cajui::collectSample(sensors, 1, state, 300, 0);
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[0].status));
        TEST_ASSERT_EQUAL_INT32(0, sample.readings[0].milliValue);
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Ok), int(sample.readings[1].status));
    }
    for (float value : {-1.0f, 101.0f, NAN}) {
        climate.values[0] = 0;
        climate.values[1] = value;
        const auto sample = cajui::collectSample(sensors, 1, state, 300, 0);
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Ok), int(sample.readings[0].status));
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[1].status));
        TEST_ASSERT_EQUAL_INT32(0, sample.readings[1].milliValue);
    }
    climate.values[0] = -40;
    climate.values[1] = 100;
    auto sample = cajui::collectSample(sensors, 1, state, 300, 0);
    TEST_ASSERT_EQUAL_INT32(-40000, sample.readings[0].milliValue);
    TEST_ASSERT_EQUAL_INT32(100000, sample.readings[1].milliValue);
    climate.values[0] = 80;
    sample = cajui::collectSample(sensors, 1, state, 300, 0);
    TEST_ASSERT_EQUAL_INT32(80000, sample.readings[0].milliValue);
}
void test_a_known_sensor_that_stops_answering_reports_errors_and_is_never_forgotten() {
    Fake climate(1, cajui::metric::Temperature, cajui::metric::Humidity);
    Fake late(2, cajui::metric::Voltage);
    late.attached = false;
    climate.values[0] = 20;
    climate.values[1] = 50;
    cajui::Sensor* sensors[] = {&climate, &late};
    auto state = powerOn();
    cajui::collectSample(sensors, 2, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT8(0x01, state.present);
    climate.attached = false;
    late.attached = true;
    late.values[0] = 1.5f;
    // Three missed samples: errors for the known sensor, and nobody else is tried.
    for (uint8_t miss = 1; miss <= cajui::RedetectAfter; ++miss) {
        const auto sample = cajui::collectSample(sensors, 2, state, 300, 0);
        TEST_ASSERT_EQUAL_UINT(2, sample.count);
        TEST_ASSERT_EQUAL_UINT16(1, sample.readings[0].sensor);
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[0].status));
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[1].status));
        TEST_ASSERT_EQUAL_UINT8(miss, state.misses[0]);
        TEST_ASSERT_EQUAL_UINT(1, late.reads);
    }
    // The next sample looks again: the other sensor joins, the missing one stays reported.
    auto sample = cajui::collectSample(sensors, 2, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT8(0x03, state.present);
    TEST_ASSERT_EQUAL_UINT8(0, state.misses[0]);
    TEST_ASSERT_EQUAL_UINT(3, sample.count);
    TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[0].status));
    TEST_ASSERT_EQUAL_UINT16(2, sample.readings[2].sensor);
    TEST_ASSERT_EQUAL_INT32(1500, sample.readings[2].milliValue);
    // Answering again clears the count; only reset() forgets a sensor.
    cajui::collectSample(sensors, 2, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT8(1, state.misses[0]);
    climate.attached = true;
    sample = cajui::collectSample(sensors, 2, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT8(0, state.misses[0]);
    TEST_ASSERT_EQUAL_INT(int(cajui::Status::Ok), int(sample.readings[0].status));
    cajui::reset(state);
    TEST_ASSERT_EQUAL_UINT8(0, state.present);
}
void test_with_nothing_attached_the_first_driver_reports_the_fault_and_detection_repeats() {
    Fake climate(1, cajui::metric::Temperature, cajui::metric::Humidity);
    Fake analog(3, cajui::metric::Voltage);
    climate.attached = analog.attached = false;
    cajui::Sensor* sensors[] = {&climate, &analog};
    auto state = powerOn();
    for (unsigned pass = 1; pass <= 2; ++pass) {
        const auto sample = cajui::collectSample(sensors, 2, state, 300, 0);
        TEST_ASSERT_EQUAL_UINT(2, sample.count);
        TEST_ASSERT_EQUAL_UINT16(1, sample.readings[0].sensor);
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[0].status));
        TEST_ASSERT_EQUAL_INT(int(cajui::Status::Error), int(sample.readings[1].status));
        TEST_ASSERT_EQUAL_UINT8(0, state.present);
        TEST_ASSERT_EQUAL_UINT(pass, analog.probes);
    }
    analog.attached = true;
    analog.values[0] = 3.3f;
    const auto sample = cajui::collectSample(sensors, 2, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT(1, sample.count);
    TEST_ASSERT_EQUAL_UINT16(3, sample.readings[0].sensor);
    // No driver at all gives an empty sample, which the sender refuses.
    auto none = powerOn();
    TEST_ASSERT_EQUAL_UINT(0, cajui::collectSample(sensors, 0, none, 300, 0).count);
}
void test_frame_capacity_and_unregistered_metrics_bound_the_sample() {
    // Five two-channel sensors: the fifth does not fit in eight readings and is not read.
    Fake a(1, 1, 2), b(2, 1, 2), c(3, 1, 2), d(4, 1, 2), e(5, 1, 2);
    cajui::Sensor* five[] = {&a, &b, &c, &d, &e};
    auto state = powerOn();
    auto sample = cajui::collectSample(five, 5, state, 300, 0);
    TEST_ASSERT_EQUAL_UINT(cajui::MaxReadings, sample.count);
    TEST_ASSERT_EQUAL_UINT8(0x0f, state.present);
    TEST_ASSERT_EQUAL_UINT(0, e.reads);
    // More drivers than the state can track: the extra ones are ignored.
    Fake single(9, cajui::metric::Voltage);
    cajui::Sensor* many[cajui::MaxSensors + 1];
    for (auto& sensor : many) sensor = &single;
    auto wide = powerOn();
    sample = cajui::collectSample(many, cajui::MaxSensors + 1, wide, 300, 0);
    TEST_ASSERT_EQUAL_UINT(cajui::MaxSensors, single.reads);
    TEST_ASSERT_EQUAL_UINT8(0xff, wide.present);
    // A channel outside the registry has no unit to travel with and is left out.
    Fake odd(7, 99, cajui::metric::Humidity);
    odd.values[1] = 40;
    cajui::Sensor* one[] = {&odd};
    auto fresh = powerOn();
    sample = cajui::collectSample(one, 1, fresh, 300, 0);
    TEST_ASSERT_EQUAL_UINT(1, sample.count);
    TEST_ASSERT_EQUAL_UINT16(cajui::metric::Humidity, sample.readings[0].metric);
    TEST_ASSERT_EQUAL_INT32(40000, sample.readings[0].milliValue);
    // A description claiming more channels than it can hold is cut at the limit.
    Fake long_(8, cajui::metric::Voltage);
    long_.description.count = cajui::MaxChannels + 1;
    for (auto& channel : long_.description.channels) channel = {cajui::metric::Voltage, 0, 5};
    cajui::Sensor* overlong[] = {&long_};
    auto cut = powerOn();
    sample = cajui::collectSample(overlong, 1, cut, 300, 0);
    TEST_ASSERT_EQUAL_UINT(cajui::MaxChannels, sample.count);
}
} // namespace
void runSensorTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_registry_names_known_metrics_and_units_only);
    RUN_TEST(test_detection_keeps_only_the_sensors_that_answer);
    RUN_TEST(test_invalid_values_become_errors_never_measurements);
    RUN_TEST(test_a_known_sensor_that_stops_answering_reports_errors_and_is_never_forgotten);
    RUN_TEST(test_with_nothing_attached_the_first_driver_reports_the_fault_and_detection_repeats);
    RUN_TEST(test_frame_capacity_and_unregistered_metrics_bound_the_sample);
    RUN_TEST(test_sht4x_valid_response_and_addresses);
    RUN_TEST(test_sht4x_crc_corruption_never_becomes_measurement);
    RUN_TEST(test_sht4x_bus_failures_and_retry);
    RUN_TEST(test_sht4x_rejects_addresses_without_accessing_bus);
    RUN_TEST(test_sht4x_endpoints_and_humidity_clamping);
}
