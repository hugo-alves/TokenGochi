#pragma once

#include <stdint.h>

namespace display_policy {

enum class ActivityClass : uint8_t {
    Idle,
    VoiceIdle,
    Stats,
    Foreground,
};

enum class DisplayState : uint8_t {
    Bright,
    Dimmed,
};

bool allowsAutoDim(ActivityClass activity);
DisplayState targetState(ActivityClass activity,
                         uint32_t idleMs,
                         bool autoDimEnabled,
                         uint32_t timeoutMs);
uint8_t targetBrightnessPercent(DisplayState state,
                                uint8_t brightPercent,
                                uint8_t dimPercent,
                                bool criticalBattery);

}  // namespace display_policy
