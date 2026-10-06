# Wireless Stick Lite V3 transmitter

The experimental `runtime_tx_stick_lite` image reads one SHT4x over I2C. The existing
`runtime_tx` and `runtime_rx` images keep their WiFi LoRa 32 V3 behavior. All use the same
radio profile, enrollment protocol and climate metrics; no receiver schema change is needed.

## Wiring

| Wireless Stick Lite V3 | SHT4x breakout |
| --- | --- |
| Ve | VIN (3.3 V compatible input) |
| GND | GND |
| GPIO33 | SDA |
| GPIO34 | SCL |

Qwiic/STEMMA QT adapters are optional passive wiring. Direct wires work identically.
The board profile selects address `0x44` and 100 kHz. The sensor driver supports SHT4x
addresses `0x44` through `0x46`, but this image configures one sensor, not automatic discovery.
GPIO35 is the onboard LED and must not be used for I2C.

Vext is enabled through GPIO36 low, with 100 ms supply settling. Each high-repeatability
reading waits 10 ms and verifies both CRC bytes. Any bus/CRC failure reports sensor errors,
not zero readings. The I2C bus is ended and both signal pins floated before turning Vext off,
also on failure and before deep sleep. External pull-ups must use the switched sensor supply.
There is no heater activation. No OLED reset pin is driven on this board.

## Build and scope

```sh
pio run -e runtime_tx_stick_lite
```

This target reuses the WiFi LoRa 32 V3 ESP32-S3FN8 compiler/flash settings, with an explicit
Stick Lite board profile. Used radio pins are common to both boards; I2C pins never rely on
Arduino defaults. The battery divider handling remains provisional, as on the existing
application: validate voltage against a meter and measure sleep consumption on the actual
board. See [radio applications](radio-applications.md).

Install over USB, selecting this image explicitly. It is **not** included in the web
installer or signed release pipeline yet. Existing signed packages identify a role but not a
board; do not use a Heltec WiFi LoRa 32 image as a Stick Lite update. A factory board requires
initializing the `cajui` storage partition per the new-board installation procedure in
[updates](updates.md); updating an enrolled board must preserve that partition.

Host tests cover I2C NACK, short/oversized response, CRC corruption, conversion and humidity
clamping. CI compiles both previous applications and this target, and runs board lint.
Hardware integration and power measurements must be recorded separately; a passing host test
does not establish RF, sleep current or long-term reliability.

Reference: [Sensirion SHT4x datasheet](https://sensirion.com/resource/datasheet/sht4x).
