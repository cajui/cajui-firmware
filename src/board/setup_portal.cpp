// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "setup_portal.h"
#include "display.h"
#include "sx1262_radio.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

namespace board {
namespace {
constexpr uint32_t WifiTimeoutMs = 20000, CloseDelayMs = 1000;
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
constexpr uint32_t TaskStack = 10240, DiscoveryStack = 4096;
constexpr uint32_t ActiveDelayMs = 2, IdleDelayMs = 20;
// A storage write waits this long for the radio to finish acknowledging; after that the
// radio is stopped or failed and the write proceeds.
constexpr uint32_t RadioIdleWaitMs = 3000, RadioIdlePollMs = 5;
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

template <typename Action> auto SetupPortal::whileRadioIdle(Action action) -> decltype(action()) {
    const uint32_t start = millis();
    lock_.take();
    while (!radioIdle_->load() && uint32_t(millis() - start) < RadioIdleWaitMs) {
        lock_.give();
        vTaskDelay(pdMS_TO_TICKS(RadioIdlePollMs));
        lock_.take();
    }
    auto result = action();
    lock_.give();
    return result;
}
bool SetupPortal::start(const std::atomic<bool>& radioIdle) {
    radioIdle_ = &radioIdle;
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
    cajui::setupSsid(store_.device(), ssid_, sizeof(ssid_));
    {
        Locked held(lock_);
        stored_ = cajui::loadUplink(blob_, pending_) == cajui::ReadResult::Ok;
    }
    running_ = cajui::UplinkConfig{};
    if (stored_) copyWifi(running_, pending_);
    savedCurrent_ = stored_;
    trial_ = trialFailed_ = reconnect_ = closing_ = false;
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid_); // Open network: see the TODO in setup_portal.h.
    const IPAddress address = WiFi.softAPIP();
    std::snprintf(address_, sizeof(address_), "%s", address.toString().c_str());
    // Wi-Fi runs now, so the hardware RNG is a true RNG for the session token.
    uint8_t random[cajui::SetupSession::TokenBytes]{};
    entropy_.fill(random, sizeof(random));
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
    if (!stored_) return;
    cajui::UplinkConfig stored{};
    bool loaded = false;
    {
        Locked held(lock_);
        loaded = cajui::loadUplink(blob_, stored) == cajui::ReadResult::Ok;
    }
    if (loaded && !sameWifi(stored, running_)) {
        MqttUplink::startWifi(stored.ssid, stored.wifiPassword);
        copyWifi(running_, stored);
    }
    if (loaded) copyWifi(pending_, stored);
    cajui::wipe(stored);
}
void SetupPortal::close() {
    if (!active_) return;
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
    if (mdnsEndPending_ && !searching_.load()) {
        MDNS.end();
        mdns_ = mdnsEndPending_ = false;
    }
    if (!active_) return;
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
        MqttUplink::startWifi(pending_.ssid, pending_.wifiPassword);
        copyWifi(running_, pending_);
        wifi_ = cajui::WifiState::Connecting;
        wifiSince_ = millis();
    }
    collectScan();
    trackWifi();
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
void SetupPortal::trackWifi() {
    const bool connected = MqttUplink::wifiConnected();
    if (connected && wifi_ != cajui::WifiState::Connected) {
        wifi_ = cajui::WifiState::Connected;
        Serial.printf("CJAPP SETUP wifi=connected address=%s\n", WiFi.localIP().toString().c_str());
        startMdns();
        settleAt_ = millis() + SettleMs;
        settling_ = true;
        if (trial_) {
            trial_ = trialFailed_ = false;
            // The new network works: save now if the broker section is complete.
            if (cajui::validUplink(pending_) && !savedCurrent_) save();
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
        scan(); // Offer the network list again so the user can correct the choice.
    }
}
void SetupPortal::startMdns() {
    if (mdns_) {
        mdnsEndPending_ = false; // Reopened before the previous session's mDNS stopped.
        return;
    }
    char name[sizeof("cajui-rx-") + 4]{};
    std::snprintf(name, sizeof(name), "cajui-rx-%04x", unsigned(store_.device() & 0xffff));
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
    scanning_ = started == WIFI_SCAN_RUNNING;
    Serial.printf("CJAPP SETUP scan start=%d\n", int(started));
}
void SetupPortal::collectScan() {
    if (!scanning_) return;
    const int16_t found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return;
    if (found == WIFI_SCAN_FAILED && uint32_t(millis() - scanStartedAt_) < ScanPatienceMs) return;
    scanning_ = false;
    networkCount_ = 0;
    Serial.printf("CJAPP SETUP scan found=%d\n", int(found));
    if (found < 0 && scanFailures_ < ScanRetries) {
        ++scanFailures_;
        scanRetryDue_ = true;
        scanRetryAt_ = millis() + ScanRetryMs;
    } else if (found >= 0) {
        scanFailures_ = 0;
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
    bool idle = false;
    if (!mdns_ || mdnsEndPending_ || !searching_.compare_exchange_strong(idle, true)) return;
    // mDNS queries block for seconds; run them off the page task.
    if (xTaskCreate(discoveryTask, "cajui-mdns", DiscoveryStack, this, 1, nullptr) != pdPASS)
        searching_.store(false);
}
void SetupPortal::discoveryTask(void* self) {
    auto* portal = static_cast<SetupPortal*>(self);
    int found = 0;
    for (int attempt = 0; attempt < DiscoveryAttempts && found <= 0; ++attempt) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(DiscoveryRetryMs));
        found = MDNS.queryService("mqtt", "tcp");
    }
    size_t count = 0;
    for (int i = 0; i < found && count < cajui::MaxBrokers; ++i) {
        const String host = MDNS.IP(i).toString();
        if (host.length() > cajui::HostCapacity || host == "0.0.0.0") continue;
        std::strcpy(portal->brokers_[count].host, host.c_str());
        portal->brokers_[count++].port = MDNS.port(i);
    }
    portal->brokerCount_.store(count);
    portal->searching_.store(false);
    vTaskDelete(nullptr);
}
// Only the access point interface, only its own address, and for a POST only the page's
// origin with the session token. Anything else is refused without touching state.
bool SetupPortal::authorize(bool post) {
    if (server_.client().localIP() != WiFi.softAPIP()) {
        server_.send(404, "text/plain", "Not found");
        return false;
    }
    if (!cajui::allowedHost(server_.hostHeader().c_str(), address_)) {
        server_.sendHeader("Location", String("http://") + address_ + "/");
        server_.send(302);
        return false;
    }
    if (post && (!cajui::allowedOrigin(server_.header("Origin").c_str(), address_) ||
                 !session_.validToken(server_.arg("token").c_str()))) {
        server_.send(403, "text/plain", "This setup page expired. Reload it and try again.");
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
    server_.send(303);
}
bool SetupPortal::save() {
    if (!cajui::validUplink(pending_)) return false;
    const bool written = whileRadioIdle([this] { return cajui::saveUplink(blob_, pending_); });
    if (written) stored_ = true;
    savedCurrent_ = written && control_.applyUplink(pending_);
    Serial.printf("CJAPP SETUP saved ok=%u\n", unsigned(savedCurrent_));
    return savedCurrent_;
}
void SetupPortal::fillPairing(cajui::PairingView& pairing) const {
    pairing.open = pairing_->state() != cajui::HostState::Closed;
    pairing.remainingSeconds = pairing_->remainingMs() / 1000;
    pairing.count = pairing_->candidateCount();
    for (size_t i = 0; i < pairing.count && i < cajui::MaxPairingCandidates; ++i) {
        pairing.nodes[i] = pairing_->candidates()[i].node;
        pairing.rssi[i] = pairing_->candidates()[i].rssi;
        pairing.conflict[i] = pairing_->candidates()[i].conflict;
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
    view.networks = networks_;
    view.networkCount = networkCount_;
    view.scanning = scanning_;
    view.searching = searching_.load();
    view.brokers = brokers_;
    view.brokerCount = view.searching ? 0 : brokerCount_.load();
    const String host = server_.arg("host");
    const long port = server_.arg("port").toInt();
    if (host.length() && host.length() <= cajui::HostCapacity && port > 0 && port <= 65535) {
        view.prefillHost = host.c_str();
        view.prefillPort = uint16_t(port);
    }
    view.notice = cajui::noticeText(cajui::parseNotice(server_.arg("n").c_str()));
    view.token = session_.token();
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
        server_.send(500, "text/plain", "Page too large");
        return;
    }
    server_.send(200, "text/html; charset=utf-8", page_);
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
        server_.send(500, "text/plain", "Page too large");
        return;
    }
    server_.send(200, "text/html; charset=utf-8", page_);
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
    server_.on("/scan", HTTP_POST, [this] {
        if (!authorize(true)) return;
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
        if (!changed && wifi_ == cajui::WifiState::Connected && !trial_) {
            if (!savedCurrent_ && cajui::validUplink(pending_)) save();
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
        savedCurrent_ = false;
        if (!verified()) return redirect(cajui::Notice::BrokerStaged);
        redirect(save() ? cajui::Notice::BrokerSaved : cajui::Notice::SaveFailed);
    });
    server_.on("/revoke", HTTP_POST, [this] {
        if (!authorize(true)) return;
        uint64_t node = 0, generation = 0;
        if (!parseId(server_.arg("node"), node) || !parseId(server_.arg("generation"), generation))
            return redirect(cajui::Notice::UnknownTransmitter);
        if (server_.arg("confirm") != "1") {
            if (!cajui::renderRevoke(node, generation, session_.token(), page_, sizeof(page_)))
                return redirect(cajui::Notice::UnknownTransmitter);
            server_.send(200, "text/html; charset=utf-8", page_);
            return;
        }
        const auto result = whileRadioIdle([&] { return store_.revoke(node, generation); });
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
    server_.on("/close", HTTP_POST, [this] {
        if (!authorize(true)) return;
        cajui::renderClosed(page_, sizeof(page_));
        server_.send(200, "text/html; charset=utf-8", page_);
        closing_ = true;
        closeAt_ = millis() + CloseDelayMs;
    });
    // Phones probe fixed URLs to detect captive portals; redirecting them opens this page.
    server_.onNotFound([this] {
        server_.sendHeader("Location", String("http://") + address_ + "/");
        server_.send(302);
    });
}
} // namespace board
#endif
