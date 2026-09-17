// --- app launcher ----------------------------------------------------------
static bool launcherEntryAllowed() {
#if TOKENGOCHI_COMPACT_UI
    return false;
#else
    return !audio::micActive() &&
        (g_mode == Mode::IDLE || g_mode == Mode::VOICE_IDLE ||
         g_mode == Mode::STATS || g_mode == Mode::LAUNCHER);
#endif
}

static bool launcherTouchPresent() {
#if TOKENGOCHI_HAS_TOUCH
    return M5.Touch.isEnabled() && M5.Touch.getCount() > 0;
#else
    return false;
#endif
}

static void drawLauncherFrame() {
    if (!displayVisible()) return;
    const char* warning = nullptr;
    if (g_settings.lowBatteryWarning && battery_status::isWarning(g_batteryWarning)) {
        warning = g_batteryWarning == battery_status::WarningState::Critical
            ? "BATTERY CRITICAL" : "BATTERY LOW";
    }
    apps::drawLauncher(g_launcher, warning);
    g_launcher.painted();
}

static void enterLauncher(const char* source) {
    if (!launcherEntryAllowed()) return;
    Serial.printf("[apps] %s -> launcher\n", source);
    apps::quietLauncherPeripherals();
    ui::pageReset();
    g_greetingUntilMs = 0;
    g_mode = Mode::LAUNCHER;
    g_launcher.enter();
    noteInteraction(source);
    // Do not carry a half-completed button/touch gesture into the new screen.
    g_suppressButtonsUntilRelease = true;
    g_launcherWakeRelease = true;
    g_launcherHold.cancelUntilRelease();
    // This screen has no network work; returning to Pet requests reconnection.
    g_wifiReconnectRequested = false;
    drawLauncherFrame();
}

// Sample this before handleSettingsChord(): even an incomplete/late A+B chord
// must cancel the single-A shortcut. The existing settings handler remains owner
// of the two-button hold. A single-A hold opens on release after 800 ms.
static bool handleLauncherNavigation() {
    const bool allowed = launcherEntryAllowed();
    const bool a = M5.BtnA.isPressed();
    const bool b = M5.BtnB.isPressed();
    const bool touch = launcherTouchPresent();
    const uint32_t now = millis();

    if (allowed && !displayVisible() && (a || b || touch)) {
        noteInteraction("apps wake");
        g_launcherWakeRelease = true;
        g_suppressButtonsUntilRelease = true;
        g_launcherHold.cancelUntilRelease();
        if (g_mode == Mode::LAUNCHER) {
            g_wifiReconnectRequested = false;
            g_launcher.invalidate();
            drawLauncherFrame();
        }
        return true;
    }
    if (g_launcherWakeRelease) {
        g_launcherHold.cancelUntilRelease();
        if (!a && !b && !touch) {
            g_launcherWakeRelease = false;
            g_suppressButtonsUntilMs = now + 80;
        }
        return true;
    }

    const bool suppressed = buttonsSuppressed();
    const bool held = g_launcherHold.update(now, a, b, allowed && !suppressed);
    if (!allowed) return false;
    if (a && b) g_suppressButtonsUntilRelease = true;
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
        noteInteraction("apps button");
        if (g_mode == Mode::LAUNCHER) g_wifiReconnectRequested = false;
    }
    if (!held) return false;

    g_suppressButtonsUntilRelease = true;
    g_launcherWakeRelease = true;
    noteInteraction("A hold release");
    if (g_mode != Mode::LAUNCHER) {
        enterLauncher("A hold release");
    } else if (g_launcher.view() == apps::View::SteadyPreview) {
        apps::quietLauncherPeripherals();
        g_launcher.back();
        g_wifiReconnectRequested = false;
        drawLauncherFrame();
    } else {
        returnToPet("A hold from launcher");
    }
    return true;
}

static void updateLauncher() {
    const uint32_t now = millis();
    g_launcher.tick(now);
    // Clear wake-triggered requests while this offline screen owns the display.
    g_wifiReconnectRequested = false;
    static battery_status::WarningState lastWarning = battery_status::WarningState::Unknown;
    if (lastWarning != g_batteryWarning) {
        lastWarning = g_batteryWarning;
        g_launcher.invalidate();
    }
    if (!displayVisible()) return;

    apps::Action action = apps::Action::None;
    if (btnBClicked()) {
        g_launcher.next();
    } else if (btnAClicked()) {
        action = g_launcher.accept(now);
    } else {
        int16_t x = 0, y = 0;
        if (touchClicked(&x, &y)) action = g_launcher.tap(x, y, now);
    }
    if (action == apps::Action::OpenPet) {
        g_suppressButtonsUntilRelease = true;
        g_launcherWakeRelease = true;
        returnToPet("launcher Pet");
        return;
    }
    // The click helpers call noteInteraction(), which may request Wi-Fi.
    g_wifiReconnectRequested = false;
    if (g_launcher.dirty()) drawLauncherFrame();
}
