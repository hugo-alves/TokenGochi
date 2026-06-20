#include "device_settings.h"

#if defined(ARDUINO) && defined(ESP32)
#include <Preferences.h>
#endif

namespace device_settings {

static constexpr const char* kNamespace = "tg_settings";

Settings defaults() {
    Settings s = {};
    s.version = VERSION;
    s.recordSeconds = RECORD_SECONDS_DEFAULT;
    s.recordAuto = RECORD_AUTO_DEFAULT;
    s.brightnessPercent = BRIGHTNESS_PERCENT_DEFAULT;
    s.volumePercent = VOLUME_PERCENT_DEFAULT;
    s.buttonSound = true;
    s.vibration = true;
    s.autoDimEnabled = true;
    s.autoDimTimeoutMs = AUTO_DIM_TIMEOUT_DEFAULT_MS;
    s.dimBrightnessPercent = AUTO_DIM_PERCENT_DEFAULT;
    s.lowBatteryWarning = true;
    s.lowBatteryPercent = LOW_BATTERY_PERCENT_DEFAULT;
    s.criticalBatteryPercent = CRITICAL_BATTERY_PERCENT_DEFAULT;
    return s;
}

uint8_t clampPercent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (uint8_t)value;
}

uint8_t clampBrightnessPercent(int value) {
    if (value < 10) return 10;
    if (value > 100) return 100;
    return (uint8_t)value;
}

uint8_t clampDimBrightnessPercent(int value) {
    if (value < 5) return 5;
    if (value > 80) return 80;
    return (uint8_t)value;
}

uint32_t normalizeRecordSeconds(uint32_t seconds) {
    if (seconds <= 10) return 10;
    if (seconds <= 20) return 20;
    return 30;
}

uint32_t nextRecordSeconds(uint32_t seconds) {
    seconds = normalizeRecordSeconds(seconds);
    if (seconds == 10) return 20;
    if (seconds == 20) return 30;
    return 10;
}

uint32_t effectiveRecordSeconds(const Settings& settings) {
    Settings normalized = normalize(settings);
    return normalized.recordSeconds;
}

Settings cycleRecordMode(Settings current) {
    current = normalize(current);
    if (current.recordAuto) {
        current.recordAuto = false;
        current.recordSeconds = 10;
    } else if (current.recordSeconds == 10) {
        current.recordSeconds = 20;
    } else if (current.recordSeconds == 20) {
        current.recordSeconds = 30;
    } else {
        current.recordAuto = true;
        current.recordSeconds = RECORD_SECONDS_DEFAULT;
    }
    return normalize(current);
}

uint8_t adjustBrightness(uint8_t current, int delta) {
    int rounded = ((int)current + 5) / 10 * 10;
    return clampBrightnessPercent(rounded + delta);
}

uint8_t adjustVolume(uint8_t current, int delta) {
    int rounded = ((int)current + 5) / 10 * 10;
    return clampPercent(rounded + delta);
}

uint32_t normalizeAutoDimTimeoutMs(bool enabled, uint32_t timeoutMs) {
    if (!enabled || timeoutMs == 0) return 0;
    if (timeoutMs <= 15000) return 15000;
    if (timeoutMs <= 30000) return 30000;
    return 60000;
}

Settings cycleAutoDim(Settings current) {
    current = normalize(current);
    if (!current.autoDimEnabled || current.autoDimTimeoutMs == 0) {
        current.autoDimEnabled = true;
        current.autoDimTimeoutMs = 15000;
    } else if (current.autoDimTimeoutMs <= 15000) {
        current.autoDimTimeoutMs = 30000;
    } else if (current.autoDimTimeoutMs <= 30000) {
        current.autoDimTimeoutMs = 60000;
    } else {
        current.autoDimEnabled = false;
        current.autoDimTimeoutMs = 0;
    }
    return normalize(current);
}

