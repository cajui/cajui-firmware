# XIAO ESP32-S3 + Wio-SX1262 transmitter

The experimental `runtime_tx_xiao_s3` image targets the XIAO ESP32-S3 with the
Wio-SX1262 **B2B** expansion and one SHT4x. The `runtime_tx_xiao_s3_dht22`
image uses the same board with a DHT22/AM2302 instead. It uses the same Cajuí radio profile,
enrollment and measurement format as the Heltec transmitters. The receiver remains
`runtime_rx` on Heltec WiFi LoRa 32 V3; no Central or MQTT schema change is needed.
Mixed-board radio operation has not yet been verified on hardware.

This profile does not cover XIAO nRF52840, ESP32-S3 Plus or the header-connected
Wio expansion. The XIAO receiver role is not implemented.

## Assembly and sensor wiring

Mate the small B2B connectors with all power disconnected. The Wio side holes
expose radio signals; they are not passthrough XIAO GPIO headers. Connect the
LoRa antenna to Wio and the 2.4 GHz antenna to XIAO. Keep a suitable LoRa antenna
installed whenever either radio board is powered.

| XIAO header | SHT4x breakout |
| --- | --- |
| 3V3 | VIN compatible with 3.3 V |
| GND | GND |
| D4 / GPIO5 | SDA |
| D5 / GPIO6 | SCL |

The driver configures address `0x44` at 100 kHz, waits 10 ms per measurement and
checks both CRC bytes. Missing sensors and failed reads produce sensor-error
telemetry. QT/Qwiic adapters are optional passive wiring. Use external I2C pull-ups
to 3.3 V; no 5 V signals. No heater command is sent.

For a DHT22/AM2302 module, use the `runtime_tx_xiao_s3_dht22` image:

| XIAO header | DHT22 module |
| --- | --- |
| 3V3 | VCC |
| GND | GND |
| D0 / GPIO1 | OUT / DATA |

Follow the module's labels, not its physical pin order. DATA needs a pull-up to
3.3 V; check whether the module includes one, and add 10 kΩ if it does not.
The sensor is selected when building, not detected automatically. The DHT22
stays powered during sleep on XIAO, unlike the switched supply on Heltec.
Its standby draw and any module LED must be included in battery estimates.

## Pin assignments and sleep

| Function | ESP32-S3 GPIO |
| --- | --- |
| LoRa SCK / MISO / MOSI | 7 / 8 / 9 |
| LoRa NSS / RESET / BUSY / DIO1 | 41 / 42 / 40 / 39 |
| RF switch control | 38 |
| Wio green LED | 48, active high |
| Wio pairing button | 21, active low, external pull-up |

The radio uses DC-DC mode, DIO2 RF switching and a 1.8 V DIO3-controlled TCXO.
GPIO38 is the separate external RF control, not the SX1262 DIO2 pin itself.
SPI header pins D8–D10 are shared with the radio and are not independent GPIOs.

After delivery, firmware requests radio sleep, releases the sensor pins and holds NSS high,
RESET high, RF control low and the Wio LED low. If radio sleep fails, RESET is
held low instead. Startup restores safe output levels before releasing the holds.
The timer wakes the node for its next measurement; the Wio button uses EXT1 with
its external pull-up. Hold that button for three seconds to request pairing.
The XIAO BOOT button is not the pairing button for this profile.

GPIO21 also connects to the XIAO's active-low user LED. It remains an input, not
an output driven against the Wio button; pressing the button may light that LED.

The 3V3 supply stays on during deep sleep. The SHT4x rests between single-shot
measurements, and SDA/SCL are released without internal pull-downs. There is no
Ve rail and no external power-switch support in this profile. The breakout's own
regulator and other components can add standby current.

## Battery and installation

The original XIAO ESP32-S3 profile has no configured battery divider. Firmware
reports voltage as unknown and does not touch the Heltec ADC/control pins. The
normal interval remains five minutes; voltage-dependent low-battery scheduling
does not apply. Do not connect a battery directly to an ADC pin.

```sh
uvx --from platformio==6.2.0 pio run -e runtime_tx_xiao_s3
uvx --from platformio==6.2.0 pio run -e runtime_tx_xiao_s3_dht22
```

Build and install only the target matching the connected sensor, explicitly over USB. Follow the new-board storage procedure
in [updates](updates.md) for a factory board, and preserve enrollment/counter
storage on subsequent updates. Native USB CDC can disappear during deep sleep;
use a data cable and enter the bootloader when necessary for USB installation.

This target is excluded from signed releases and the web installer. The current
signed format identifies role, not board, so a signed Heltec transmitter package
is not a XIAO update. Model-bound packages are required before enabling that path.
The transmitter has no web firmware-upload endpoint.

## Validation scope

Host tests exercise GPIO preparation, sleep failure fallback and unavailable
battery sensing. CI compiles all supported applications and runs board lint.
Neither proves physical pin retention, RF delivery, power consumption or autonomy.
XIAO sensor reads, pairing with Heltec, repeated wake/delivery and USB recovery
must be checked on hardware before deployment.

References:

- [Seeed kit](https://wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit/)
- [XIAO pinout](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Wio B2B schematic](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf)
- [Wio module datasheet](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf)
- [ESP32-S3 GPIO retention](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/peripherals/gpio.html)
