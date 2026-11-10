// SPDX-License-Identifier: Apache-2.0
#include "cajui_registry.h"

namespace cajui {
namespace {
const Quantity Quantities[] = {
    {metric::Temperature, unit::Celsius, "temperature"},
    {metric::Humidity, unit::Percent, "humidity"},
    {metric::Voltage, unit::Volt, "voltage"},
};
struct Unit {
    uint8_t unit;
    const char* name;
};
const Unit Units[] = {{unit::Celsius, "degC"}, {unit::Percent, "%"}, {unit::Volt, "V"}};
} // namespace
const Quantity* quantity(uint16_t metric) {
    for (const auto& candidate : Quantities)
        if (candidate.metric == metric) return &candidate;
    return nullptr;
}
const char* unitName(uint8_t unit) {
    for (const auto& candidate : Units)
        if (candidate.unit == unit) return candidate.name;
    return nullptr;
}
} // namespace cajui
