#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 2
#include "setup_portal.h"
#include "display.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

namespace board {
namespace {
constexpr uint32_t IdleMs = 10UL * 60 * 1000, WifiTimeoutMs = 20000, CloseDelayMs = 1000;
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
constexpr uint32_t DiscoveryStack = 4096;
constexpr int IdDigits = 16;
bool parseId(const String& text, uint64_t& value) {
    if (text.length() != IdDigits) return false;
    char* end = nullptr;
    value = std::strtoull(text.c_str(), &end, 16);
    return end && !*end && value;
}
}
void SetupPortal::open() {
    if (active_) return;
    cajui::setupSsid(store_.device(), ssid_, sizeof(ssid_));
    saved_ = cajui::loadUplink(blob_, pending_) == cajui::ReadResult::Ok;
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid_); // Open network: see the TODO in setup_portal.h.
    const IPAddress address = WiFi.softAPIP();
    dns_.start(DnsPort, "*", address); // Captive portal: every name resolves here.
    route();
    server_.begin();
    active_ = true;
    closing_ = false;
    notice_ = nullptr;
    wifi_ = MqttUplink::wifiConnected() ? cajui::WifiState::Connected
            : saved_                    ? cajui::WifiState::Connecting
                                        : cajui::WifiState::Idle;
    wifiSince_ = millis();
    touch();
    scan();
    if (wifi_ == cajui::WifiState::Connected) {
        startMdns();
        settling_ = true;
        settleAt_ = millis();
    }
    char qr[cajui::QrCapacity]{};
    cajui::wifiQr(ssid_, qr, sizeof(qr));
    const bool shown = showSetup(ssid_, qr, address.toString().c_str());
    Serial.printf("CJAPP SETUP open ssid=%s display=%u\n", ssid_, unsigned(shown));
}
void SetupPortal::close() {
    if (!active_) return;
    server_.stop();
    dns_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(saved_ ? WIFI_STA : WIFI_OFF);
    displayOff();
    cajui::wipe(pending_);
    active_ = false;
    Serial.println("CJAPP SETUP closed");
}
void SetupPortal::poll(bool quiet) {
    if (!active_) return;
    dns_.processNextRequest();
    if (quiet) server_.handleClient();
    if (reconnect_) {
        reconnect_ = false;
        if (scanning_) {
            esp_wifi_scan_stop();
            WiFi.scanDelete();
            scanning_ = false;
        }
        MqttUplink::startWifi(pending_.ssid, pending_.wifiPassword);
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
    if ((closing_ && int32_t(now - closeAt_) >= 0) || uint32_t(now - lastActivity_) >= IdleMs)
        close();
}
void SetupPortal::trackWifi() {
    const bool connected = MqttUplink::wifiConnected();
    if (connected && wifi_ != cajui::WifiState::Connected) {
        wifi_ = cajui::WifiState::Connected;
        Serial.printf("CJAPP SETUP wifi=connected address=%s\n", WiFi.localIP().toString().c_str());
        startMdns();
        settleAt_ = millis() + SettleMs;
        settling_ = true;
    } else if (!connected && wifi_ == cajui::WifiState::Connected) {
        wifi_ = cajui::WifiState::Connecting;
        wifiSince_ = millis();
    } else if (wifi_ == cajui::WifiState::Connecting &&
               uint32_t(millis() - wifiSince_) >= WifiTimeoutMs) {
        wifi_ = cajui::WifiState::Failed;
        scan(); // Offer the network list again so the user can correct the choice.
    }
}
void SetupPortal::startMdns() {
    if (mdns_) return;
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
    if (!mdns_ || !searching_.compare_exchange_strong(idle, true)) return;
    // mDNS queries block for seconds; run them off the radio loop.
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
void SetupPortal::redirect(const char* notice) {
    notice_ = notice;
    server_.sendHeader("Location", "/");
    server_.send(303);
}
void SetupPortal::save() {
    if (!cajui::validUplink(pending_)) {
        saved_ = false;
        return;
    }
    saved_ = cajui::saveUplink(blob_, pending_) && apply_(pending_);
    Serial.printf("CJAPP SETUP saved ok=%u\n", unsigned(saved_));
}
void SetupPortal::home() {
    touch();
    cajui::SetupView view{};
    view.device = store_.device();
    view.wifi = wifi_;
    const String network = WiFi.SSID();
    const String address = WiFi.localIP().toString();
    view.wifiSsid = wifi_ == cajui::WifiState::Connected ? network.c_str() : pending_.ssid;
    view.address = address.c_str();
    view.brokerOnline = uplink_.connected();
    view.staged = &pending_;
    view.saved = saved_;
    view.queued = store_.queued();
    view.networks = networks_;
    view.networkCount = networkCount_;
    view.scanning = scanning_;
    view.searching = searching_.load();
    view.brokers = brokers_;
    view.brokerCount = view.searching ? 0 : brokerCount_.load();
    view.transmitters = transmitters_;
    view.transmitterCount = store_.list(transmitters_, cajui::BindingCapacity);
    const String host = server_.arg("host");
    const long port = server_.arg("port").toInt();
    if (host.length() && host.length() <= cajui::HostCapacity && port > 0 && port <= 65535) {
        view.prefillHost = host.c_str();
        view.prefillPort = uint16_t(port);
    }
    view.notice = notice_;
    notice_ = nullptr;
    if (!cajui::renderSetup(view, page_, sizeof(page_))) {
        server_.send(500, "text/plain", "Page too large");
        return;
    }
    server_.send(200, "text/html; charset=utf-8", page_);
}
void SetupPortal::route() {
    if (routed_) return;
    routed_ = true;
    server_.on("/", HTTP_GET, [this] { home(); });
    server_.on("/scan", HTTP_GET, [this] {
        scan();
        redirect(nullptr);
    });
    server_.on("/discover", HTTP_GET, [this] {
        discover();
        redirect(nullptr);
    });
    server_.on("/wifi", HTTP_POST, [this] {
        touch();
        cajui::UplinkConfig before = pending_;
        const auto error = cajui::stageWifi(pending_, server_.arg("ssid").c_str(),
                                            server_.arg("password").c_str());
        if (error != cajui::SetupError::None) {
            cajui::wipe(before);
            return redirect(cajui::describe(error));
        }
        const bool unchanged = wifi_ == cajui::WifiState::Connected &&
                               !std::strcmp(before.ssid, pending_.ssid) &&
                               !std::strcmp(before.wifiPassword, pending_.wifiPassword);
        cajui::wipe(before);
        // Reconnect after the response is sent: a client on the home network would lose it.
        reconnect_ = !unchanged;
        save();
        redirect(unchanged ? "Wi-Fi settings unchanged."
                 : saved_  ? "Connecting with the new Wi-Fi settings. Refresh in a few seconds."
                           : "Connecting. Complete the broker section to save.");
    });
    server_.on("/broker", HTTP_POST, [this] {
        touch();
        const auto error =
            cajui::stageBroker(pending_, server_.arg("host").c_str(), server_.arg("port").c_str(),
                               server_.arg("username").c_str(), server_.arg("password").c_str());
        if (error != cajui::SetupError::None) return redirect(cajui::describe(error));
        save();
        redirect(saved_ ? "Broker saved. Forwarding restarts with these settings."
                        : "Broker staged. Complete the Wi-Fi section to save.");
    });
    server_.on("/revoke", HTTP_POST, [this] {
        touch();
        uint64_t node = 0, generation = 0;
        if (!parseId(server_.arg("node"), node) || !parseId(server_.arg("generation"), generation))
            return redirect("Unknown transmitter.");
        if (server_.arg("confirm") != "1") {
            if (!cajui::renderRevoke(node, generation, page_, sizeof(page_)))
                return redirect("Unknown transmitter.");
            server_.send(200, "text/html; charset=utf-8", page_);
            return;
        }
        const auto result = store_.revoke(node, generation);
        Serial.printf("CJAPP SETUP revoke node=%016" PRIx64 " result=%u\n", node, unsigned(result));
        redirect(result == cajui::Result::Ok ? "Transmitter revoked." : "Could not revoke.");
    });
    server_.on("/close", HTTP_POST, [this] {
        cajui::renderClosed(page_, sizeof(page_));
        server_.send(200, "text/html; charset=utf-8", page_);
        closing_ = true;
        closeAt_ = millis() + CloseDelayMs;
    });
    // Phones probe fixed URLs to detect captive portals; redirecting them opens this page.
    server_.onNotFound([this] {
        server_.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
        server_.send(302);
    });
}
} // namespace board
#endif
