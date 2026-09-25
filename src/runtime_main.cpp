#ifdef CAJUI_RUNTIME_ROLE
#include <Arduino.h>
#include <DHT.h>
#include <bootloader_random.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_system.h>
#include "cajui_nvs.h"
#include "cajui_application.h"
#include "board/sx1262_radio.h"
#include "board/admin_console.h"
#include "board/display.h"
#include "cajui_provisioning.h"
#include "cajui_pairing.h"
#include "cajui_setup.h"
#if CAJUI_RUNTIME_ROLE == 2
#include "cajui_uplink.h"
#include "board/mqtt_uplink.h"
#include "board/setup_portal.h"
#endif

namespace {
#if CAJUI_RUNTIME_ROLE == 1
constexpr uint32_t SampleSeconds = 300, SensorWarmupMs = 2200;
#endif
constexpr uint32_t SerialBaud = 115200;
class BoardClock final : public cajui::Clock {
public:
    uint32_t nowMs() const override { return millis(); }
};
class BoardJitter final : public cajui::Jitter {
public:
    bool between(uint32_t minimum, uint32_t maximum, uint32_t& value) override {
        if (minimum > maximum) return false;
        const uint64_t width = uint64_t(maximum) - minimum + 1;
        value = uint32_t(minimum + uint64_t(esp_random()) % width);
        return true;
    }
};
// Keys, nonces and generations. The hardware RNG is only a true RNG while Wi-Fi/BT run
// or with the SAR ADC entropy source enabled; the transmitter never starts Wi-Fi.
class BoardEntropy final : public cajui::Entropy {
public:
    bool fill(uint8_t* output, size_t size) override {
#if CAJUI_RUNTIME_ROLE == 1
        bootloader_random_enable();
        esp_fill_random(output, size);
        bootloader_random_disable();
#else
        esp_fill_random(output, size); // Pairing runs from the setup page, with Wi-Fi on.
#endif
        return true;
    }
};
BoardEntropy entropy;
constexpr uint8_t PairButton = 0; // PRG.
constexpr uint32_t PairHoldMs = 3000, PairBlinkMs = 100;
board::Sx1262Radio radio;
cajui::NvsBlob blob;
BoardClock clockSource;
BoardJitter jitter;
cajui::SendController sender(radio, clockSource, jitter);
#if CAJUI_RUNTIME_ROLE == 2
cajui::ReceiverController* receiver = nullptr;
cajui::PersistentStore* receiverStore = nullptr;
cajui::NvsBlob uplinkBlob("uplink", cajui::MinUplinkSize, cajui::UplinkBlobCapacity);
board::MqttUplink uplink;
cajui::Forwarder* forwarder = nullptr;
uint32_t reportedForwards = 0, reportedRetries = 0;
bool reportedOnline = false;
board::SetupPortal* portal = nullptr;
cajui::PairingHost* pairingHost = nullptr;
cajui::LongPress setupButton(board::SetupHoldMs);
// Called by the setup page after saving: restarts MQTT and forwarding without a reboot.
bool applyUplink(const cajui::UplinkConfig& settings) {
    if (!uplink.startMqtt(settings, receiverStore->device())) return false;
    if (forwarder) return forwarder->setSource(settings.username);
    static cajui::Forwarder instance(uplink, clockSource, *receiverStore, settings.username);
    forwarder = &instance;
    return forwarder->state() != cajui::ForwardState::Failed;
}
// Forwarding is optional: without a stored configuration the receiver keeps queueing.
void startForwarding(cajui::PersistentStore& store) {
    static cajui::UplinkConfig settings;
    const auto loaded =
        uplinkBlob.begin() ? cajui::loadUplink(uplinkBlob, settings) : cajui::ReadResult::Error;
    if (loaded != cajui::ReadResult::Ok) {
        Serial.println(loaded == cajui::ReadResult::Missing ? "CJAPP UPLINK disabled"
                                                            : "CJAPP UPLINK config_error");
        return;
    }
    if (uplink.begin(settings, store.device())) {
        static cajui::Forwarder instance(uplink, clockSource, store, settings.username);
        forwarder = &instance;
        Serial.printf("CJAPP UPLINK started host=%s port=%u source=%s\n", settings.host,
                      unsigned(settings.port), settings.username);
    } else {
        Serial.println("CJAPP UPLINK start_failed");
    }
    cajui::wipe(settings);
}
void reportForwarding() {
    if (uplink.connected() != reportedOnline) {
        reportedOnline = uplink.connected();
        Serial.printf("CJAPP UPLINK %s wifi=%u\n", reportedOnline ? "online" : "offline",
                      unsigned(uplink.wifiConnected()));
    }
    if (forwarder->forwarded() != reportedForwards) {
        reportedForwards = forwarder->forwarded();
        Serial.printf("CJAPP FORWARD puback total=%u queued=%u\n", unsigned(reportedForwards),
                      unsigned(receiverStore->queued()));
    }
    if (forwarder->retries() != reportedRetries) {
        reportedRetries = forwarder->retries();
        Serial.printf("CJAPP FORWARD retry total=%u\n", unsigned(reportedRetries));
    }
}
#endif
uint32_t bootAt = 0;
bool running = false;
#if CAJUI_RUNTIME_ROLE == 1
cajui::PairingClient* pairingClient = nullptr;
cajui::LongPress pairButton(PairHoldMs);
// True when PRG is held continuously for PairHoldMs after boot or wake.
bool pairButtonHeld() {
    pinMode(PairButton, INPUT_PULLUP);
    const uint32_t start = millis();
    while (digitalRead(PairButton) == LOW)
        if (millis() - start >= PairHoldMs) return true;
    return false;
}
#endif
// Admin mode keeps the radio in reset and serves only the USB console.
bool adminMode = false, restartPending = false;
cajui::Provisioning* provisioning = nullptr;
board::Console* console = nullptr;
uint64_t deviceId() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return 0;
    uint64_t id = 0;
    for (auto byte : mac) id = (id << 8) | byte;
    return id;
}
void output(uint8_t pin, uint8_t level) {
    digitalWrite(pin, level);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}
