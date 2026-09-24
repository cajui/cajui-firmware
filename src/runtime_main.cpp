#ifdef CAJUI_RUNTIME_ROLE
#include <Arduino.h>
#include <DHT.h>
#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "cajui_nvs.h"
#include "cajui_application.h"
#include "board/sx1262_radio.h"
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
    static cajui::PersistentStore store(blob, cajui::Role(CAJUI_RUNTIME_ROLE), deviceId());
    if (!blob.begin() || !store.mount() || !store.network() ||
        store.profile() != board::RadioProfile) {
        halt("STORAGE_OR_PROFILE");
        return;
    }
#if CAJUI_RUNTIME_ROLE == 1
    cajui::Binding binding{};
    if (!store.binding(store.device(), binding)) {
        halt("NOT_ENROLLED");
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
    constexpr uint8_t DisplayReset = 21;
    output(DisplayReset, LOW);
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
    static board::SetupPortal setupPortal(store, uplink, uplinkBlob, applyUplink);
    portal = &setupPortal;
#endif
    running = true;
}
void loop() {
    if (running) {
#if CAJUI_RUNTIME_ROLE == 1
        sender.poll();
        if (!sender.active()) {
            const auto& report = sender.report();
            Serial.printf("CJAPP DELIVERY completion=%u attempts=%u\n", unsigned(report.completion),
                          unsigned(report.attempts));
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
#endif
    }
    delay(1);
}
#endif
