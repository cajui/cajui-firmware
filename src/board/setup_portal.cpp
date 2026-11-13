// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "setup_portal.h"
#include "broker_scan.h"
#include "cajui_service.h"
#include "display.h"
#include "release_key.h"
#include "sx1262_radio.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <cinttypes>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace board {
namespace {
constexpr uint32_t WifiTimeoutMs = 20000, CloseDelayMs = 1000, MsPerSecond = 1000;
constexpr int HttpOk = 200, HttpFound = 302, HttpSeeOther = 303, HttpForbidden = 403,
              HttpNotFound = 404, HttpServerError = 500;
constexpr uint64_t IdSuffixMask = 0xffff; // Last two bytes of the device ID name the host.
// Right after the station gets an address, scans fail and mDNS queries return nothing.
constexpr uint32_t SettleMs = 3000, DiscoveryRetryMs = 2000;
constexpr int DiscoveryAttempts = 2;
// Early scans sometimes complete as failed on hardware; a later retry succeeds.
constexpr uint32_t ScanRetryMs = 3000;
// The library reports failure after 20 x 300 ms, but with the access point active the scan
// takes longer and its results still arrive; keep polling before giving up.
constexpr uint32_t ScanPatienceMs = 15000;
constexpr uint8_t ScanRetries = 2;
constexpr uint16_t DnsPort = 53;
// Visible feedback on boards without a display: the LED blinks while setup is open.
constexpr uint32_t BlinkMs = 500;
constexpr uint32_t TaskStack = 10240, DiscoveryStack = 6144;
constexpr uint32_t ActiveDelayMs = 2, IdleDelayMs = 20;
class PortalRadioIdle final : public cajui::RadioIdleAccess {
public:
    PortalRadioIdle(AppLock& lock, const std::atomic<cajui::RadioAccessState>& state)
        : lock_(lock), state_(state) {}
    uint32_t nowMs() const override { return millis(); }
    bool take(uint32_t timeoutMs) override { return lock_.takeFor(timeoutMs); }
    void give() override { lock_.give(); }
    cajui::RadioAccessState state() const override { return state_.load(); }
    void wait(uint32_t durationMs) override {
        vTaskDelay(std::max(TickType_t(1), pdMS_TO_TICKS(durationMs)));
    }

private:
    AppLock& lock_;
    const std::atomic<cajui::RadioAccessState>& state_;
};
constexpr int IdDigits = 16;
// WebServer::collectHeaders takes a mutable array.
const char* CollectedHeaders[] = {"Origin"};
bool parseId(const String& text, uint64_t& value) {
    if (text.length() != IdDigits) return false;
    char* end = nullptr;
    value = std::strtoull(text.c_str(), &end, 16);
    return end && !*end && value;
}
bool sameWifi(const cajui::UplinkConfig& a, const cajui::UplinkConfig& b) {
    return !std::strcmp(a.ssid, b.ssid) && !std::strcmp(a.wifiPassword, b.wifiPassword);
}
void copyWifi(cajui::UplinkConfig& to, const cajui::UplinkConfig& from) {
    std::memcpy(to.ssid, from.ssid, sizeof(to.ssid));
    std::memcpy(to.wifiPassword, from.wifiPassword, sizeof(to.wifiPassword));
}
} // namespace

