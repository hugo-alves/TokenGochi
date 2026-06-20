#pragma once

#include <stdint.h>

namespace battery_status {

enum class ChargeState : uint8_t {
    Unknown,
    Charging,
    Discharging,
};

enum class WarningState : uint8_t {
    Unknown,
    Normal,
    Low,
    Critical,
};

struct Snapshot {
    bool percentKnown;
    int8_t percent;
    bool voltageKnown;
    int16_t voltageMv;
    bool currentKnown;
    int32_t currentMa;
    ChargeState charge;
    uint32_t sampledAtMs;
};

Snapshot unknown(uint32_t nowMs = 0);
uint8_t percentFromVoltage(int16_t voltageMv);
WarningState warningFor(const Snapshot& snapshot, uint8_t lowPercent, uint8_t criticalPercent);
bool isWarning(WarningState state);
const char* chargeLabel(ChargeState state);
const char* warningLabel(WarningState state);

Snapshot readHardware(uint32_t nowMs);

}  // namespace battery_status
