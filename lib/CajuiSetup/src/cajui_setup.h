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
// only for the same network (Wi-Fi) or the same host, port and username (broker).
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
// Radio pairing window state, docs/radio-pairing.md.
constexpr size_t MaxPairingCandidates = 4;
struct PairingView {
    bool open = false;
    uint32_t remainingSeconds = 0;
    size_t count = 0;
    uint64_t nodes[MaxPairingCandidates]{};
    int16_t rssi[MaxPairingCandidates]{};
    bool conflict[MaxPairingCandidates]{}; // Two devices claimed this ID: cannot be added.
    uint64_t offered = 0, paired = 0;
};
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
    size_t networksOmitted = 0; // Set by renderSetup when the list did not fit.
    bool scanning = false;
    const BrokerView* brokers = nullptr;
    size_t brokerCount = 0;
    bool searching = false;
    const EnrollmentInfo* transmitters = nullptr;
    size_t transmitterCount = 0;
    const PairingView* pairing = nullptr; // Absent: new transmitters need USB.
    const char* prefillHost = nullptr;    // Chosen from the discovered brokers.
    uint16_t prefillPort = 0;
    const char* notice = nullptr;
};
constexpr size_t PageCapacity = 12288;
// Nearby networks and announced brokers are outside our control (a beacon flood can fill
// the list with long, escaped names): when the page does not fit, they are dropped from
// the end of their lists before the page is refused.
bool renderSetup(const SetupView&, char* output, size_t capacity);
// The transmitters section alone (list and pairing), refreshed live by the setup page.
bool renderTransmitters(const SetupView&, char* output, size_t capacity);
bool renderRevoke(uint64_t node, uint64_t generation, char* output, size_t capacity);
bool renderClosed(char* output, size_t capacity);
} // namespace cajui
