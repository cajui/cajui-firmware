// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cstring>
#include <string>
#include "assertions.h"
#include "cajui_search.h"

namespace {
using namespace cajui;

constexpr uint32_t address(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a << 24U) | (b << 16U) | (c << 8U) | d;
}

void test_search_covers_the_station_network_without_its_edges() {
    SearchRange range{};
    TEST_ASSERT_TRUE(searchRange(address(192, 168, 1, 37), address(255, 255, 255, 0), range));
    TEST_ASSERT_EQUAL_HEX32(address(192, 168, 1, 1), range.first);
    TEST_ASSERT_EQUAL_HEX32(address(192, 168, 1, 254), range.last);
    TEST_ASSERT_TRUE(searchRange(address(192, 168, 1, 200), address(255, 255, 255, 128), range));
    TEST_ASSERT_EQUAL_HEX32(address(192, 168, 1, 129), range.first);
    TEST_ASSERT_EQUAL_HEX32(address(192, 168, 1, 254), range.last);
    // The smallest network with another host.
    TEST_ASSERT_TRUE(searchRange(address(10, 0, 0, 1), address(255, 255, 255, 252), range));
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 0, 1), range.first);
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 0, 2), range.last);
}
void test_search_of_a_large_network_is_bounded_to_the_block_around_the_station() {
    SearchRange range{};
    TEST_ASSERT_TRUE(searchRange(address(10, 0, 5, 77), address(255, 255, 0, 0), range));
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 5, 1), range.first);
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 5, 254), range.last);
    TEST_ASSERT_EQUAL_UINT32(MaxSearchHosts, range.last - range.first + 1);
    // In a larger network an address ending in 0 or 255 is an ordinary host.
    TEST_ASSERT_TRUE(searchRange(address(10, 0, 5, 0), address(255, 255, 254, 0), range));
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 5, 1), range.first);
    TEST_ASSERT_EQUAL_HEX32(address(10, 0, 5, 254), range.last);
}
void test_search_refuses_networks_without_another_host() {
    struct Row {
        uint32_t address, netmask;
    };
    const Row rows[] = {
        {address(192, 168, 1, 10), 0},                           // No mask yet.
        {address(192, 168, 1, 10), address(255, 0, 255, 0)},     // Not contiguous.
        {address(192, 168, 1, 10), address(255, 255, 255, 254)}, // /31
        {address(192, 168, 1, 10), address(255, 255, 255, 255)}, // /32
        {address(192, 168, 1, 0), address(255, 255, 255, 0)},    // Network address.
        {address(192, 168, 1, 255), address(255, 255, 255, 0)},  // Broadcast address.
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        SCENARIO(i);
        SearchRange range{};
        TEST_ASSERT_FALSE(searchRange(rows[i].address, rows[i].netmask, range));
        TEST_ASSERT_EQUAL_UINT32(0, range.first);
        TEST_ASSERT_EQUAL_UINT32(0, range.last);
    }
}
void test_addresses_and_probe_ids_are_formatted_into_large_enough_buffers_only() {
    char text[AddressCapacity]{};
    TEST_ASSERT_TRUE(formatAddress(address(192, 168, 1, 20), text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", text);
    TEST_ASSERT_TRUE(formatAddress(address(255, 255, 255, 255), text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", text);
    TEST_ASSERT_FALSE(formatAddress(1, text, sizeof(text) - 1));
    TEST_ASSERT_FALSE(formatAddress(1, nullptr, sizeof(text)));

    char id[ProbeIdCapacity + 1]{};
    TEST_ASSERT_TRUE(formatProbeId(0x0123456789abcdefULL, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("cjprobe0123456789abcdef", id);
    TEST_ASSERT_EQUAL_size_t(ProbeIdCapacity, std::strlen(id));
    TEST_ASSERT_FALSE(formatProbeId(1, id, sizeof(id) - 1));
    TEST_ASSERT_FALSE(formatProbeId(1, nullptr, sizeof(id)));
}
void test_probe_is_a_clean_session_connect_with_the_credentials() {
    uint8_t packet[ProbeCapacity]{};
    const uint8_t expected[] = {0x10, 19, 0, 4,   'M', 'Q', 'T', 'T', 4, 0xc2, 0,
                                10,   0,  1, 'c', 0,   1,   'u', 0,   1, 'p'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), probeConnect("c", "u", "p", packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, packet, sizeof(expected));
    // The exact size fits; one byte less does not.
    TEST_ASSERT_EQUAL_size_t(sizeof(expected),
                             probeConnect("c", "u", "p", packet, sizeof(expected)));
    TEST_ASSERT_EQUAL_size_t(0, probeConnect("c", "u", "p", packet, sizeof(expected) - 1));

    // Longest fields: the remaining length needs two bytes (167 = 0x27 + 1 * 128).
    const std::string id(ProbeIdCapacity, 'i');
    const std::string user(ProbeFieldCapacity, 'u');
    const std::string secret(ProbeFieldCapacity, 's');
    TEST_ASSERT_EQUAL_size_t(ProbeCapacity, probeConnect(id.c_str(), user.c_str(), secret.c_str(),
                                                         packet, sizeof(packet)));
    TEST_ASSERT_EQUAL_HEX8(0x10, packet[0]);
    TEST_ASSERT_EQUAL_HEX8(0xa7, packet[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, packet[2]);
    TEST_ASSERT_EQUAL_HEX8('s', packet[ProbeCapacity - 1]);
    TEST_ASSERT_EQUAL_size_t(
        0, probeConnect(id.c_str(), user.c_str(), secret.c_str(), packet, sizeof(packet) - 1));
}
void test_probe_refuses_missing_empty_or_oversized_fields() {
    uint8_t packet[ProbeCapacity]{};
    const std::string longId(ProbeIdCapacity + 1, 'i');
    const std::string longField(ProbeFieldCapacity + 1, 'x');
    struct Row {
        const char *id, *user, *secret;
    };
    const Row rows[] = {
        {nullptr, "u", "p"},
        {"c", nullptr, "p"},
        {"c", "u", nullptr},
        {"", "u", "p"},
        {"c", "", "p"},
        {"c", "u", ""},
        {longId.c_str(), "u", "p"},
        {"c", longField.c_str(), "p"},
        {"c", "u", longField.c_str()},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        SCENARIO(i);
        TEST_ASSERT_EQUAL_size_t(
            0, probeConnect(rows[i].id, rows[i].user, rows[i].secret, packet, sizeof(packet)));
    }
    TEST_ASSERT_EQUAL_size_t(0, probeConnect("c", "u", "p", nullptr, sizeof(packet)));
}
void test_only_a_connack_with_code_zero_accepts() {
    const uint8_t accepted[] = {0x20, 2, 0, 0};
    const uint8_t resumed[] = {0x20, 2, 1, 0}; // Session present does not matter.
    const uint8_t badPassword[] = {0x20, 2, 0, 4};
    const uint8_t notAuthorized[] = {0x20, 2, 0, 5};
    const uint8_t http[] = {'H', 'T', 'T', 'P'};
    const uint8_t otherLength[] = {0x20, 3, 0, 0};
    EXPECT_RESULT(ProbeAnswer::Accepted, probeAnswer(accepted, sizeof(accepted)));
    EXPECT_RESULT(ProbeAnswer::Accepted, probeAnswer(resumed, sizeof(resumed)));
    EXPECT_RESULT(ProbeAnswer::Refused, probeAnswer(badPassword, sizeof(badPassword)));
    EXPECT_RESULT(ProbeAnswer::Refused, probeAnswer(notAuthorized, sizeof(notAuthorized)));
    EXPECT_RESULT(ProbeAnswer::NotMqtt, probeAnswer(http, sizeof(http)));
    EXPECT_RESULT(ProbeAnswer::NotMqtt, probeAnswer(http, 1));
    EXPECT_RESULT(ProbeAnswer::NotMqtt, probeAnswer(otherLength, sizeof(otherLength)));
    EXPECT_RESULT(ProbeAnswer::NotMqtt, probeAnswer(otherLength, 2));
    // A CONNACK that has not fully arrived yet decides nothing.
    for (size_t size = 0; size < sizeof(accepted); ++size) {
        SCENARIO(size);
        EXPECT_RESULT(ProbeAnswer::Incomplete, probeAnswer(accepted, size));
    }
    EXPECT_RESULT(ProbeAnswer::Incomplete, probeAnswer(nullptr, sizeof(accepted)));
}
void test_search_starts_after_two_minutes_offline_and_repeats_every_five() {
    SearchSchedule schedule;
    const uint32_t first = SearchSchedule::FirstDelayMs;
    const uint32_t retry = SearchSchedule::RetryMs;
    uint32_t now = 1000;
    TEST_ASSERT_FALSE(schedule.due(now, true, true));
    now += 500; // The outage starts here.
    TEST_ASSERT_FALSE(schedule.due(now, true, false));
    TEST_ASSERT_FALSE(schedule.due(now + first - 1, true, false));
    now += first;
    TEST_ASSERT_TRUE(schedule.due(now, true, false));
    TEST_ASSERT_FALSE(schedule.due(now, true, false)); // Once per due time.
    TEST_ASSERT_FALSE(schedule.due(now + retry - 1, true, false));
    now += retry;
    TEST_ASSERT_TRUE(schedule.due(now, true, false));
    now += retry;
    TEST_ASSERT_TRUE(schedule.due(now, true, false));
}
void test_search_waits_again_after_the_broker_or_wifi_came_back() {
    SearchSchedule schedule;
    const uint32_t first = SearchSchedule::FirstDelayMs;
    // The timer wraps during the outage.
    uint32_t now = UINT32_MAX - first / 2;
    TEST_ASSERT_FALSE(schedule.due(now, true, false));
    now += first;
    TEST_ASSERT_TRUE(schedule.due(now, true, false));
    // Back online: the next outage waits the first delay again, not the retry interval.
    now += 10;
    TEST_ASSERT_FALSE(schedule.due(now, true, true));
    now += 10;
    TEST_ASSERT_FALSE(schedule.due(now, true, false));
    TEST_ASSERT_FALSE(schedule.due(now + first - 1, true, false));
    now += first;
    TEST_ASSERT_TRUE(schedule.due(now, true, false));
    // Without Wi-Fi there is no network to search, and time without it does not count.
    now += 10 * first;
    TEST_ASSERT_FALSE(schedule.due(now, false, false));
    now += 10;
    TEST_ASSERT_FALSE(schedule.due(now, true, false));
    TEST_ASSERT_FALSE(schedule.due(now + first - 1, true, false));
    TEST_ASSERT_TRUE(schedule.due(now + first, true, false));
}
} // namespace

void runSearchTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_search_covers_the_station_network_without_its_edges);
    RUN_TEST(test_search_of_a_large_network_is_bounded_to_the_block_around_the_station);
    RUN_TEST(test_search_refuses_networks_without_another_host);
    RUN_TEST(test_addresses_and_probe_ids_are_formatted_into_large_enough_buffers_only);
    RUN_TEST(test_probe_is_a_clean_session_connect_with_the_credentials);
    RUN_TEST(test_probe_refuses_missing_empty_or_oversized_fields);
    RUN_TEST(test_only_a_connack_with_code_zero_accepts);
    RUN_TEST(test_search_starts_after_two_minutes_offline_and_repeats_every_five);
    RUN_TEST(test_search_waits_again_after_the_broker_or_wifi_came_back);
}
