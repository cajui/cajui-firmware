#pragma once
#include "cajui_storage.h"
#include "cajui_uplink.h"
#include <cstddef>
#include <cstdint>

namespace cajui {
// Reports a button held continuously for holdMs, once per press.
class LongPress {
public:
    explicit LongPress(uint32_t holdMs) : hold_(holdMs) {}
    bool update(bool pressed, uint32_t now);

private:
    uint32_t hold_, since_ = 0;
    bool down_ = false, fired_ = false;
};

constexpr size_t SetupSsidCapacity = 16, QrCapacity = 96;
// "Cajui-XXXX" from the last two bytes of the device ID.
bool setupSsid(uint64_t device, char* output, size_t capacity);
// Standard Wi-Fi join code for an open network, scannable by phone cameras.
// TODO(security): the setup network is open; add a per-device password (label/QR).
bool wifiQr(const char* ssid, char* output, size_t capacity);

enum class SetupError { None, Ssid, WifiPassword, Host, Port, Username, MqttPassword };
// Stages fields into a pending configuration. An empty password keeps the staged one
// when the network or username is unchanged, so saved secrets never need re-entry.
SetupError stageWifi(UplinkConfig&, const char* ssid, const char* password);
SetupError stageBroker(UplinkConfig&, const char* host, const char* port, const char* username,
                       const char* password);
const char* describe(SetupError);

// Everything the setup page shows. Passwords are never part of the view.
constexpr size_t MaxNetworks = 12, MaxBrokers = 6;
struct NetworkView {
    char ssid[SsidCapacity + 1]{};
    int rssi = 0;
};
struct BrokerView {
    char host[HostCapacity + 1]{};
    uint16_t port = 0;
};
enum class WifiState { Idle, Connecting, Connected, Failed };
struct SetupView {
    uint64_t device = 0;
    WifiState wifi = WifiState::Idle;
    const char* wifiSsid = "";
    const char* address = ""; // Station IPv4 address when connected.
    bool brokerOnline = false;
    const UplinkConfig* staged = nullptr;
    bool saved = false; // Staged settings match what is stored.
    size_t queued = 0;
    const NetworkView* networks = nullptr;
    size_t networkCount = 0;
    bool scanning = false;
    const BrokerView* brokers = nullptr;
    size_t brokerCount = 0;
    bool searching = false;
    const EnrollmentInfo* transmitters = nullptr;
    size_t transmitterCount = 0;
    const char* prefillHost = nullptr; // Chosen from the discovered brokers.
    uint16_t prefillPort = 0;
    const char* notice = nullptr;
};
constexpr size_t PageCapacity = 12288;
bool renderSetup(const SetupView&, char* output, size_t capacity);
bool renderRevoke(uint64_t node, uint64_t generation, char* output, size_t capacity);
bool renderClosed(char* output, size_t capacity);
} // namespace cajui
