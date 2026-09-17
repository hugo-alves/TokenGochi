#pragma once
#include "app_launcher_model.h"
namespace apps {
void drawLauncher(const Launcher& launcher, const char* warning = nullptr);
// Only call at safe-mode boundaries; never tears down an active recorder.
void quietLauncherPeripherals();
}
