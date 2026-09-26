// SPDX-License-Identifier: Apache-2.0
#include <unity.h>
#include <cstring>
#include <string>
#include "assertions.h"
#include "cajui_command.h"

namespace {
using namespace cajui;

ParseResult parse(const std::string& payload, Command& command) {
    return parseCommand(payload.data(), payload.size(), command);
}
std::string result(const char* id, CommandStatus status, CommandReason reason) {
    char out[ResultCapacity]{};
    size_t size = 0;
    if (!formatResult(id, status, reason, out, sizeof(out), size)) return "<rejected>";
    TEST_ASSERT_EQUAL_size_t(std::strlen(out), size);
    return out;
}

void test_commands_parse_strictly() {
    Command c{};
    EXPECT_RESULT(
        ParseResult::Ok,
        parse(R"({"version":1,"command_id":"c-7f3a","type":"pairing.open","params":{}})", c));
    TEST_ASSERT_EQUAL_STRING("c-7f3a", c.id);
    EXPECT_RESULT(CommandType::PairingOpen, c.type);
    EXPECT_RESULT(ParseResult::Ok, parse(" {\n \"params\" : { \"node_id\" : \"00000000000000a2\" } "
                                         ",\t\"type\":\"pairing.accept\","
                                         "\"command_id\":\"A.b:c_d-1\",\"version\":1 } \r\n",
                                         c));
    EXPECT_RESULT(CommandType::PairingAccept, c.type);
    TEST_ASSERT_EQUAL_UINT64(0xa2, c.node);
    EXPECT_RESULT(ParseResult::Ok,
                  parse(R"({"version":1,"command_id":"x","type":"pairing.close","params":{}})", c));
    EXPECT_RESULT(CommandType::PairingClose, c.type);
    EXPECT_RESULT(
        ParseResult::Ok,
        parse(
            R"({"version":1,"command_id":"x","type":"node.revoke","params":{"node_id":"ffffffffffffffff"}})",
            c));
    EXPECT_RESULT(CommandType::NodeRevoke, c.type);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c.node);
    EXPECT_RESULT(
        ParseResult::Unsupported,
        parse(R"({"version":1,"command_id":"x","type":"parameters.set","params":{}})", c));
    TEST_ASSERT_EQUAL_STRING("x", c.id);
}
void test_bad_commands_are_invalid_or_unreadable() {
    const char* invalid[] = {
        R"({"version":2,"command_id":"x","type":"pairing.open","params":{}})",
        R"({"version":01,"command_id":"x","type":"pairing.open","params":{}})",
        R"({"command_id":"x","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"x","params":{}})",
        R"({"version":1,"command_id":"x","type":"pairing.open"})",
        R"({"version":1,"command_id":"x","type":"pairing.open","params":{},"extra":1})",
        R"({"version":1,"command_id":"x","type":"pairing.open","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"x","type":"pairing.open","params":{}} {})",
        R"({"version":1,"command_id":"x","type":"pairing.open","params":{}},)",
        R"({"version":1,"command_id":"x","type":"pairing.open","params":{"node_id":"00000000000000a2"}})",
        R"({"version":1,"command_id":"x","type":"pairing.accept","params":{}})",
        R"({"version":1,"command_id":"x","type":"pairing.accept","params":{"node_id":"A2"}})",
        R"({"version":1,"command_id":"x","type":"pairing.accept","params":{"node_id":"00000000000000A2"}})",
        R"({"version":1,"command_id":"x","type":"pairing.accept","params":{"node_id":"00000000000000a2","node_id":"00000000000000a3"}})",
        R"({"version":1,"command_id":"x","type":"pairing.accept","params":{"other":"1"}})",
        R"({"version":1,"command_id":"x","type":"pairing\u002eopen","params":{}})", // An escape,
                                                                                    // even a valid
                                                                                    // one.
        R"({"version":1,"command_id":"x","type":"pairing.open","params":[]})",
        R"({"version":1,"command_id":"x","type":"pairing.open","params":{})",
        R"({"version":1,"command_id":"x","type":"a-type-name-too-long","params":{}})",
    };
    size_t index = 0;
    for (const char* payload : invalid) {
        SCENARIO(index++);
        Command c{};
        EXPECT_RESULT(ParseResult::Invalid, parse(payload, c));
        TEST_ASSERT_EQUAL_STRING("x", c.id);
    }
    const std::string unreadable[] = {
        "",
        "[]",
        R"({"version":1,"type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"-x","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"bad id","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"x\"","type":"pairing.open","params":{}})",
        R"({"bogus":1,"command_id":"x","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":")" + std::string(65, 'a') +
            R"(","type":"pairing.open","params":{}})",
        R"({"version":1,"command_id":"x)",
        // A malformed field before command_id stops the reading there.
        R"({"version":1.0,"command_id":"x","type":"pairing.open","params":{}})",
        std::string(CommandPayloadCapacity + 1, ' '),
    };
    index = 0;
    for (const auto& payload : unreadable) {
        SCENARIO(index++);
        Command c{};
        EXPECT_RESULT(ParseResult::Unreadable, parse(payload, c));
    }
    Command c{};
    EXPECT_RESULT(ParseResult::Unreadable, parseCommand(nullptr, 0, c));
}
void test_command_topics_belong_to_the_source() {
    uint64_t device = 0;
    TEST_ASSERT_TRUE(
        parseCommandTopic("manage/v1/receiver-1/000048ca433c5e10/commands", "receiver-1", device));
    TEST_ASSERT_EQUAL_UINT64(0x48ca433c5e10ULL, device);
    const char* bad[] = {
        "manage/v1/receiver-2/000048ca433c5e10/commands",
        "manage/v1/receiver-1x/000048ca433c5e10/commands",
        "manage/v1/receiver-1/000048CA433C5E10/commands",
        "manage/v1/receiver-1/48ca433c5e10/commands",
        "manage/v1/receiver-1/000048ca433c5e10/results",
        "manage/v2/receiver-1/000048ca433c5e10/commands",
        "manage/v1/receiver-1/000048ca433c5e10/commands/x",
    };
    for (const char* topic : bad) TEST_ASSERT_FALSE(parseCommandTopic(topic, "receiver-1", device));
    TEST_ASSERT_FALSE(parseCommandTopic(nullptr, "receiver-1", device));
    TEST_ASSERT_FALSE(parseCommandTopic("manage/v1/r/0000000000000001/commands", nullptr, device));
}
void test_results_match_the_contract() {
    TEST_ASSERT_EQUAL_STRING(
        R"({"version":1,"command_id":"c-7f3a","status":"applied","reason":null})",
        result("c-7f3a", CommandStatus::Applied, CommandReason::None).c_str());
    TEST_ASSERT_EQUAL_STRING(R"({"version":1,"command_id":"x","status":"pending","reason":null})",
                             result("x", CommandStatus::Pending, CommandReason::None).c_str());
    const struct {
        CommandReason reason;
        const char* name;
    } reasons[] = {
        {CommandReason::Invalid, "invalid"},
        {CommandReason::Unsupported, "unsupported"},
        {CommandReason::Busy, "busy"},
        {CommandReason::UnknownNode, "unknown_node"},
        {CommandReason::Closed, "closed"},
        {CommandReason::NotRequested, "not_requested"},
        {CommandReason::Conflict, "conflict"},
        {CommandReason::Full, "full"},
        {CommandReason::Superseded, "superseded"},
        {CommandReason::Storage, "storage"},
        {CommandReason::Failed, "failed"},
    };
    for (const auto& r : reasons) {
        const std::string expected =
            std::string(R"({"version":1,"command_id":"x","status":"rejected","reason":")") +
            r.name + "\"}";
        TEST_ASSERT_EQUAL_STRING(expected.c_str(),
                                 result("x", CommandStatus::Rejected, r.reason).c_str());
    }
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result("x", CommandStatus::Rejected, CommandReason::None).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result("x", CommandStatus::Applied, CommandReason::Busy).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result("bad id", CommandStatus::Applied, CommandReason::None).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result(nullptr, CommandStatus::Applied, CommandReason::None).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result("x", CommandStatus(9), CommandReason::None).c_str());
    TEST_ASSERT_EQUAL_STRING("<rejected>",
                             result("x", CommandStatus::Rejected, CommandReason(99)).c_str());
    char small[8]{};
    size_t size = 1;
    TEST_ASSERT_FALSE(
        formatResult("x", CommandStatus::Applied, CommandReason::None, small, sizeof(small), size));
    TEST_ASSERT_EQUAL_size_t(0, size);
    TEST_ASSERT_FALSE(
        formatResult("x", CommandStatus::Applied, CommandReason::None, nullptr, 8, size));
}
} // namespace

void runCommandTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_commands_parse_strictly);
    RUN_TEST(test_bad_commands_are_invalid_or_unreadable);
    RUN_TEST(test_command_topics_belong_to_the_source);
    RUN_TEST(test_results_match_the_contract);
}
