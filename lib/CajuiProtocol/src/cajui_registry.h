// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace cajui {
// Semantic registry of the wire format (docs/protocol-v1.md). Append-only: an ID is never
// reused and never changes meaning. A metric has exactly one unit, so its values compare
// across sensors; a driver converts whatever its part reports into that unit.
namespace metric {
constexpr uint16_t Temperature = 1, Humidity = 2, Voltage = 3;
} // namespace metric
namespace unit {
constexpr uint8_t Celsius = 1, Percent = 2, Volt = 3;
} // namespace unit
struct Quantity {
    uint16_t metric;
    uint8_t unit;
    const char* name; // Metric name in forwarded telemetry.
};
// Null outside the registry. Consumers still pass such a reading on, under a generic name.
const Quantity* quantity(uint16_t metric);
const char* unitName(uint8_t unit);
} // namespace cajui