#if CAJUI_RUNTIME_ROLE == 1
void hold(uint8_t pin, uint8_t level) {
    output(pin, level);
    gpio_hold_en(gpio_num_t(pin));
}
void sleepNode() {
    // Reset provides a fallback if the radio driver cannot confirm sleep.
    if (!radio.sleep()) hold(board::RadioReset, LOW);
    pinMode(board::SensorData, INPUT);
    hold(board::Vext, HIGH);
    hold(board::Led, LOW);
    hold(board::RadioCs, HIGH);
    gpio_deep_sleep_hold_en();
    // PRG wakes the node so a long press can start radio pairing.
    rtc_gpio_pullup_en(gpio_num_t(PairButton));
    rtc_gpio_pulldown_dis(gpio_num_t(PairButton));
    esp_sleep_enable_ext0_wakeup(gpio_num_t(PairButton), 0);
    const uint32_t elapsed = millis() - bootAt;
    const uint32_t period = SampleSeconds * 1000;
    const uint32_t remaining = elapsed < period ? period - elapsed : period;
    esp_sleep_enable_timer_wakeup(uint64_t(remaining) * 1000);
    Serial.println("CJAPP SLEEP");
    Serial.flush();
    esp_deep_sleep_start();
}
#endif
void halt(const char* reason) {
    Serial.print("CJAPP STOP ");
    Serial.println(reason);
    radio.sleep();
    output(board::RadioReset, LOW);
    output(board::Vext, HIGH);
    running = false;
}
}
void setup() {
    bootAt = millis();
    gpio_deep_sleep_hold_dis();
    for (auto pin : {board::Vext, board::Led, board::RadioCs, board::RadioReset})
        gpio_hold_dis(gpio_num_t(pin));
    output(board::RadioReset, LOW);
    output(board::RadioCs, HIGH);
    output(board::Vext, HIGH);
    output(board::Led, LOW);
    Serial.begin(SerialBaud);
    // USB serial carries the CJ1 console. ESP-IDF components log from other tasks and
    // interleaved an MQTT error into the middle of a HELLO reply on hardware; CJAPP lines
    // remain the diagnostic output.
    esp_log_level_set("*", ESP_LOG_NONE);
    rtc_gpio_deinit(gpio_num_t(PairButton));
    const board::BootRequest request = board::takeBootRequest();
    const bool requested = request == board::BootRequest::Admin;
    static cajui::PersistentStore store(blob, cajui::Role(CAJUI_RUNTIME_ROLE), deviceId());
    const bool mounted = blob.begin() && store.mount();
    bool enrolled = mounted && store.network() && store.profile() == board::RadioProfile;
#if CAJUI_RUNTIME_ROLE == 1
    cajui::Binding binding{};
    enrolled = enrolled && store.binding(store.device(), binding);
    cajui::AtomicBlob* settings = nullptr;
    const bool pairing =
        mounted && !requested && (request == board::BootRequest::Pair || pairButtonHeld());
#else
    // A receiver runs without bindings so radio pairing can create the first one.
    enrolled = mounted && (!store.network() || store.profile() == board::RadioProfile);
    cajui::AtomicBlob* settings = uplinkBlob.begin() ? &uplinkBlob : nullptr;
#endif
    // Without enrollment, or with unusable storage, administer over USB instead of halting.
#if CAJUI_RUNTIME_ROLE == 1
    adminMode = requested || (!enrolled && !pairing);
#else
    adminMode = requested || !enrolled;
#endif
    static cajui::Provisioning commands(store, esp_random(), settings,
                                        adminMode ? cajui::ConsoleMode::Admin
                                                  : cajui::ConsoleMode::Operation);
    static board::Console usb(commands);
    provisioning = &commands;
    console = &usb;
    if (adminMode) {
        Serial.printf("CJAPP ADMIN reason=%s\n", requested  ? "requested"
                                                 : !mounted ? "storage"
                                                            : "not_enrolled");
        return;
    }
#if CAJUI_RUNTIME_ROLE == 1
    if (pairing) {
        if (!radio.begin()) {
            halt("RADIO_INIT");
            return;
        }
        static cajui::PairingClient client(radio, clockSource, jitter, store, entropy);
        pairingClient = &client;
        Serial.printf("CJAPP PAIR start node=%016llx\n",
                      static_cast<unsigned long long>(store.device()));
        if (!client.start()) {
            halt("PAIR_START");
            return;
        }
        running = true;
        return;
    }
#endif
    if (!radio.begin()) {
        halt("RADIO_INIT");
        return;
    }
#if CAJUI_RUNTIME_ROLE == 1
    output(board::Vext, LOW);
    // Hold the shared-rail display in reset; this application does not initialize it.
    output(board::OledReset, LOW);
    DHT sensor(board::SensorData, DHT22);
    sensor.begin();
    delay(SensorWarmupMs);
    const float humidity = sensor.readHumidity();
    const float temperature = sensor.readTemperature();
    const auto sample = cajui::climateSample(temperature, humidity, SampleSeconds);
    pinMode(board::SensorData, INPUT);
    output(board::Vext, HIGH);
    Serial.printf("CJAPP SAMPLE temperature_status=%u humidity_status=%u\n",
                  unsigned(sample.readings[0].status), unsigned(sample.readings[1].status));
    if (sender.start(binding, sample, store) != cajui::StartResult::Started) {
        halt("SEND_START");
        return;
    }
#else
    static cajui::ReceiverController controller(radio, clockSource, store);
    receiver = &controller;
    receiverStore = &store;
    if (!receiver->start()) {
        halt("RECEIVE_START");
        return;
    }
    Serial.printf("CJAPP RECEIVER queued=%u\n", unsigned(store.queued()));
    startForwarding(store);
    pinMode(board::SetupButton, INPUT_PULLUP);
    static cajui::PairingHost host(store, entropy, clockSource);
    pairingHost = &host;
    controller.setPairing(&host);
    static board::SetupPortal setupPortal(store, uplink, uplinkBlob, applyUplink);
    setupPortal.setPairing(&host);
    portal = &setupPortal;
#endif
    running = true;
}
void loop() {
    // The restart waits until the radio is idle; admin mode and halted devices restart now.
    if (console && console->poll()) restartPending = true;
    if (adminMode || !running) {
        if (restartPending) board::restartFor(*provisioning);
#if CAJUI_RUNTIME_ROLE == 1
        // An unenrolled node waits in admin mode; a long press starts radio pairing.
        if (pairButton.update(digitalRead(PairButton) == LOW, millis()))
            board::restartInto(board::BootRequest::Pair);
#endif
        delay(1);
        return;
    }
#if CAJUI_RUNTIME_ROLE == 1
    if (pairingClient) {
        pairingClient->poll();
        digitalWrite(board::Led, (millis() / PairBlinkMs) % 2 ? HIGH : LOW);
        const auto state = pairingClient->state();
        if (state == cajui::ClientState::Paired || state == cajui::ClientState::Failed) {
            Serial.printf("CJAPP PAIR %s receiver=%016llx\n",
                          state == cajui::ClientState::Paired ? "paired" : "failed",
                          static_cast<unsigned long long>(pairingClient->receiver()));
            digitalWrite(board::Led, LOW);
            radio.sleep();
            board::restartInto(board::BootRequest::None);
        }
        if (restartPending) board::restartFor(*provisioning);
        delay(1);
        return;
    }
#endif
    if (running) {
#if CAJUI_RUNTIME_ROLE == 1
        sender.poll();
        if (!sender.active()) {
            const auto& report = sender.report();
            Serial.printf("CJAPP DELIVERY completion=%u attempts=%u\n", unsigned(report.completion),
                          unsigned(report.attempts));
            if (restartPending) board::restartFor(*provisioning);
            sleepNode();
        }
#else
        const auto before = receiver->state();
        receiver->poll();
        if (before == cajui::ReceiverState::Listening &&
            receiver->state() == cajui::ReceiverState::Acknowledging)
            Serial.printf("CJAPP ACCEPT result=%u queued=%u\n", unsigned(receiver->lastResult()),
                          unsigned(receiverStore->queued()));
        const bool listening = receiver->state() == cajui::ReceiverState::Listening;
        if (receiver->state() == cajui::ReceiverState::Failed) {
            halt("RECEIVER");
        } else if (forwarder) {
            // Only while listening: forwarding writes flash and must not delay an ACK.
            forwarder->poll(listening);
            reportForwarding();
            if (forwarder->state() == cajui::ForwardState::Failed) halt("FORWARDER");
        }
        if (running && setupButton.update(digitalRead(board::SetupButton) == LOW, millis()))
            portal->active() ? portal->close() : portal->open();
        if (running) portal->poll(listening);
        if (restartPending && listening) board::restartFor(*provisioning);
        pairingHost->poll();
#endif
    }
    delay(1);
}
#endif
