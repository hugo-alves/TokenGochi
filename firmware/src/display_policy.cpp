#include "display_policy.h"

#include "device_settings.h"

namespace display_policy {

bool allowsAutoDim(ActivityClass activity) {
    return activity == ActivityClass::Idle ||
           activity == ActivityClass::VoiceIdle ||
           activity == ActivityClass::Stats;
}

DisplayState targetState(ActivityClass activity,
                         uint32_t idleMs,
                         bool autoDimEnabled,
                         uint32_t timeoutMs,
                         uint32_t sleepTimeoutMs) {
    if (!autoDimEnabled || timeoutMs == 0) return DisplayState::Bright;
    if (!allowsAutoDim(activity)) return DisplayState::Bright;
    if (sleepTimeoutMs != 0 && idleMs >= sleepTimeoutMs) return DisplayState::Asleep;
    return idleMs >= timeoutMs ? DisplayState::Dimmed : DisplayState::Bright;
}

uint8_t targetBrightnessPercent(DisplayState state,
                                uint8_t brightPercent,
                                uint8_t dimPercent,
                                bool criticalBattery) {
    brightPercent = device_settings::clampBrightnessPercent(brightPercent);
    dimPercent = device_settings::clampDimBrightnessPercent(dimPercent);
    if (state == DisplayState::Bright) return brightPercent;
    if (state == DisplayState::Asleep) return 0;
    if (criticalBattery && dimPercent > 20) return 20;
    return dimPercent;
}

}  // namespace display_policy
