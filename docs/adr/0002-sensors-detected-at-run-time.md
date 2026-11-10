# ADR 0002: Sensors detected at run time

- Status: Accepted
- Date: 2026-11-10

## Context

A transmitter image contained exactly one climate sensor driver, chosen with
`CAJUI_SENSOR_*` at build time. Each board and sensor pair was its own environment, so
supporting a sensor multiplied the images, and whoever installs a transmitter had to know
its sensor beforehand and pick the matching image. Changing the sensor meant installing
another image, and a transmitter could not carry two sensors. The sample was built by
`climateSample`, fixed to sensor 1 with temperature and humidity, and the receiver kept
its own table of metric and unit names. This replaces the sensor selection described in
[ADR 0001](0001-board-profile-headers.md); its board profiles are unchanged.

## Decision

- Every sensor driver is built into every transmitter image. An image is chosen by board
  only; the board profile's `Sensor*` pins say which drivers the board can carry.
- A driver implements `cajui::Sensor`: its description (name, wire sensor ID, metrics
  with their valid ranges), one read and a sleep state. `collectSample` in `lib/` turns
  the attached sensors into one sample and is tested on the host with driver doubles.
- After any start that is not a wake from deep sleep, the transmitter tries each driver
  and keeps the sensors that answer, in RTC memory. Later wakes read only those. A known
  sensor that stops answering is reported as error readings and never dropped on its own;
  after three such samples the other drivers are tried again.
- Sensor IDs are fixed per driver and append-only: SHT4x 1, DHT22 2, analog input 3.
- Metric and unit IDs and names live in one registry, `cajui_registry.h`, used by the
  transmitter and the receiver. A metric has exactly one unit.
- An input that cannot identify itself is reported raw: the analog input sends volts, and
  the consumer gives the value its meaning.

## Alternatives

- Keep one image per board and sensor: no detection to get wrong, but the number of
  images grows with every sensor and the choice stays with the person installing.
- Configure the attached sensors from the receiver or a monitoring application: needs a
  downlink for transmitter parameters, which the radio protocol does not carry yet, and
  still asks for a choice that most sensors can answer themselves.
- Drop a sensor that stops answering: a broken cable would then make its readings
  disappear without an error, and nothing downstream would report it.
- Number sensors in order of detection: an ID would change when another sensor is added.
- Convert an analog input to a quantity in the transmitter: every calibration change
  would need new firmware, and earlier readings could not be recalibrated.

## Consequences

- One image per transmitter board, and one target fewer to build, lint and release.
  The DHT22 is read by `cajui_dht22`, not the Adafruit DHT library: that library keeps
  interrupts off for all 80 pulses even after one times out, and bounds each wait by loop
  iterations, not time. With detection it runs on every board with a data pin, whatever
  is attached, and something other than a DHT22 on that pin made the interrupt watchdog
  restart a XIAO at every detection pass. Each wait is now bounded in microseconds and
  the read stops at the first timeout.
- **A DHT22 changes identity**: it was sensor 1 and is sensor 2. Consumers see a new
  sensor after the update, and the earlier history stays under the old ID.
- Adding or removing a sensor takes effect at the next power-on or reset, not while the
  transmitter sleeps.
- A board with a data pin and no DHT22 spends the driver's 2.2-second wait on each
  detection pass; with no sensor at all that is every wake.
- A frame holds eight readings; a sensor that would not fit is not read.
- Home Assistant Discovery still announces temperature and humidity of one sensor per
  transmitter.
- The analog presence test runs on every read, so a loose cable becomes error readings.
  It rests on an unconnected pin reading near zero with the internal pull-down on, still
  to be measured on each board, and a source resting near 0 V needs another way to be
  declared.
