#include <unity.h>
#include <cstring>
#include <string>
#include "assertions.h"
#include "cajui_setup.h"
#include "storage_support.h"

namespace {
using namespace cajui;

EnrollmentInfo enrollment(Enrollment state, uint64_t node, uint64_t generation, uint64_t received) {
    EnrollmentInfo info{};
    info.state = state;
    info.node = node;
    info.generation = generation;
    info.received = received;
    return info;
}
bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}
UplinkConfig complete() {
    UplinkConfig c{};
    TEST_ASSERT_TRUE(stageWifi(c, "Home", "wifi-secret") == SetupError::None);
    TEST_ASSERT_TRUE(stageBroker(c, "192.168.1.20", "1883", "receiver-1", "mqtt-secret") ==
                     SetupError::None);
    return c;
}

void test_long_press_fires_once_per_hold() {
    LongPress button(3000);
    TEST_ASSERT_FALSE(button.update(false, 0));
    TEST_ASSERT_FALSE(button.update(true, 100));
    TEST_ASSERT_FALSE(button.update(true, 3099));
    TEST_ASSERT_TRUE(button.update(true, 3100));
    TEST_ASSERT_FALSE(button.update(true, 9000)); // Still held: no repeat.
    TEST_ASSERT_FALSE(button.update(false, 9001));
    TEST_ASSERT_FALSE(button.update(true, 9002)); // A short press never fires.
    TEST_ASSERT_FALSE(button.update(false, 9500));
    TEST_ASSERT_FALSE(button.update(true, UINT32_MAX - 1000)); // Clock wrap.
    TEST_ASSERT_TRUE(button.update(true, 2000));
}
void test_setup_network_name_and_join_code() {
    char ssid[SetupSsidCapacity]{};
    TEST_ASSERT_TRUE(setupSsid(0x000048ca433c5e10ull, ssid, sizeof(ssid)));
    TEST_ASSERT_EQUAL_STRING("Cajui-5E10", ssid);
    TEST_ASSERT_FALSE(setupSsid(1, ssid, 5));
    TEST_ASSERT_FALSE(setupSsid(1, nullptr, 5));
    char qr[QrCapacity]{};
    TEST_ASSERT_TRUE(wifiQr("Cajui-5E10", qr, sizeof(qr)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:Cajui-5E10;;", qr);
    TEST_ASSERT_TRUE(wifiQr("a;b,c:d\"e\\f", qr, sizeof(qr)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:nopass;S:a\\;b\\,c\\:d\\\"e\\\\f;;", qr);
    TEST_ASSERT_FALSE(wifiQr("", qr, sizeof(qr)));
    TEST_ASSERT_FALSE(wifiQr(nullptr, qr, sizeof(qr)));
    TEST_ASSERT_FALSE(wifiQr("Cajui-5E10", qr, 10));
}
void test_wifi_staging_validates_and_keeps_saved_password() {
    UplinkConfig c{};
    EXPECT_RESULT(SetupError::Ssid, stageWifi(c, "", "wifi-secret"));
    EXPECT_RESULT(SetupError::Ssid, stageWifi(c, std::string(33, 's').c_str(), "wifi-secret"));
    EXPECT_RESULT(SetupError::Ssid, stageWifi(c, nullptr, "wifi-secret"));
    EXPECT_RESULT(SetupError::WifiPassword, stageWifi(c, "Home", "short"));
    EXPECT_RESULT(SetupError::WifiPassword, stageWifi(c, "Home", std::string(65, 'p').c_str()));
    EXPECT_RESULT(SetupError::WifiPassword, stageWifi(c, "Home", "")); // Nothing saved yet.
    EXPECT_RESULT(SetupError::WifiPassword, stageWifi(c, "Home", nullptr));
    TEST_ASSERT_EQUAL_STRING("", c.ssid); // Failures leave the staged values untouched.
    EXPECT_RESULT(SetupError::None, stageWifi(c, "Home", "wifi-secret"));
    EXPECT_RESULT(SetupError::None, stageWifi(c, "Home", ""));
    TEST_ASSERT_EQUAL_STRING("wifi-secret", c.wifiPassword);
    EXPECT_RESULT(SetupError::WifiPassword, stageWifi(c, "Other", "")); // New network.
    EXPECT_RESULT(SetupError::None, stageWifi(c, "Rede é", "new-secret"));
    TEST_ASSERT_EQUAL_STRING("Rede é", c.ssid);
}
void test_broker_staging_validates_and_keeps_saved_password() {
    UplinkConfig c{};
    EXPECT_RESULT(SetupError::Host, stageBroker(c, "", "1883", "rx", "p"));
    EXPECT_RESULT(SetupError::Host, stageBroker(c, nullptr, "1883", "rx", "p"));
    EXPECT_RESULT(SetupError::Host, stageBroker(c, "bad host", "1883", "rx", "p"));
    EXPECT_RESULT(SetupError::Host,
                  stageBroker(c, std::string(65, 'h').c_str(), "1883", "rx", "p"));
    const char* ports[] = {"", "0", "65536", "18a3", "123456"};
    for (auto port : ports) {
        UNITY_SET_DETAIL(port);
        EXPECT_RESULT(SetupError::Port, stageBroker(c, "broker.local", port, "rx", "p"));
    }
    EXPECT_RESULT(SetupError::Port, stageBroker(c, "broker.local", nullptr, "rx", "p"));
    EXPECT_RESULT(SetupError::Username, stageBroker(c, "broker.local", "1883", "bad/user", "p"));
    EXPECT_RESULT(SetupError::Username, stageBroker(c, "broker.local", "1883", nullptr, "p"));
    EXPECT_RESULT(SetupError::MqttPassword, stageBroker(c, "broker.local", "1883", "rx", ""));
    EXPECT_RESULT(SetupError::MqttPassword,
                  stageBroker(c, "broker.local", "1883", "rx", std::string(65, 'p').c_str()));
    EXPECT_RESULT(SetupError::MqttPassword, stageBroker(c, "broker.local", "1883", "rx", nullptr));
    TEST_ASSERT_EQUAL_STRING("", c.host);
    EXPECT_RESULT(SetupError::None, stageBroker(c, "broker.local", "65535", "rx", "mqtt-secret"));
    EXPECT_RESULT(SetupError::None, stageBroker(c, "10.0.0.2", "1883", "rx", ""));
    TEST_ASSERT_EQUAL_STRING("mqtt-secret", c.password);
    TEST_ASSERT_EQUAL_UINT16(1883, c.port);
    EXPECT_RESULT(SetupError::MqttPassword, stageBroker(c, "10.0.0.2", "1883", "other", ""));
    const SetupError all[] = {SetupError::None,         SetupError::Ssid, SetupError::WifiPassword,
                              SetupError::Host,         SetupError::Port, SetupError::Username,
                              SetupError::MqttPassword, SetupError(99)};
    for (auto error : all) TEST_ASSERT_GREATER_THAN(0, std::strlen(describe(error)));
}
void test_setup_page_escapes_input_and_never_shows_passwords() {
    auto staged = complete();
    std::strcpy(staged.ssid, "<script>");
    NetworkView networks[2]{};
    std::strcpy(networks[0].ssid, "Cafe \"&\" bar");
    networks[0].rssi = -40;
    std::strcpy(networks[1].ssid, "Home");
    BrokerView brokers[1]{};
    std::strcpy(brokers[0].host, "192.168.1.20");
    brokers[0].port = 1883;
    EnrollmentInfo transmitters[2]{};
    transmitters[0] = enrollment(Enrollment::Active, 2, 10, 34);
    transmitters[1] = enrollment(Enrollment::Revoked, 3, 11, 5);
    SetupView view{};
    view.device = 0x000048ca433c5e10ull;
    view.wifi = WifiState::Connected;
    view.wifiSsid = "Home";
    view.address = "192.168.1.50";
    view.brokerOnline = true;
    view.staged = &staged;
    view.queued = 3;
    view.networks = networks;
    view.networkCount = 2;
    view.brokers = brokers;
    view.brokerCount = 1;
    view.transmitters = transmitters;
    view.transmitterCount = 2;
    view.notice = "Saved <now>";
    static char page[PageCapacity];
    TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
    const std::string html = page;
    TEST_ASSERT_FALSE(contains(html, "<script>"));
    TEST_ASSERT_TRUE(contains(html, "&lt;script&gt;"));
    TEST_ASSERT_TRUE(contains(html, "Cafe &quot;&amp;&quot; bar"));
    TEST_ASSERT_TRUE(contains(html, "Saved &lt;now&gt;"));
    TEST_ASSERT_FALSE(contains(html, "wifi-secret"));
    TEST_ASSERT_FALSE(contains(html, "mqtt-secret"));
    TEST_ASSERT_TRUE(contains(html, "connected to <b>Home</b> (192.168.1.50)"));
    TEST_ASSERT_TRUE(contains(html, "Broker: <b>online</b> (192.168.1.20:1883, user receiver-1)"));
    TEST_ASSERT_TRUE(contains(html, "Samples waiting to be forwarded: 3"));
    TEST_ASSERT_TRUE(contains(html, "/?host=192.168.1.20&amp;port=1883"));
    TEST_ASSERT_TRUE(contains(html, "Changes are staged")); // saved=false
    TEST_ASSERT_TRUE(contains(html, "0000000000000002</td><td>active</td><td>34"));
    TEST_ASSERT_TRUE(contains(html, "0000000000000003</td><td>revoked</td><td>5"));
    TEST_ASSERT_TRUE(contains(html, "name=\"generation\" value=\"000000000000000a\""));
    TEST_ASSERT_FALSE(contains(html, "value=\"000000000000000b\"")); // Revoked: no button.
    TEST_ASSERT_TRUE(contains(html, "2 networks found"));
    TEST_ASSERT_FALSE(renderSetup(view, page, 100));
    TEST_ASSERT_FALSE(renderSetup(view, nullptr, 0));
}
void test_setup_page_states_and_prefill() {
    static char page[PageCapacity];
    SetupView view{};
    const WifiState states[] = {WifiState::Idle, WifiState::Connecting, WifiState::Failed,
                                WifiState(9)};
    const char* expected[] = {"not configured", "connecting to <b>Net</b>", "could not connect",
                              "Wi-Fi: <br>"};
    view.wifiSsid = "Net";
    for (size_t i = 0; i < 4; ++i) {
        SCENARIO(i);
        view.wifi = states[i];
        TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
        TEST_ASSERT_TRUE(contains(page, expected[i]));
    }
    TEST_ASSERT_TRUE(contains(page, "No transmitter enrolled"));
    TEST_ASSERT_TRUE(contains(page, "No broker announced"));
    TEST_ASSERT_TRUE(contains(page, "Broker: offline"));
    TEST_ASSERT_FALSE(contains(page, "Search again")); // Not connected: no discovery link.
    view.scanning = true;
    view.searching = true;
    view.prefillHost = "a b&c";
    view.prefillPort = 1884;
    auto staged = complete();
    view.staged = &staged;
    view.saved = true;
    TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "Scanning for networks"));
    TEST_ASSERT_TRUE(contains(page, "Searching the network"));
    TEST_ASSERT_TRUE(contains(page, "value=\"a b&amp;c\""));
    TEST_ASSERT_TRUE(contains(page, "value=\"1884\""));
    TEST_ASSERT_FALSE(contains(page, "Changes are staged"));
    BrokerView odd[1]{};
    std::strcpy(odd[0].host, "host name");
    odd[0].port = 1;
    view.brokers = odd;
    view.brokerCount = 1;
    view.wifi = WifiState::Connected;
    TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "/?host=host%20name&amp;port=1"));
    TEST_ASSERT_TRUE(contains(page, "Search again"));
    EnrollmentInfo prepared[1]{};
    prepared[0] = enrollment(Enrollment::Prepared, 4, 12, 0);
    view.transmitters = prepared;
    view.transmitterCount = 1;
    TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "prepared"));
    prepared[0].state = Enrollment::Empty;
    TEST_ASSERT_TRUE(renderSetup(view, page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "empty"));
}
void test_confirmation_and_closed_pages() {
    static char page[PageCapacity];
    TEST_ASSERT_TRUE(renderRevoke(2, 10, page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "Revoke transmitter 0000000000000002?"));
    TEST_ASSERT_TRUE(contains(page, "name=\"confirm\" value=\"1\""));
    TEST_ASSERT_FALSE(renderRevoke(2, 10, page, 50));
    TEST_ASSERT_FALSE(renderRevoke(2, 10, nullptr, 0));
    TEST_ASSERT_TRUE(renderClosed(page, sizeof(page)));
    TEST_ASSERT_TRUE(contains(page, "Setup closed"));
    TEST_ASSERT_FALSE(renderClosed(page, 10));
    TEST_ASSERT_FALSE(renderClosed(nullptr, 0));
}
void test_store_lists_enrollments_with_last_received_counter() {
    fixtures::MemoryBlob blob;
    auto store = fixtures::mounted(blob, Role::Receiver);
    EnrollmentInfo list[BindingCapacity]{};
    TEST_ASSERT_EQUAL_size_t(0, store->list(list, BindingCapacity));
    TEST_ASSERT_TRUE(fixtures::enroll(*store));
    TEST_ASSERT_TRUE(fixtures::enroll(*store, 3, 11, 2));
    Frame ack{};
    EXPECT_RESULT(Result::Ok, receive(fixtures::binding(), fixtures::data(7), *store, ack));
    EXPECT_RESULT(Result::Ok, store->revoke(3, 11));
    TEST_ASSERT_EQUAL_size_t(2, store->list(list, BindingCapacity));
    EXPECT_RESULT(Enrollment::Active, list[0].state);
    TEST_ASSERT_EQUAL_UINT64(2, list[0].node);
    TEST_ASSERT_EQUAL_UINT64(7, list[0].received);
    EXPECT_RESULT(Enrollment::Revoked, list[1].state);
    TEST_ASSERT_EQUAL_size_t(1, store->list(list, 1));
    TEST_ASSERT_EQUAL_size_t(0, store->list(nullptr, 4));
    EnrollmentInfo info{};
    TEST_ASSERT_TRUE(store->info(2, 10, info));
    TEST_ASSERT_EQUAL_UINT64(7, info.received);
    blob.failBefore = true;
    EXPECT_RESULT(Result::StorageError, store->revoke(2, 10));
    TEST_ASSERT_EQUAL_size_t(0, store->list(list, BindingCapacity)); // Unhealthy store.
}
} // namespace

void runSetupTests() {
    UnitySetTestFile(__FILE__);
    RUN_TEST(test_long_press_fires_once_per_hold);
    RUN_TEST(test_setup_network_name_and_join_code);
    RUN_TEST(test_wifi_staging_validates_and_keeps_saved_password);
    RUN_TEST(test_broker_staging_validates_and_keeps_saved_password);
    RUN_TEST(test_setup_page_escapes_input_and_never_shows_passwords);
    RUN_TEST(test_setup_page_states_and_prefill);
    RUN_TEST(test_confirmation_and_closed_pages);
    RUN_TEST(test_store_lists_enrollments_with_last_received_counter);
}