template <typename Action> cajui::RadioAccessState SetupPortal::whileRadioIdle(Action action) {
    PortalRadioIdle access(lock_, *radioState_);
    cajui::RadioIdleGuard held(access);
    if (!held.ready()) {
        Serial.println(held.state() == cajui::RadioAccessState::Stopped
                           ? "CJAPP SETUP radio_stopped"
                           : "CJAPP SETUP radio_busy");
        return held.state();
    }
    action();
    return cajui::RadioAccessState::Listening;
}
std::atomic<uint32_t> SetupPortal::addresses_{0};
bool SetupPortal::start(const std::atomic<cajui::RadioAccessState>& radioState) {
    radioState_ = &radioState;
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { addresses_.fetch_add(1); },
                 ARDUINO_EVENT_WIFI_STA_GOT_IP);
    return xTaskCreate(task, "cajui-setup", TaskStack, this, 1, nullptr) == pdPASS;
}
void SetupPortal::task(void* self) {
    static_cast<SetupPortal*>(self)->run();
}
void SetupPortal::run() {
    cajui::LongPress button(SetupHoldMs);
    pinMode(SetupButton, INPUT_PULLUP);
    for (;;) {
        if (button.update(digitalRead(SetupButton) == LOW, millis())) active_ ? close() : open();
        poll();
        vTaskDelay(pdMS_TO_TICKS(active_ ? ActiveDelayMs : IdleDelayMs));
    }
}
void SetupPortal::open() {
    if (active_) return;
    cancelSearch_.store(true); // The page now decides the network and the broker.
    stale_ = true;
    saveRetry_.reset();
    cajui::setupSsid(store_.device(), ssid_, sizeof(ssid_));
    {
        Locked held(lock_);
        stored_ = cajui::loadUplink(blob_, pending_) == cajui::ReadResult::Ok;
    }
    running_ = cajui::UplinkConfig{};
    if (stored_) copyWifi(running_, pending_);
    savedCurrent_ = stored_;
    scanFailures_ = 0;
    scanFailed_ = scanRetryDue_ = scanPending_ = false;
    trial_ = trialFailed_ = reconnect_ = closing_ = false;
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid_); // Open network: see the TODO in setup_portal.h.
    const IPAddress address = WiFi.softAPIP();
    std::snprintf(address_, sizeof(address_), "%s", address.toString().c_str());
    // Wi-Fi runs now, so the hardware RNG is a true RNG for the session token.
    uint8_t random[cajui::SetupSession::TokenBytes]{};
    if (!entropy_.fill(random, sizeof(random))) {
        WiFi.softAPdisconnect(true); // No session without a random token.
        Serial.println("CJAPP SETUP entropy_error");
        return;
    }
    session_.open(millis(), random);
    std::memset(random, 0, sizeof(random));
    dns_.start(DnsPort, "*", address); // Captive portal: every name resolves here.
    route();
    server_.begin();
    active_ = true;
    wifi_ = MqttUplink::wifiConnected() ? cajui::WifiState::Connected
            : stored_                   ? cajui::WifiState::Connecting
                                        : cajui::WifiState::Idle;
    wifiSince_ = millis();
    scan();
    if (wifi_ == cajui::WifiState::Connected) {
        startMdns();
        settling_ = true;
        settleAt_ = millis();
    }
    char qr[cajui::QrCapacity]{};
    cajui::wifiQr(ssid_, qr, sizeof(qr));
    const bool shown = showSetup(ssid_, qr, address_);
    Serial.printf("CJAPP SETUP open ssid=%s display=%u\n", ssid_, unsigned(shown));
}
// Credentials that never connected are not kept: the station returns to the stored ones.
void SetupPortal::restoreStoredWifi() {
    saveRetry_.reset();
    if (!stored_) return;
    cajui::UplinkConfig stored{};
    bool loaded = false;
    {
        Locked held(lock_);
        loaded = cajui::loadUplink(blob_, stored) == cajui::ReadResult::Ok;
    }
    if (loaded && (!sameWifi(stored, running_) || !MqttUplink::wifiConnected())) {
        MqttUplink::startWifi(stored.ssid, stored.wifiPassword);
        copyWifi(running_, stored);
    }
    if (loaded) {
        pending_ = stored;
        savedCurrent_ = true;
        recovery_.restore(millis());
    }
    cajui::wipe(stored);
}
void SetupPortal::close() {
    if (!active_) return;
    cancelSearch_.store(true); // Nobody is left to read the page's list.
    saveRetry_.reset();
    if (scanning_) esp_wifi_scan_stop();
    scanning_ = scanRetryDue_ = scanPending_ = false;
    WiFi.scanDelete();
    WiFi.setAutoReconnect(true);
    server_.stop();
    dns_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(stored_ ? WIFI_STA : WIFI_OFF);
    restoreStoredWifi();
    displayOff();
    digitalWrite(Led, LOW);
    cajui::wipe(pending_);
    cajui::wipe(running_);
    session_.close();
    // mDNS is stopped once no discovery query is running, then restarted on the next open.
    mdnsEndPending_ = mdns_;
    active_ = false;
    Serial.println("CJAPP SETUP closed");
}
void SetupPortal::poll() {
    // A failed trial/closed portal may restore Wi-Fi asynchronously. Never send the old
    // broker credentials until the station is back on the stored network.
    if (MqttUplink::wifiConnected() && recovery_.due(millis())) {
        cajui::UplinkConfig stored{};
        bool loaded = false;
        {
            Locked held(lock_);
            loaded = cajui::loadUplink(blob_, stored) == cajui::ReadResult::Ok;
        }
        if (loaded && WiFi.SSID() == stored.ssid) {
            recovery_.applied(control_.applyUplink(stored));
        }
        cajui::wipe(stored);
    }
    if (mdnsEndPending_ && !searching_.load()) {
        MDNS.end();
        mdns_ = mdnsEndPending_ = false;
    }
    if (rediscovering_ && !searching_.load()) finishRediscovery();
    if (discoverQueued_ && !searching_.load()) {
        discoverQueued_ = false;
        if (active_) discover();
    }
    if (!active_) {
        rediscover();
        return;
    }
    digitalWrite(Led, (millis() / BlinkMs) % 2 ? HIGH : LOW);
    dns_.processNextRequest();
    server_.handleClient();
    if (reconnect_) {
        reconnect_ = false;
        if (scanning_) {
            esp_wifi_scan_stop();
            WiFi.scanDelete();
            scanning_ = false;
        }
        cancelSearch_.store(true); // The station leaves the network being searched.
        uplink_.suspend();
        recovery_.suspend();
        trialAddresses_ = addresses_.load();
        MqttUplink::startWifi(pending_.ssid, pending_.wifiPassword);
        copyWifi(running_, pending_);
        wifi_ = cajui::WifiState::Connecting;
        wifiSince_ = millis();
    }
    collectScan();
    trackWifi();
    if (saveRetry_.pending() && radioState_->load() == cajui::RadioAccessState::Stopped)
        saveRetry_.record(cajui::SetupSaveResult::RadioStopped, millis());
    if (saveRetry_.due(millis()) && verified()) save();
    const uint32_t now = millis();
    if (settling_ && int32_t(now - settleAt_) >= 0) {
        settling_ = false;
        discover();
        if (!networkCount_) scan(); // Deferred until discovery ends.
    }
    if (scanRetryDue_ && int32_t(now - scanRetryAt_) >= 0) {
        scanRetryDue_ = false;
        scan();
    }
    if (scanPending_ && !searching_.load()) {
        scanPending_ = false;
        scan();
    }
    if ((closing_ && int32_t(now - closeAt_) >= 0) || session_.expired(now)) close();
}
bool SetupPortal::verified() const {
    return wifi_ == cajui::WifiState::Connected && !trial_ && sameWifi(pending_, running_);
}
bool SetupPortal::trialConnected() const {
    return addresses_.load() != trialAddresses_ && WiFi.SSID() == pending_.ssid;
}
void SetupPortal::trackWifi() {
    const bool connected = MqttUplink::wifiConnected() && (!trial_ || trialConnected());
    if (connected && wifi_ != cajui::WifiState::Connected) {
        wifi_ = cajui::WifiState::Connected;
        Serial.printf("CJAPP SETUP wifi=connected address=%s\n", WiFi.localIP().toString().c_str());
        startMdns();
        settleAt_ = millis() + SettleMs;
        settling_ = true;
        if (trial_) {
            trial_ = trialFailed_ = false;
            // Persist verified Wi-Fi even when no broker has been configured.
            if (cajui::validSettings(pending_) && !savedCurrent_) save();
        }
    } else if (!connected && wifi_ == cajui::WifiState::Connected) {
        wifi_ = cajui::WifiState::Connecting;
        wifiSince_ = millis();
    } else if (wifi_ == cajui::WifiState::Connecting &&
               uint32_t(millis() - wifiSince_) >= WifiTimeoutMs) {
        wifi_ = cajui::WifiState::Failed;
        if (trial_) {
            trial_ = false;
            trialFailed_ = true;
            if (stored_) {
                restoreStoredWifi(); // Nothing was saved; go back to what works.
                wifi_ = cajui::WifiState::Connecting;
                wifiSince_ = millis();
                return;
            }
        }
        // Stop retries of unreachable credentials before scanning. The ESP32 refuses
        // scans while its station is still trying to associate. Keep the setup AP on.
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        scanRetryDue_ = true;
        scanRetryAt_ = millis() + ScanRetryMs;
    }
}
void SetupPortal::startMdns() {
    if (mdns_) {
        mdnsEndPending_ = false; // Reopened before the previous session's mDNS stopped.
        return;
    }
    char name[sizeof("cajui-rx-") + 4]{};
    std::snprintf(name, sizeof(name), "cajui-rx-%04x", unsigned(store_.device() & IdSuffixMask));
    mdns_ = MDNS.begin(name);
}
void SetupPortal::scan() {
    // A scan aborts a connection in progress and leaves the station idle until reboot
    // (observed on hardware); scan only when the station is connected or idle.
    if (scanning_ || wifi_ == cajui::WifiState::Connecting) return;
    // Scanning hops channels and drops mDNS traffic: never overlap the two.
    if (searching_.load()) {
        scanPending_ = true;
        return;
    }
    const int16_t started = WiFi.scanNetworks(true);
    scanStartedAt_ = millis();
    scanning_ = started == WIFI_SCAN_RUNNING || started >= 0;
    if (!scanning_) scanFailed();
    Serial.printf("CJAPP SETUP scan start=%d\n", int(started));
}
void SetupPortal::scanFailed() {
    scanFailed_ = true;
    if (scanFailures_ < ScanRetries) {
        ++scanFailures_;
        scanRetryDue_ = true;
        scanRetryAt_ = millis() + ScanRetryMs;
    }
}
void SetupPortal::collectScan() {
    if (!scanning_) return;
    const int16_t found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) {
        if (uint32_t(millis() - scanStartedAt_) < ScanPatienceMs) return;
        esp_wifi_scan_stop();
    }
    if (found == WIFI_SCAN_FAILED && uint32_t(millis() - scanStartedAt_) < ScanPatienceMs) return;
    scanning_ = false;
    networkCount_ = 0;
    Serial.printf("CJAPP SETUP scan found=%d\n", int(found));
    if (found < 0) {
        scanFailed();
    } else {
        scanFailures_ = 0;
        scanFailed_ = false;
    }
    for (int16_t i = 0; i < found; ++i) {
        const String name = WiFi.SSID(i);
        if (!name.length() || name.length() > cajui::SsidCapacity) continue;
        bool duplicate = false;
        for (size_t j = 0; j < networkCount_; ++j)
            if (name == networks_[j].ssid) {
                duplicate = true;
                if (WiFi.RSSI(i) > networks_[j].rssi) networks_[j].rssi = WiFi.RSSI(i);
            }
        if (duplicate || networkCount_ == cajui::MaxNetworks) continue;
        std::strcpy(networks_[networkCount_].ssid, name.c_str());
        networks_[networkCount_++].rssi = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    if (discoverPending_) {
        discoverPending_ = false;
        discover();
    }
}
void SetupPortal::discover() {
    if (scanning_) {
        discoverPending_ = true;
        return;
    }
    if (wifi_ != cajui::WifiState::Connected) return; // No network to search yet.
    bool idle = false;
    // A search in operation is finished by poll() before the page's starts.
    if (rediscovering_ || !searching_.compare_exchange_strong(idle, true)) {
        discoverQueued_ = true; // Search once the running one ends: it may be for another network.
        return;
    }
    searchStored_ = false;
    searchMdns_ = mdns_ && !mdnsEndPending_;
    searchPort_ = pending_.port ? pending_.port : cajui::DefaultMqttPort;
    cancelSearch_.store(false);
    // mDNS queries and the address search block for seconds; run them off the page task.
    if (xTaskCreate(discoveryTask, "cajui-find", DiscoveryStack, this, 1, nullptr) != pdPASS)
        searching_.store(false);
}
void SetupPortal::discoveryTask(void* self) {
    auto* portal = static_cast<SetupPortal*>(self);
    if (portal->searchStored_)
        portal->searchStoredBroker();
    else
        portal->searchForPage();
    portal->searching_.store(false);
    vTaskDelete(nullptr);
}
// For the page: announced brokers first, then every address of the station's network that
// accepts a connection on the broker port. No credentials are sent.
void SetupPortal::searchForPage() {
    int found = 0;
    for (int attempt = 0;
         searchMdns_ && attempt < DiscoveryAttempts && found <= 0 && !cancelSearch_.load();
         ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(DiscoveryRetryMs));
        found = MDNS.queryService("mqtt", "tcp");
    }
    size_t count = 0;
    for (int i = 0; i < found && count < cajui::MaxBrokers; ++i) {
        const String host = MDNS.IP(i).toString();
        if (host.length() > cajui::HostCapacity || host == "0.0.0.0") continue;
        std::strcpy(brokers_[count].host, host.c_str());
        brokers_[count++].port = MDNS.port(i);
    }
    count = findBrokers(searchPort_, brokers_, count, cajui::MaxBrokers, cancelSearch_);
    // A cancelled search was for a network or a page that no longer exists.
    brokerCount_.store(cancelSearch_.load() ? 0 : count);
}
// In operation: which address of the stored network has a broker that checks passwords and
// accepts the stored credentials. The stored password leaves the receiver only while the
// station is on that network, and only to a broker that refused a control password.
void SetupPortal::searchStoredBroker() {
    cajui::BrokerView candidates[cajui::MaxBrokers]{};
    const size_t count = findBrokers(probe_.port, candidates, 0, cajui::MaxBrokers, cancelSearch_);
    candidates_ = count;
    openBrokers_ = 0;
    // The stored host first: when it answers again, staying beats moving.
    for (size_t i = 1; i < count; ++i)
        if (!std::strcmp(candidates[i].host, probe_.host)) {
            std::swap(candidates[0], candidates[i]);
            break;
        }
    uint8_t random[cajui::ControlRandomBytes]{};
    char control[cajui::ControlSecretCapacity]{};
    esp_fill_random(random, sizeof(random)); // A true RNG while Wi-Fi runs.
    if (!cajui::controlSecret(random, sizeof(random), probe_.password, control, sizeof(control)))
        return;
    for (size_t i = 0; i < count && !cancelSearch_.load(); ++i) {
        if (WiFi.SSID() != probe_.ssid) break;
        const char* host = candidates[i].host;
        const auto withControl =
            probeBroker(host, probe_.port, probe_.username, control, probeDevice_);
        const auto withStored =
            withControl == cajui::ProbeAnswer::Refused
                ? probeBroker(host, probe_.port, probe_.username, probe_.password, probeDevice_)
                : cajui::ProbeAnswer::Incomplete;
        const auto verdict = cajui::judgeCandidate(withControl, withStored);
        if (verdict == cajui::BrokerMatch::Open) ++openBrokers_;
        if (verdict != cajui::BrokerMatch::Matches) continue;
        std::strcpy(movedTo_, host);
        break;
    }
}
void SetupPortal::rediscover() {
    // A page search can outlive the page by a few seconds; the due search waits for it.
    if (rediscovering_ || searching_.load() ||
        !search_.due(millis(), MqttUplink::wifiConnected(), uplink_.connected()))
        return;
    bool loaded = false;
    {
        Locked held(lock_);
        loaded = cajui::loadUplink(blob_, probe_) == cajui::ReadResult::Ok;
    }
    bool idle = false;
    if (!loaded || !cajui::validUplink(probe_) || WiFi.SSID() != probe_.ssid ||
        !searching_.compare_exchange_strong(idle, true)) {
        cajui::wipe(probe_);
        return;
    }
    searchStored_ = true;
    probeDevice_ = store_.device();
    movedTo_[0] = 0;
    stale_ = false;
    cancelSearch_.store(false);
    if (xTaskCreate(discoveryTask, "cajui-find", DiscoveryStack, this, 1, nullptr) != pdPASS) {
        searching_.store(false);
        cajui::wipe(probe_);
        search_.postpone(millis());
        return;
    }
    rediscovering_ = true;
    Serial.println("CJAPP UPLINK search");
}
// The search ended: use the broker it found, unless the page opened, the station changed
// network or the stored broker came back meanwhile. The settings the search loaded are
// stale once the page has been open, even if it closed again.
void SetupPortal::finishRediscovery() {
    rediscovering_ = false;
    const bool current = !active_ && !stale_ && !cancelSearch_.load() &&
                         MqttUplink::wifiConnected() && WiFi.SSID() == probe_.ssid &&
                         !uplink_.connected();
    if (current && movedTo_[0]) {
        // The stored host itself accepting means the client is stuck on it (an earlier
        // apply failed): restart it there instead of waiting for a reboot.
        const bool moved = std::strcmp(movedTo_, probe_.host) != 0;
        bool written = !moved;
        if (moved) {
            std::strcpy(probe_.host, movedTo_); // An IPv4 address always fits a host.
            whileRadioIdle([&] { written = cajui::saveUplink(blob_, probe_); });
        }
        const bool applied = written && control_.applyUplink(probe_);
        Serial.printf("CJAPP UPLINK %s host=%s ok=%u\n", moved ? "moved" : "restarted", movedTo_,
                      unsigned(applied));
    } else if (current) {
        Serial.printf("CJAPP UPLINK search found=%u open=%u accepted=0\n", unsigned(candidates_),
                      unsigned(openBrokers_));
    }
    cajui::wipe(probe_);
    movedTo_[0] = 0;
}
// Only the access point interface, only its own address, and for a POST only the page's
// origin with the session token. Anything else is refused without touching state.
bool SetupPortal::authorize(bool post) {
    if (server_.client().localIP() != WiFi.softAPIP()) {
        server_.send(HttpNotFound, "text/plain", "Not found");
        return false;
    }
    if (!cajui::allowedHost(server_.hostHeader().c_str(), address_)) {
        server_.sendHeader("Location", String("http://") + address_ + "/");
        server_.send(HttpFound);
        return false;
    }
    if (post && (!cajui::allowedOrigin(server_.header("Origin").c_str(), address_) ||
                 !session_.validToken(server_.arg("token").c_str()))) {
        server_.send(HttpForbidden, "text/plain",
                     "This setup page expired. Reload it and try again.");
        return false;
    }
    session_.touch(millis());
    return true;
}
void SetupPortal::redirect(cajui::Notice notice) {
    char location[sizeof("/?n=4294967295")] = "/";
    if (notice != cajui::Notice::None)
        std::snprintf(location, sizeof(location), "/?n=%u", unsigned(notice));
    server_.sendHeader("Location", location);
    server_.send(HttpSeeOther);
}
cajui::SetupSaveResult SetupPortal::save() {
    auto result = cajui::SetupSaveResult::Invalid;
    if (cajui::validSettings(pending_)) {
        bool written = false;
        const auto access = whileRadioIdle([&] { written = cajui::saveUplink(blob_, pending_); });
        if (access == cajui::RadioAccessState::Stopped) {
            result = cajui::SetupSaveResult::RadioStopped;
        } else if (access == cajui::RadioAccessState::Busy) {
            result = cajui::SetupSaveResult::RadioBusy;
        } else if (!written) {
            result = cajui::SetupSaveResult::StorageFailed;
        } else {
            stored_ = true;
            result = control_.applyUplink(pending_) ? cajui::SetupSaveResult::Saved
                                                    : cajui::SetupSaveResult::ApplyFailed;
        }
    }
    savedCurrent_ = result == cajui::SetupSaveResult::Saved;
    recovery_.applied(savedCurrent_);
    saveRetry_.record(result, millis());
    Serial.printf("CJAPP SETUP saved ok=%u result=%u\n", unsigned(savedCurrent_), unsigned(result));
    return result;
}
void SetupPortal::fillPairing(cajui::PairingView& pairing) const {
    pairing.open = pairing_->state() != cajui::HostState::Closed;
    pairing.remainingSeconds = pairing_->remainingMs() / MsPerSecond;
    cajui::Candidate waiting[cajui::MaxCandidates]{};
    pairing.count = pairing_->waiting(waiting, cajui::MaxCandidates);
    for (size_t i = 0; i < pairing.count && i < cajui::MaxPairingCandidates; ++i) {
        pairing.nodes[i] = waiting[i].node;
        pairing.rssi[i] = waiting[i].rssi;
        pairing.conflict[i] = waiting[i].conflict;
    }
    if (pairing_->state() == cajui::HostState::Offered) pairing.offered = pairing_->offeredNode();
    pairing.paired = pairing_->pairedNode();
}
void SetupPortal::fillTransmitters(cajui::SetupView& view) {
    view.transmitters = transmitters_;
    view.transmitterCount = store_.list(transmitters_, cajui::BindingCapacity);
}
void SetupPortal::home() {
    cajui::SetupView view{};
    cajui::PairingView pairing{};
    view.device = store_.device();
    view.wifi = wifi_;
    const String network = WiFi.SSID();
    const String address = WiFi.localIP().toString();
    view.wifiSsid = wifi_ == cajui::WifiState::Connected ? network.c_str() : pending_.ssid;
    view.address = address.c_str();
    view.wifiTrialFailed = trialFailed_;
    view.brokerOnline = uplink_.connected();
    view.staged = &pending_;
    view.saved = savedCurrent_;
    view.savePending = saveRetry_.pending();
    view.saveResult = saveRetry_.result();
    view.radioStopped = radioState_->load() == cajui::RadioAccessState::Stopped;
    view.networks = networks_;
    view.networkCount = networkCount_;
    view.scanning = scanning_ || scanRetryDue_ || scanPending_;
    view.scanFailed = scanFailed_ && !view.scanning;
    fillBrokers(view);
    const String host = server_.arg("host");
    const long port = server_.arg("port").toInt();
    if (host.length() && host.length() <= cajui::HostCapacity && port > 0 && port <= UINT16_MAX) {
        view.prefillHost = host.c_str();
        view.prefillPort = uint16_t(port);
    }
    view.notice = cajui::noticeText(cajui::parseNotice(server_.arg("n").c_str()));
    view.token = session_.token();
    view.updates = true;
    view.firmwareVersion = FirmwareVersion;
    bool rendered = false;
    {
        Locked held(lock_);
        view.queued = store_.queued();
        fillTransmitters(view);
        if (pairing_) {
            fillPairing(pairing);
            view.pairing = &pairing;
        }
        rendered = cajui::renderSetup(view, page_, sizeof(page_));
    }
    // Sent without the lock: a slow client must not hold up the radio loop.
    if (!rendered) {
        server_.send(HttpServerError, "text/plain", "Page too large");
        return;
    }
    server_.send(HttpOk, "text/html; charset=utf-8", page_);
}
// A search that is waiting for the station to settle, a scan or another search counts as
// running: the page keeps refreshing until its result is there.
void SetupPortal::fillBrokers(cajui::SetupView& view) {
    view.wifi = wifi_;
    view.searching = searching_.load() || settling_ || discoverPending_ || discoverQueued_;
    view.brokers = brokers_;
    view.brokerCount = view.searching ? 0 : brokerCount_.load();
    view.token = session_.token();
}
void SetupPortal::brokers() {
    cajui::SetupView view{};
    fillBrokers(view);
    if (!cajui::renderBrokers(view, page_, sizeof(page_))) {
        server_.send(HttpServerError, "text/plain", "Page too large");
        return;
    }
    server_.send(HttpOk, "text/html; charset=utf-8", page_);
}
void SetupPortal::transmitters() {
    cajui::SetupView view{};
    cajui::PairingView pairing{};
    view.token = session_.token();
    bool rendered = false;
    {
        Locked held(lock_);
        fillTransmitters(view);
        if (pairing_) {
            fillPairing(pairing);
            view.pairing = &pairing;
        }
        rendered = cajui::renderTransmitters(view, page_, sizeof(page_));
    }
    if (!rendered) {
        server_.send(HttpServerError, "text/plain", "Page too large");
        return;
    }
    server_.send(HttpOk, "text/html; charset=utf-8", page_);
}
// Streams an uploaded update into the free application slot. Runs before the request's
// handler, so authorization is checked here without responding; the handler responds.
void SetupPortal::upload() {
    HTTPUpload& part = server_.upload();
    if (part.status == UPLOAD_FILE_START) {
        delete update_;
        // One install per restart: after it, nothing more is written until the receiver has
        // restarted into it (OtaSink refuses as well).
        uploadAllowed_ = !OtaSink::pending() && server_.client().localIP() == WiFi.softAPIP() &&
                         cajui::allowedHost(server_.hostHeader().c_str(), address_) &&
                         cajui::allowedOrigin(server_.header("Origin").c_str(), address_) &&
                         session_.validToken(server_.arg("token").c_str());
        update_ = uploadAllowed_
                      ? new (std::nothrow) cajui::UpdateReceiver(
                            ota_, cajui::ReleaseKey, sizeof(cajui::ReleaseKey),
                            cajui::UpdateRole::Receiver, FirmwareVersion, OtaSink::capacity())
                      : nullptr;
        if (update_) Serial.println("CJAPP UPDATE receiving");
        return;
    }
    if (!update_) return;
    if (part.status == UPLOAD_FILE_WRITE) {
        update_->feed(part.buf, part.currentSize);
    } else if (part.status == UPLOAD_FILE_END) {
        // Restart even if the response never reaches the client, so an installed image
        // never waits for some unrelated restart.
        if (update_->finish() == cajui::UpdateStatus::Installed) control_.restartForUpdate();
    } else if (part.status == UPLOAD_FILE_ABORTED) {
        update_->cancel();
    }
    session_.touch(millis()); // A large upload is activity.
}
void SetupPortal::uploaded() {
    if (!authorize(true)) {
        delete update_;
        update_ = nullptr;
        return;
    }
    cajui::UpdateStatus status = cajui::UpdateStatus::Truncated;
    uint32_t version = 0;
    if (!update_ && OtaSink::pending()) return redirect(cajui::Notice::UpdatePending);
    if (update_) {
        update_->cancel(); // No effect once installed; aborts a transfer that never ended.
        status = update_->status();
        version = update_->version();
    }
    delete update_;
    update_ = nullptr;
    Serial.printf("CJAPP UPDATE %s version=%u\n", cajui::updateStatusName(status),
                  unsigned(version));
    switch (status) {
    case cajui::UpdateStatus::Installed:
        cajui::renderUpdated(version, page_, sizeof(page_));
        server_.send(HttpOk, "text/html; charset=utf-8", page_);
        return;
    case cajui::UpdateStatus::BadHeader:
    case cajui::UpdateStatus::WrongRole: return redirect(cajui::Notice::UpdateWrongFile);
    case cajui::UpdateStatus::Downgrade: return redirect(cajui::Notice::UpdateOlder);
    case cajui::UpdateStatus::TooLarge: return redirect(cajui::Notice::UpdateTooLarge);
    case cajui::UpdateStatus::BadSignature: return redirect(cajui::Notice::UpdateBadSignature);
    case cajui::UpdateStatus::WriteError: return redirect(cajui::Notice::UpdateFailed);
    case cajui::UpdateStatus::Receiving:
    case cajui::UpdateStatus::Truncated: break;
    }
    redirect(cajui::Notice::UpdateIncomplete);
}
void SetupPortal::route() {
    if (routed_) return;
    routed_ = true;
    server_.collectHeaders(CollectedHeaders, 1);
    server_.on("/", HTTP_GET, [this] {
        if (authorize(false)) home();
    });
    server_.on("/transmitters", HTTP_GET, [this] {
        if (authorize(false)) transmitters();
    });
    server_.on("/brokers", HTTP_GET, [this] {
        if (authorize(false)) brokers();
    });
    server_.on("/scan", HTTP_POST, [this] {
        if (!authorize(true)) return;
        scanFailures_ = 0;
        scanFailed_ = scanRetryDue_ = false;
        scan();
        redirect(cajui::Notice::None);
    });
    server_.on("/discover", HTTP_POST, [this] {
        if (!authorize(true)) return;
        discover();
        redirect(cajui::Notice::None);
    });
    server_.on("/wifi", HTTP_POST, [this] {
        if (!authorize(true)) return;
        cajui::UplinkConfig before = pending_;
        const auto error = cajui::stageWifi(pending_, server_.arg("ssid").c_str(),
                                            server_.arg("password").c_str());
        const bool changed = !sameWifi(before, pending_);
        cajui::wipe(before);
        if (error != cajui::SetupError::None) return redirect(cajui::noticeFor(error));
        saveRetry_.reset();
        if (!changed && wifi_ == cajui::WifiState::Connected && !trial_) {
            if (!savedCurrent_ && cajui::validSettings(pending_))
                return redirect(cajui::noticeFor(save()));
            return redirect(cajui::Notice::WifiUnchanged);
        }
        // Nothing is saved until the station connects with these credentials. Reconnect
        // after the response is sent: a client on the old network would lose it.
        savedCurrent_ = false;
        trial_ = reconnect_ = true;
        trialFailed_ = false;
        redirect(cajui::validUplink(pending_) ? cajui::Notice::WifiTrying
                                              : cajui::Notice::WifiStaged);
    });
    server_.on("/broker", HTTP_POST, [this] {
        if (!authorize(true)) return;
        const auto error =
            cajui::stageBroker(pending_, server_.arg("host").c_str(), server_.arg("port").c_str(),
                               server_.arg("username").c_str(), server_.arg("password").c_str());
        if (error != cajui::SetupError::None) return redirect(cajui::noticeFor(error));
        saveRetry_.reset();
        savedCurrent_ = false;
        if (!verified()) return redirect(cajui::Notice::BrokerStaged);
        redirect(cajui::noticeFor(save()));
    });
    server_.on("/revoke", HTTP_POST, [this] {
        if (!authorize(true)) return;
        uint64_t node = 0;
        uint64_t generation = 0;
        if (!parseId(server_.arg("node"), node) || !parseId(server_.arg("generation"), generation))
            return redirect(cajui::Notice::UnknownTransmitter);
        if (server_.arg("confirm") != "1") {
            if (!cajui::renderRevoke(node, generation, session_.token(), page_, sizeof(page_)))
                return redirect(cajui::Notice::UnknownTransmitter);
            server_.send(HttpOk, "text/html; charset=utf-8", page_);
            return;
        }
        auto result = cajui::Result::Invalid;
        const auto access = whileRadioIdle([&] { result = store_.revoke(node, generation); });
        if (access != cajui::RadioAccessState::Listening)
            return redirect(access == cajui::RadioAccessState::Stopped ? cajui::Notice::RadioStopped
                                                                       : cajui::Notice::RadioBusy);
        Serial.printf("CJAPP SETUP revoke node=%016" PRIx64 " result=%u\n", node, unsigned(result));
        redirect(result == cajui::Result::Ok ? cajui::Notice::Revoked
                                             : cajui::Notice::RevokeFailed);
    });
    server_.on("/pair/open", HTTP_POST, [this] {
        if (!authorize(true)) return;
        if (!pairing_) return redirect(cajui::Notice::PairingUnavailable);
        {
            Locked held(lock_);
            pairing_->open();
        }
        Serial.println("CJAPP PAIR window_open");
        redirect(cajui::Notice::PairingOpened);
    });
    server_.on("/pair/stop", HTTP_POST, [this] {
        if (!authorize(true)) return;
        if (pairing_) {
            Locked held(lock_);
            pairing_->close();
        }
        redirect(cajui::Notice::PairingStopped);
    });
    server_.on("/pair/add", HTTP_POST, [this] {
        if (!authorize(true)) return;
        uint64_t node = 0;
        if (!pairing_ || !parseId(server_.arg("node"), node))
            return redirect(cajui::Notice::UnknownTransmitter);
        cajui::Result result = cajui::Result::Invalid;
        {
            Locked held(lock_);
            result = pairing_->accept(node);
        }
        Serial.printf("CJAPP PAIR accept node=%016" PRIx64 " result=%u\n", node, unsigned(result));
        redirect(result == cajui::Result::Ok         ? cajui::Notice::OfferSent
                 : result == cajui::Result::Conflict ? cajui::Notice::AddConflict
                                                     : cajui::Notice::AddFailed);
    });
    server_.on("/update", HTTP_POST, [this] { uploaded(); }, [this] { upload(); });
    server_.on("/close", HTTP_POST, [this] {
        if (!authorize(true)) return;
        cajui::renderClosed(page_, sizeof(page_));
        server_.send(HttpOk, "text/html; charset=utf-8", page_);
        closing_ = true;
        closeAt_ = millis() + CloseDelayMs;
    });
    // Phones probe fixed URLs to detect captive portals; redirecting them opens this page.
    server_.onNotFound([this] {
        if (server_.client().localIP() != WiFi.softAPIP()) { // Nothing on the home network.
            server_.send(HttpNotFound, "text/plain", "Not found");
            return;
        }
        server_.sendHeader("Location", String("http://") + address_ + "/");
        server_.send(HttpFound);
    });
}
} // namespace board
#endif
