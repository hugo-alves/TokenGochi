#include <unity.h>

#include "battery_status.h"
#include "device_settings.h"
#include "display_policy.h"

void setUp() {}
void tearDown() {}

static void test_settings_defaults_are_normalized() {
    device_settings::Settings s = device_settings::defaults();
    s = device_settings::normalize(s);
    TEST_ASSERT_EQUAL_UINT8(device_settings::VERSION, s.version);
    TEST_ASSERT_EQUAL_UINT32(10, s.recordSeconds);
    TEST_ASSERT_TRUE(s.recordAuto);
    TEST_ASSERT_EQUAL_STRING("auto", device_settings::recordModeLabel(s));
    TEST_ASSERT_EQUAL_UINT8(35, s.brightnessPercent);
    TEST_ASSERT_EQUAL_UINT8(40, s.volumePercent);
    TEST_ASSERT_TRUE(s.buttonSound);
    TEST_ASSERT_TRUE(s.vibration);
    TEST_ASSERT_TRUE(s.autoDimEnabled);
    TEST_ASSERT_EQUAL_UINT32(15000, s.autoDimTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(1, s.dimBrightnessPercent);
}

static void test_old_settings_migrate_to_power_saver_defaults() {
    device_settings::Settings s = device_settings::defaults();
    s.version = 3;
    s.brightnessPercent = 80;
    s.volumePercent = 70;
    s.autoDimEnabled = true;
    s.autoDimTimeoutMs = 60000;
    s.dimBrightnessPercent = 35;

    s = device_settings::normalize(s);
    TEST_ASSERT_EQUAL_UINT8(device_settings::VERSION, s.version);
    TEST_ASSERT_EQUAL_UINT8(35, s.brightnessPercent);
    TEST_ASSERT_EQUAL_UINT8(40, s.volumePercent);
    TEST_ASSERT_TRUE(s.autoDimEnabled);
    TEST_ASSERT_EQUAL_UINT32(15000, s.autoDimTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(1, s.dimBrightnessPercent);
}

static void test_settings_clamp_and_snap_bad_values() {
    device_settings::Settings s = device_settings::defaults();
    s.recordSeconds = 12;
    s.recordAuto = false;
    s.brightnessPercent = 0;
    s.volumePercent = 250;
    s.autoDimTimeoutMs = 29000;
    s.dimBrightnessPercent = 99;
    s.lowBatteryPercent = 2;
    s.criticalBatteryPercent = 50;

    s = device_settings::normalize(s);
    TEST_ASSERT_EQUAL_UINT32(20, s.recordSeconds);
    TEST_ASSERT_FALSE(s.recordAuto);
    TEST_ASSERT_EQUAL_UINT8(10, s.brightnessPercent);
    TEST_ASSERT_EQUAL_UINT8(100, s.volumePercent);
    TEST_ASSERT_EQUAL_UINT32(30000, s.autoDimTimeoutMs);
    TEST_ASSERT_TRUE(s.dimBrightnessPercent < s.brightnessPercent);
    TEST_ASSERT_EQUAL_UINT8(1, s.dimBrightnessPercent);
    TEST_ASSERT_TRUE(s.criticalBatteryPercent < s.lowBatteryPercent);
}

static void test_settings_cycle_values() {
    TEST_ASSERT_EQUAL_UINT32(20, device_settings::nextRecordSeconds(10));
    TEST_ASSERT_EQUAL_UINT32(30, device_settings::nextRecordSeconds(20));
    TEST_ASSERT_EQUAL_UINT32(10, device_settings::nextRecordSeconds(30));

    device_settings::Settings voice = device_settings::defaults();
    voice = device_settings::cycleRecordMode(voice);
    TEST_ASSERT_FALSE(voice.recordAuto);
    TEST_ASSERT_EQUAL_STRING("10s", device_settings::recordModeLabel(voice));
    voice = device_settings::cycleRecordMode(voice);
    TEST_ASSERT_EQUAL_STRING("20s", device_settings::recordModeLabel(voice));
    voice = device_settings::cycleRecordMode(voice);
    TEST_ASSERT_EQUAL_STRING("30s", device_settings::recordModeLabel(voice));
    voice = device_settings::cycleRecordMode(voice);
    TEST_ASSERT_TRUE(voice.recordAuto);
    TEST_ASSERT_EQUAL_STRING("auto", device_settings::recordModeLabel(voice));

    TEST_ASSERT_EQUAL_UINT8(90, device_settings::adjustBrightness(83, 10));
    TEST_ASSERT_EQUAL_UINT8(0, device_settings::adjustVolume(4, -10));

    device_settings::Settings s = device_settings::defaults();
    s.autoDimEnabled = false;
    s.autoDimTimeoutMs = 0;
    s = device_settings::cycleAutoDim(s);
    TEST_ASSERT_TRUE(s.autoDimEnabled);
    TEST_ASSERT_EQUAL_UINT32(15000, s.autoDimTimeoutMs);
}

static void test_hardware_mapping_stays_in_expected_ranges() {
    TEST_ASSERT_EQUAL_UINT8(0, device_settings::volumeToHardware(0));
    TEST_ASSERT_EQUAL_UINT8(255, device_settings::volumeToHardware(100));
    TEST_ASSERT_TRUE(device_settings::brightnessToHardware(10) > 0);
    TEST_ASSERT_EQUAL_UINT8(255, device_settings::brightnessToHardware(100));
}

static void test_battery_warning_uses_percent_or_voltage_fallback() {
    battery_status::Snapshot s = battery_status::unknown();
    s.percentKnown = true;
    s.percent = 9;
    TEST_ASSERT_EQUAL((int)battery_status::WarningState::Critical,
                      (int)battery_status::warningFor(s, 20, 10));

    s.percentKnown = false;
    s.voltageKnown = true;
    s.voltageMv = 3450;
    TEST_ASSERT_EQUAL((int)battery_status::WarningState::Low,
                      (int)battery_status::warningFor(s, 20, 10));

    s.voltageKnown = false;
    TEST_ASSERT_EQUAL((int)battery_status::WarningState::Unknown,
                      (int)battery_status::warningFor(s, 20, 10));
}

static void test_display_policy_dims_only_safe_states() {
    TEST_ASSERT_TRUE(display_policy::allowsAutoDim(display_policy::ActivityClass::Idle));
    TEST_ASSERT_TRUE(display_policy::allowsAutoDim(display_policy::ActivityClass::VoiceIdle));
    TEST_ASSERT_FALSE(display_policy::allowsAutoDim(display_policy::ActivityClass::Foreground));
    TEST_ASSERT_EQUAL((int)display_policy::DisplayState::Dimmed,
                      (int)display_policy::targetState(display_policy::ActivityClass::Idle,
                                                       60000,
                                                       true,
                                                       60000,
                                                       0));
    TEST_ASSERT_EQUAL((int)display_policy::DisplayState::Bright,
                      (int)display_policy::targetState(display_policy::ActivityClass::Foreground,
                                                       60000,
                                                       true,
                                                       60000,
                                                       0));
    TEST_ASSERT_EQUAL((int)display_policy::DisplayState::Asleep,
                      (int)display_policy::targetState(display_policy::ActivityClass::Idle,
                                                       120000,
                                                       true,
                                                       15000,
                                                       120000));
}

static void test_display_policy_clamps_critical_dim_brightness() {
    TEST_ASSERT_EQUAL_UINT8(80,
                            display_policy::targetBrightnessPercent(display_policy::DisplayState::Bright,
                                                                     80,
                                                                     35,
                                                                     true));
    TEST_ASSERT_EQUAL_UINT8(35,
                            display_policy::targetBrightnessPercent(display_policy::DisplayState::Dimmed,
                                                                     80,
                                                                     35,
                                                                     false));
    TEST_ASSERT_EQUAL_UINT8(0,
                            display_policy::targetBrightnessPercent(display_policy::DisplayState::Asleep,
                                                                     80,
                                                                     35,
                                                                     false));
    TEST_ASSERT_EQUAL_UINT8(20,
                            display_policy::targetBrightnessPercent(display_policy::DisplayState::Dimmed,
                                                                     80,
                                                                     35,
                                                                     true));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_settings_defaults_are_normalized);
    RUN_TEST(test_settings_clamp_and_snap_bad_values);
    RUN_TEST(test_old_settings_migrate_to_power_saver_defaults);
    RUN_TEST(test_settings_cycle_values);
    RUN_TEST(test_hardware_mapping_stays_in_expected_ranges);
    RUN_TEST(test_battery_warning_uses_percent_or_voltage_fallback);
    RUN_TEST(test_display_policy_dims_only_safe_states);
    RUN_TEST(test_display_policy_clamps_critical_dim_brightness);
    return UNITY_END();
}