Settings normalize(Settings value) {
    Settings fallback = defaults();
    value.version = VERSION;
    value.recordSeconds = normalizeRecordSeconds(value.recordSeconds);
    value.brightnessPercent = clampBrightnessPercent(value.brightnessPercent);
    value.volumePercent = clampPercent(value.volumePercent);
    value.autoDimTimeoutMs = normalizeAutoDimTimeoutMs(value.autoDimEnabled, value.autoDimTimeoutMs);
    value.autoDimEnabled = value.autoDimTimeoutMs != 0;
    value.dimBrightnessPercent = clampDimBrightnessPercent(value.dimBrightnessPercent);
    if (value.dimBrightnessPercent >= value.brightnessPercent) {
        value.dimBrightnessPercent = clampDimBrightnessPercent(value.brightnessPercent / 2);
    }
    value.lowBatteryPercent = clampPercent(value.lowBatteryPercent);
    value.criticalBatteryPercent = clampPercent(value.criticalBatteryPercent);
    if (value.lowBatteryPercent < 10) value.lowBatteryPercent = fallback.lowBatteryPercent;
    if (value.criticalBatteryPercent < 5) value.criticalBatteryPercent = fallback.criticalBatteryPercent;
    if (value.criticalBatteryPercent >= value.lowBatteryPercent) {
        value.criticalBatteryPercent = value.lowBatteryPercent > 5 ? value.lowBatteryPercent - 5 : 5;
    }
    return value;
}

uint8_t brightnessToHardware(uint8_t brightnessPercent) {
    brightnessPercent = clampBrightnessPercent(brightnessPercent);
    uint16_t value = (uint16_t)brightnessPercent * 255U / 100U;
    if (value == 0) value = 1;
    return (uint8_t)value;
}

uint8_t volumeToHardware(uint8_t volumePercent) {
    volumePercent = clampPercent(volumePercent);
    return (uint8_t)((uint16_t)volumePercent * 255U / 100U);
}

const char* autoDimLabel(const Settings& settings) {
    Settings normalized = normalize(settings);
    if (!normalized.autoDimEnabled) return "off";
    if (normalized.autoDimTimeoutMs <= 15000) return "15s";
    if (normalized.autoDimTimeoutMs <= 30000) return "30s";
    return "60s";
}

const char* recordModeLabel(const Settings& settings) {
    Settings normalized = normalize(settings);
    if (normalized.recordAuto) return "auto";
    if (normalized.recordSeconds == 10) return "10s";
    if (normalized.recordSeconds == 20) return "20s";
    return "30s";
}

bool load(Settings& out) {
    out = defaults();
#if defined(ARDUINO) && defined(ESP32)
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        out = normalize(out);
        return false;
    }
    out.version = prefs.getUChar("ver", out.version);
    out.recordSeconds = prefs.getUInt("rec_s", out.recordSeconds);
    out.recordAuto = prefs.getBool("rec_auto", out.recordAuto);
    out.brightnessPercent = prefs.getUChar("bright", out.brightnessPercent);
    out.volumePercent = prefs.getUChar("vol", out.volumePercent);
    out.buttonSound = prefs.getBool("sound", out.buttonSound);
    out.vibration = prefs.getBool("vibe", out.vibration);
    out.autoDimEnabled = prefs.getBool("dim_en", out.autoDimEnabled);
    out.autoDimTimeoutMs = prefs.getUInt("dim_ms", out.autoDimTimeoutMs);
    out.dimBrightnessPercent = prefs.getUChar("dim_pct", out.dimBrightnessPercent);
    out.lowBatteryWarning = prefs.getBool("bat_warn", out.lowBatteryWarning);
    out.lowBatteryPercent = prefs.getUChar("bat_low", out.lowBatteryPercent);
    out.criticalBatteryPercent = prefs.getUChar("bat_crit", out.criticalBatteryPercent);
    prefs.end();
#endif
    out = normalize(out);
    return true;
}

bool save(const Settings& settings) {
    Settings normalized = normalize(settings);
#if defined(ARDUINO) && defined(ESP32)
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }
    prefs.putUChar("ver", normalized.version);
    prefs.putUInt("rec_s", normalized.recordSeconds);
    prefs.putBool("rec_auto", normalized.recordAuto);
    prefs.putUChar("bright", normalized.brightnessPercent);
    prefs.putUChar("vol", normalized.volumePercent);
    prefs.putBool("sound", normalized.buttonSound);
    prefs.putBool("vibe", normalized.vibration);
    prefs.putBool("dim_en", normalized.autoDimEnabled);
    prefs.putUInt("dim_ms", normalized.autoDimTimeoutMs);
    prefs.putUChar("dim_pct", normalized.dimBrightnessPercent);
    prefs.putBool("bat_warn", normalized.lowBatteryWarning);
    prefs.putUChar("bat_low", normalized.lowBatteryPercent);
    prefs.putUChar("bat_crit", normalized.criticalBatteryPercent);
    prefs.end();
#else
    (void)normalized;
#endif
    return true;
}

}  // namespace device_settings
