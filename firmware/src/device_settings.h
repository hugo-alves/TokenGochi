#pragma once

#include <stdint.h>

namespace device_settings {

static constexpr uint8_t VERSION = 2;
static constexpr uint32_t RECORD_SECONDS_DEFAULT = 10;
static constexpr bool RECORD_AUTO_DEFAULT = true;
static constexpr uint8_t BRIGHTNESS_PERCENT_DEFAULT = 80;
static constexpr uint8_t VOLUME_PERCENT_DEFAULT = 70;
static constexpr uint32_t AUTO_DIM_TIMEOUT_DEFAULT_MS = 60000;
static constexpr uint8_t AUTO_DIM_PERCENT_DEFAULT = 35;
static constexpr uint8_t LOW_BATTERY_PERCENT_DEFAULT = 20;
static constexpr uint8_t CRITICAL_BATTERY_PERCENT_DEFAULT = 10;

struct Settings {
    uint8_t version;
    uint32_t recordSeconds;
    bool recordAuto;
    uint8_t brightnessPercent;
    uint8_t volumePercent;
    bool buttonSound;
    bool vibration;
    bool autoDimEnabled;
    uint32_t autoDimTimeoutMs;
    uint8_t dimBrightnessPercent;
    bool lowBatteryWarning;
    uint8_t lowBatteryPercent;
    uint8_t criticalBatteryPercent;
};

Settings defaults();
Settings normalize(Settings value);

uint32_t normalizeRecordSeconds(uint32_t seconds);
uint32_t nextRecordSeconds(uint32_t seconds);
uint32_t effectiveRecordSeconds(const Settings& settings);
Settings cycleRecordMode(Settings current);
const char* recordModeLabel(const Settings& settings);

uint8_t clampPercent(int value);
uint8_t clampBrightnessPercent(int value);
uint8_t clampDimBrightnessPercent(int value);
uint8_t adjustBrightness(uint8_t current, int delta);
uint8_t adjustVolume(uint8_t current, int delta);

uint32_t normalizeAutoDimTimeoutMs(bool enabled, uint32_t timeoutMs);
Settings cycleAutoDim(Settings current);

uint8_t brightnessToHardware(uint8_t brightnessPercent);
uint8_t volumeToHardware(uint8_t volumePercent);
const char* autoDimLabel(const Settings& settings);

bool load(Settings& out);
bool save(const Settings& settings);

}  // namespace device_settings
