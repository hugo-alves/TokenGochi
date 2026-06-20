#include "battery_status.h"

#if defined(ARDUINO)
#include <M5Unified.h>
#endif

namespace battery_status {

Snapshot unknown(uint32_t nowMs) {
    Snapshot s = {};
    s.percentKnown = false;
    s.percent = -1;
    s.voltageKnown = false;
    s.voltageMv = 0;
    s.currentKnown = false;
    s.currentMa = 0;
    s.charge = ChargeState::Unknown;
    s.sampledAtMs = nowMs;
    return s;
}

uint8_t percentFromVoltage(int16_t voltageMv) {
    static constexpr int16_t kEmptyMv = 3300;
    static constexpr int16_t kFullMv = 4200;
    if (voltageMv <= kEmptyMv) return 0;
    if (voltageMv >= kFullMv) return 100;
    return (uint8_t)(((int32_t)(voltageMv - kEmptyMv) * 100L) / (kFullMv - kEmptyMv));
}

WarningState warningFor(const Snapshot& snapshot, uint8_t lowPercent, uint8_t criticalPercent) {
    if (criticalPercent >= lowPercent) {
        criticalPercent = lowPercent > 5 ? lowPercent - 5 : 5;
    }
    int percent = -1;
    if (snapshot.percentKnown && snapshot.percent >= 0) {
        percent = snapshot.percent;
    } else if (snapshot.voltageKnown && snapshot.voltageMv > 0) {
        percent = percentFromVoltage(snapshot.voltageMv);
    }
    if (percent < 0) return WarningState::Unknown;
    if (percent <= criticalPercent) return WarningState::Critical;
    if (percent <= lowPercent) return WarningState::Low;
    return WarningState::Normal;
}

bool isWarning(WarningState state) {
    return state == WarningState::Low || state == WarningState::Critical;
}

const char* chargeLabel(ChargeState state) {
    switch (state) {
        case ChargeState::Charging: return "charging";
        case ChargeState::Discharging: return "battery";
        case ChargeState::Unknown: break;
    }
    return "unknown";
}

const char* warningLabel(WarningState state) {
    switch (state) {
        case WarningState::Normal: return "ok";
        case WarningState::Low: return "low";
        case WarningState::Critical: return "critical";
        case WarningState::Unknown: break;
    }
    return "unknown";
}

Snapshot readHardware(uint32_t nowMs) {
    Snapshot s = unknown(nowMs);
#if defined(ARDUINO)
    int32_t level = M5.Power.getBatteryLevel();
    if (level >= 0 && level <= 100) {
        s.percentKnown = true;
        s.percent = (int8_t)level;
    }

    int16_t voltage = M5.Power.getBatteryVoltage();
    if (voltage > 0) {
        s.voltageKnown = true;
        s.voltageMv = voltage;
    }

    int32_t current = M5.Power.getBatteryCurrent();
    if (current != 0) {
        s.currentKnown = true;
        s.currentMa = current;
    }

    auto charge = M5.Power.isCharging();
    if (charge == m5::Power_Class::is_charging) {
        s.charge = ChargeState::Charging;
    } else if (charge == m5::Power_Class::is_discharging) {
        s.charge = ChargeState::Discharging;
    } else {
        s.charge = ChargeState::Unknown;
    }
#endif
    return s;
}

}  // namespace battery_status
