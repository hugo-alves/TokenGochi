#include "app_launcher.h"
#include "config.h"
#include "audio.h"
#include "ui.h"
#if !TOKENGOCHI_COMPACT_UI
#include "app_launcher_render.h"
#endif

namespace apps {
void drawLauncher(const Launcher& launcher, const char* warning) {
#if !TOKENGOCHI_COMPACT_UI
    render::draw(ui::target(), launcher, warning);
    ui::flush();
#else
    // Launcher entry is disabled on the rectangular target; existing UI is unchanged.
    (void)launcher;
    (void)warning;
#endif
}
void quietLauncherPeripherals() {
    if (audio::micActive()) return;
    // The launcher/preview own no ongoing audio or vibration.
    M5.Speaker.end();
    M5.Mic.end();
#if TOKENGOCHI_HAS_VIBRATION
    M5.Power.setVibration(0);
#endif
}
} // namespace apps
