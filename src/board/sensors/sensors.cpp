// SPDX-License-Identifier: Apache-2.0
#if defined(CAJUI_RUNTIME_ROLE) && CAJUI_RUNTIME_ROLE == 1
#include "board/sensors/sensors.h"
#include "board/profile.h"

namespace board {
size_t sensorDrivers(cajui::Sensor** drivers, size_t capacity) {
    size_t count = 0;
    if (SensorSda != NoPin && SensorScl != NoPin && count < capacity)
        drivers[count++] = &sht4xSensor();
    if (SensorData != NoPin && count < capacity) drivers[count++] = &dht22Sensor();
    if (SensorAnalog != NoPin && count < capacity) drivers[count++] = &analogSensor();
    return count;
}
void sensorsOff() {
    cajui::Sensor* drivers[cajui::MaxSensors]{};
    const size_t count = sensorDrivers(drivers, cajui::MaxSensors);
    for (size_t i = 0; i < count; ++i) drivers[i]->off();
}
} // namespace board
#endif
