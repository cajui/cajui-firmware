// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
// Transmitter image: wakes, reads the DHT22, delivers one sample and sleeps; or pairs by
// radio when its button is held. See docs/radio-applications.md.
#include <Arduino.h>
#include <DHT.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <memory>
#include "cajui_application.h"
#include "cajui_device.h"
#include "cajui_nvs.h"
#include "cajui_pairing.h"
#include "cajui_provisioning.h"
#include "cajui_setup.h"
#include "board/admin_console.h"
#include "board/common.h"
#include "board/display.h"
#include "board/ota.h"
#include "board/sx1262_radio.h"

namespace {
using namespace board;
constexpr uint32_t SampleSeconds = 300, SensorWarmupMs = 2200;
constexpr uint8_t PairButton = 0; // PRG.
constexpr uint32_t PairHoldMs = 3000, PairBlinkMs = 100;

class TransmitterApp {
public:
    TransmitterApp() : store_(records_, cajui::Role::Transmitter, deviceId()) {}
    void setup();
    void loop();

private:
    enum class Mode { Admin, Pairing, Sending };
    BoardClock clock_;
    BoardJitter jitter_;
    BoardEntropy entropy_{false}; // Wi-Fi never runs on the transmitter.
    Sx1262Radio radio_;
    cajui::NvsRecords records_;
    cajui::RecordBlob radioBlob_{records_, "radio", cajui::RadioRecordSize, cajui::RadioRecordSize};
    cajui::PersistentStore store_;
    cajui::SendController sender_{radio_, clock_, jitter_};
    std::unique_ptr<cajui::PairingClient> pairing_;
    std::unique_ptr<cajui::Provisioning> commands_;
    std::unique_ptr<Console> console_;
    cajui::LongPress pairButton_{PairHoldMs};
    Mode mode_ = Mode::Admin;
    bool restartPending_ = false;
    uint32_t bootAt_ = 0;
    int8_t configuredPower_ = cajui::DefaultPowerDbm, power_ = cajui::DefaultPowerDbm;
    int8_t startPower();
    void rememberPower(const cajui::SendReport&);
    bool pairButtonHeld();
    void sample(cajui::Binding&);
    [[noreturn]] void sleepFor(uint32_t ms);
    [[noreturn]] void fault(const char* reason);
};

// Power across deep sleep, in plain RTC variables: a type with member initializers would
// be constructed at boot and erase them. The magic guards against RTC memory contents
// after power-on, when the node starts again from the configured power.
constexpr uint32_t PowerMagic = 0x50575253; // "PWRS"
RTC_NOINIT_ATTR uint32_t powerMagic;
RTC_NOINIT_ATTR int8_t powerDbm;
RTC_NOINIT_ATTR uint8_t powerMissed;
RTC_NOINIT_ATTR int8_t powerCeiling;
bool powerRemembered() {
    return powerMagic == PowerMagic && esp_reset_reason() != ESP_RST_POWERON;
}
cajui::PowerState rememberedPower() {
    cajui::PowerState state{};
    state.dbm = powerDbm;
    state.missed = powerMissed;
    state.ceiling = powerCeiling;
    return state;
}
int8_t TransmitterApp::startPower() {
    if (cajui::loadPower(radioBlob_, configuredPower_) == cajui::ReadResult::Error)
        Serial.println("CJAPP POWER config_error"); // The bench default applies.
    if (!powerRemembered()) powerMagic = 0;
    const cajui::PowerState state = rememberedPower();
    return cajui::currentPower(powerMagic == PowerMagic ? &state : nullptr, configuredPower_);
}
void TransmitterApp::rememberPower(const cajui::SendReport& report) {
    cajui::PowerState now{};
    if (powerMagic == PowerMagic) now = rememberedPower();
    now.dbm = cajui::currentPower(powerMagic == PowerMagic ? &now : nullptr, configuredPower_);
    const cajui::PowerState next =
        cajui::nextPower(now, configuredPower_,
                         report.completion == cajui::Completion::Acknowledged, report.powerCommand);
    powerDbm = next.dbm;
    powerMissed = next.missed;
    powerCeiling = next.ceiling;
    powerMagic = PowerMagic;
}
// True when PRG is held continuously for PairHoldMs after boot or wake.
bool TransmitterApp::pairButtonHeld() {
    pinMode(PairButton, INPUT_PULLUP);
    const uint32_t start = millis();
    while (digitalRead(PairButton) == LOW)
        if (millis() - start >= PairHoldMs) return true;
    return false;
}
void hold(uint8_t pin, uint8_t level) {
    output(pin, level);
    gpio_hold_en(gpio_num_t(pin));
}
void TransmitterApp::sleepFor(uint32_t ms) {
    // Reset provides a fallback if the radio driver cannot confirm sleep.
    if (!radio_.sleep()) hold(board::RadioReset, LOW);
    pinMode(board::SensorData, INPUT);
    hold(board::Vext, HIGH);
    hold(board::Led, LOW);
    hold(board::RadioCs, HIGH);
    gpio_deep_sleep_hold_en();
    // PRG wakes the node so a long press can start radio pairing.
    rtc_gpio_pullup_en(gpio_num_t(PairButton));
    rtc_gpio_pulldown_dis(gpio_num_t(PairButton));
    esp_sleep_enable_ext0_wakeup(gpio_num_t(PairButton), 0);
    esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000);
    Serial.println("CJAPP SLEEP");
    Serial.flush();
    esp_deep_sleep_start();
    while (true) {
    }
}
// A fault sleeps rather than idling awake: the battery lasts, and the next wake retries.
void TransmitterApp::fault(const char* reason) {
    const uint32_t faults = recordFault();
    const uint32_t delay = cajui::retryDelayMs(faults);
    Serial.printf("CJAPP STOP %s faults=%u retry_s=%u\n", reason, unsigned(faults),
                  unsigned(delay / 1000));
    sleepFor(delay);
}
void TransmitterApp::sample(cajui::Binding& binding) {
    output(board::Vext, LOW);
    // Hold the shared-rail display in reset; this application does not initialize it.
    output(board::OledReset, LOW);
    DHT sensor(board::SensorData, DHT22);
    sensor.begin();
    delay(SensorWarmupMs);
    const float humidity = sensor.readHumidity();
    const float temperature = sensor.readTemperature();
    const auto data = cajui::climateSample(temperature, humidity, SampleSeconds);
    pinMode(board::SensorData, INPUT);
    output(board::Vext, HIGH);
    Serial.printf("CJAPP SAMPLE temperature_status=%u humidity_status=%u power=%d\n",
                  unsigned(data.readings[0].status), unsigned(data.readings[1].status),
                  int(power_));
    if (sender_.start(binding, data, store_) != cajui::StartResult::Started) fault("SEND_START");
}
void TransmitterApp::setup() {
    bootAt_ = millis();
    startBoard();
    reportFirmware();
    rtc_gpio_deinit(gpio_num_t(PairButton));
    const cajui::BootRequest request = takeBootRequest();
    const bool mounted = records_.begin() && store_.mount();
    // The button is read (for up to three seconds) only when pairing is possible.
    const bool held = mounted && request == cajui::BootRequest::None && pairButtonHeld();
    const auto decision = cajui::decideBoot(store_, mounted, RadioProfile, request, held);
    commands_.reset(new cajui::Provisioning(store_, esp_random(), nullptr,
                                            decision.mode == cajui::BootMode::Admin
                                                ? cajui::ConsoleMode::Admin
                                                : cajui::ConsoleMode::Operation,
                                            &radioBlob_));
    console_.reset(new Console(*commands_));
    if (decision.mode == cajui::BootMode::Admin) {
        mode_ = Mode::Admin;
        pinMode(PairButton, INPUT_PULLUP);
        Serial.printf("CJAPP ADMIN reason=%s\n", cajui::reasonName(decision.reason));
        return;
    }
    power_ = startPower();
    if (!radio_.begin(power_)) fault("RADIO_INIT");
    if (decision.mode == cajui::BootMode::Pair) {
        mode_ = Mode::Pairing;
        pairing_.reset(new cajui::PairingClient(radio_, clock_, jitter_, store_, entropy_));
        Serial.printf("CJAPP PAIR start node=%016llx\n",
                      static_cast<unsigned long long>(store_.device()));
        if (!pairing_->start()) fault("PAIR_START");
    } else {
        mode_ = Mode::Sending;
        cajui::Binding binding{};
        store_.binding(store_.device(), binding);
        sample(binding);
    }
    enableLoopWDT(); // The loop never blocks: a hang restarts the transmitter.
}
void TransmitterApp::loop() {
    // The restart waits until the radio is idle; admin mode restarts now.
    if (console_ && console_->poll()) restartPending_ = true;
    if (mode_ == Mode::Admin) {
        if (restartPending_) restartFor(*commands_);
        // An unenrolled node waits in admin mode; a long press starts radio pairing.
        if (pairButton_.update(digitalRead(PairButton) == LOW, millis()))
            restartInto(cajui::BootRequest::Pair);
        delay(1);
        return;
    }
    if (mode_ == Mode::Pairing) {
        pairing_->poll();
        digitalWrite(board::Led, (millis() / PairBlinkMs) % 2 ? HIGH : LOW);
        const auto state = pairing_->state();
        if (state == cajui::ClientState::Paired || state == cajui::ClientState::Failed) {
            Serial.printf("CJAPP PAIR %s receiver=%016llx\n",
                          state == cajui::ClientState::Paired ? "paired" : "failed",
                          static_cast<unsigned long long>(pairing_->receiver()));
            digitalWrite(board::Led, LOW);
            radio_.sleep();
            restartInto(cajui::BootRequest::None);
        }
        if (restartPending_) restartFor(*commands_);
        delay(1);
        return;
    }
    sender_.poll();
    if (!sender_.active()) {
        const auto& report = sender_.report();
        Serial.printf("CJAPP DELIVERY completion=%u attempts=%u power_command=%d\n",
                      unsigned(report.completion), unsigned(report.attempts),
                      int(report.powerCommand));
        rememberPower(report);
        clearFaults();     // A full cycle ran: storage and radio work.
        confirmFirmware(); // So a freshly installed image is kept.
        if (restartPending_) restartFor(*commands_);
        const uint32_t elapsed = millis() - bootAt_;
        const uint32_t period = SampleSeconds * 1000;
        sleepFor(elapsed < period ? period - elapsed : period);
    }
    delay(1);
}
TransmitterApp* app = nullptr;
} // namespace

void setup() {
    static TransmitterApp instance;
    app = &instance;
    app->setup();
}
void loop() {
    app->loop();
}
#endif
