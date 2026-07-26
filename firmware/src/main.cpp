#include <M5Unified.h>
#include <WiFi.h>
#include <driver/uart.h>
#include <esp_sleep.h>

#include "config.h"
#include "secrets.h"
#include "pet_state.h"
#include "net.h"
#include "ui.h"
#include "pet_sprite.h"
#include "audio.h"
#include "transcript_log.h"
#include "device_settings.h"
#include "battery_status.h"
#include "display_policy.h"

#include <ArduinoJson.h>

struct WifiCredential {
    const char* ssid;
    const char* pass;
};

#ifndef WIFI_NETWORKS
#if defined(WIFI_SSID) && defined(WIFI_PASS)
#define WIFI_NETWORKS { { WIFI_SSID, WIFI_PASS } }
#else
#error "Define WIFI_NETWORKS in secrets.h"
#endif
#endif

static const WifiCredential kWifiNetworks[] = WIFI_NETWORKS;
static constexpr size_t kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
static_assert(kWifiNetworkCount > 0, "At least one WiFi network must be configured");

// --- globals ---------------------------------------------------------------
static PetState     g_state;
static device_settings::Settings g_settings = device_settings::defaults();
static bool         g_wifiUp     = false;
static bool         g_bridgeUp   = false;
static uint32_t     g_lastPoll   = 0;
static uint32_t     g_lastRecon  = 0;
static size_t       g_lastWifiCredentialIndex = kWifiNetworkCount;
static bool         g_wifiReconnectRequested = false;
static bool         g_wifiRadioOffForIdle = false;
static uint32_t     g_lastRecRedraw = 0;
static uint32_t     g_recordSeconds = device_settings::RECORD_SECONDS_DEFAULT;
static uint32_t     g_clockEpochSec = 0;
static uint32_t     g_clockSyncedAtMs = 0;
static size_t       g_historyIndex = 0;

enum class Mode : uint8_t { IDLE, VOICE_IDLE, RECORDING, TRANSCRIBING, SHOWING, HISTORY_LIST, HISTORY_READING, STATS, CONFIRM, SETTINGS, SETTINGS_SAVED, ERROR };
enum class SettingsView : uint8_t { MENU, VOICE, BRIGHTNESS, VOLUME, FEEDBACK, AUTO_DIM, BATTERY };
static Mode         g_mode = Mode::IDLE;
static SettingsView g_settingsView = SettingsView::MENU;
static SettingsView g_settingsSavedReturnView = SettingsView::MENU;
static uint8_t      g_settingsIndex = 0;
static char         g_lastTranscript[TRANSCRIPT_LOG_TEXT_BYTES];
static char         g_transcriptTitle[32];
static char         g_transcriptFooter[40];
static char         g_lastError[64];
static uint32_t     g_errorAtMs = 0;
static uint32_t     g_statsAtMs = 0;       // when stats view opened
static uint32_t     g_confirmAtMs = 0;     // when reset confirm opened
static uint32_t     g_settingsAtMs = 0;
static uint32_t     g_settingsSavedUntilMs = 0;
static uint32_t     g_greetingUntilMs = 0; // hide greeting after this
static uint32_t     g_lastInteractionMs = 0;
static uint32_t     g_lastBatterySampleMs = 0;
static battery_status::Snapshot g_battery = battery_status::unknown();
static battery_status::WarningState g_batteryWarning = battery_status::WarningState::Unknown;
static display_policy::DisplayState g_displayState = display_policy::DisplayState::Bright;
static bool         g_displaySleeping = false;
static uint32_t     g_appliedCpuMhz = 0;
static uint32_t     g_lightSleepEntries = 0;
static uint64_t     g_lightSleepTotalMs = 0;
static uint32_t     g_lastLightSleepMs = 0;
static int          g_lastLightSleepWake = ESP_SLEEP_WAKEUP_UNDEFINED;
static int          g_lastLightSleepErr = ESP_OK;
static int          g_lastLightSleepTimerErr = ESP_OK;
static int          g_lastLightSleepUartErr = ESP_OK;
static bool         g_lightSleepAnnounced = false;
static uint32_t     g_serialAwakeUntilMs = 0;
static bool         g_greetOnFirstPoll = true;
static bool         g_suppressButtonsUntilRelease = false;
static uint32_t     g_suppressButtonsUntilMs = 0;
static uint32_t     g_abHoldStartMs = 0;

static constexpr uint32_t ERROR_HOLD_MS  = 3000; // show error then return
static constexpr uint32_t STATS_TIMEOUT_MS = 8000; // auto-dismiss stats view
static constexpr uint32_t CONFIRM_TIMEOUT_MS = 5000;
static constexpr uint32_t SETTINGS_TIMEOUT_MS = 15000;
static constexpr uint32_t SETTINGS_SAVED_MS = 900;
static constexpr uint32_t SETTINGS_CHORD_MS = 1200;
static constexpr uint32_t GREETING_HOLD_MS = 3000;
static constexpr uint8_t SETTINGS_ITEM_COUNT = 6;

// --- helpers ---------------------------------------------------------------
static void transcribeAndShow(const uint8_t* wav, size_t size);
static void enterSettings(const char* source);
static void openSettingsItem(uint8_t index, const char* source);
static void returnToPet(const char* source);
static void stopWifiRadio(const char* source, bool preserveBridge = false, bool idleSleep = false);
static void sampleBattery(bool force, const char* source);
static void sleepForLoopDelay(uint32_t delayMs);
static void printPowerProfile(Stream& out);
static bool passiveIdlePowerMode();

static constexpr uint32_t kMaxPollIntervalMs =
    TOKENGOCHI_PASSIVE_POLL_INTERVAL_MS > POLL_INTERVAL_MS
        ? TOKENGOCHI_PASSIVE_POLL_INTERVAL_MS
        : POLL_INTERVAL_MS;

static uint32_t pollIntervalForPassive(bool passive) {
    return passive ? TOKENGOCHI_PASSIVE_POLL_INTERVAL_MS : POLL_INTERVAL_MS;
}

static uint32_t currentPollIntervalMs() {
    return pollIntervalForPassive(passiveIdlePowerMode());
}

static void forcePollDue() {
    g_lastPoll = millis() - kMaxPollIntervalMs - 1;
}

static bool serialCommandIs(const char* line, const char* command, const char* wakeSuffix = nullptr) {
    if (strcmp(line, command) == 0) return true;
    if (strstr(line, command) != nullptr) return true;
    if (!wakeSuffix) return false;
    if (strstr(line, wakeSuffix) != nullptr) return true;

    // UART wake from light sleep can drop the first byte or two. Accept a
    // short trailing fragment so diagnostic commands still work after wake.
    const size_t lineLen = strlen(line);
    const size_t suffixLen = strlen(wakeSuffix);
    if (lineLen < 3 || lineLen > suffixLen) return false;
    return strcmp(line, wakeSuffix + suffixLen - lineLen) == 0;
}

static bool handleImmediateSerialCommand(char* line) {
    if (serialCommandIs(line, "TGWAKE", "WAKE")) {
        g_serialAwakeUntilMs = millis() + TOKENGOCHI_SERIAL_WAKE_AWAKE_MS;
        return true;
    }
    if (serialCommandIs(line, "TGSHOT", "SHOT")) {
        g_serialAwakeUntilMs = millis() + TOKENGOCHI_SERIAL_WAKE_AWAKE_MS;
        ui::writeScreenshot(Serial);
        return true;
    }
    if (serialCommandIs(line, "TGPOWER", "POWER")) {
        g_serialAwakeUntilMs = millis() + TOKENGOCHI_SERIAL_WAKE_AWAKE_MS;
        printPowerProfile(Serial);
        return true;
    }
    if (serialCommandIs(line, "TGPOLL", "POLL")) {
        g_serialAwakeUntilMs = millis() + TOKENGOCHI_SERIAL_WAKE_AWAKE_MS;
        forcePollDue();
        g_wifiReconnectRequested = true;
        Serial.println("TGPOLL scheduled");
        Serial.flush();
        return true;
    }
    return false;
}

static const char* displayStateLabel(display_policy::DisplayState state) {
    switch (state) {
        case display_policy::DisplayState::Bright: return "bright";
        case display_policy::DisplayState::Dimmed: return "dimmed";
        case display_policy::DisplayState::Asleep: return "asleep";
    }
    return "unknown";
}

static void applyCpuFrequency(uint32_t targetMhz, const char* source) {
    if (g_appliedCpuMhz == targetMhz && getCpuFrequencyMhz() == targetMhz) return;

    Serial.flush();
    bool ok = setCpuFrequencyMhz(targetMhz);
    Serial.begin(115200);
    delay(2);
    g_appliedCpuMhz = getCpuFrequencyMhz();
    Serial.printf("[power] cpu source=%s target=%uMHz actual=%uMHz ok=%d\n",
                  source,
                  (unsigned)targetMhz,
                  (unsigned)g_appliedCpuMhz,
                  ok ? 1 : 0);
}

static bool statePollDue(uint32_t now) {
    return now - g_lastPoll > currentPollIntervalMs();
}

static void applyWifiPowerPolicy(const char* source) {
    const bool ok = WiFi.setTxPower(TOKENGOCHI_WIFI_TX_POWER);
    Serial.printf("[power] wifi source=%s tx_power=%d ok=%d\n",
                  source,
                  (int)WiFi.getTxPower(),
                  ok ? 1 : 0);
}

static void printPowerProfile(Stream& out) {
    out.println("TGPOWER BEGIN");
    out.flush();
    out.printf("TGPOWER device=%s cpu_mhz=%u active_cpu_mhz=%u sleep_cpu_mhz=%u wifi_mode=%d wifi_ps=%d wifi_tx_power=%d wifi_idle_off=%d last_wifi_index=%d poll_ms=%u active_poll_ms=%u passive_poll_ms=%u reconnect_ms=%u sleep_reconnect_ms=%u idle_off_ms=%u idle_delay_ms=%u active_delay_ms=%u sleep_delay_ms=%u passive_light_sleep_ms=%u\n",
               TOKENGOCHI_DEVICE_NAME,
               (unsigned)getCpuFrequencyMhz(),
               (unsigned)TOKENGOCHI_CPU_MHZ,
               (unsigned)TOKENGOCHI_SLEEP_CPU_MHZ,
               (int)WiFi.getMode(),
               (int)WiFi.getSleep(),
               (int)WiFi.getTxPower(),
               g_wifiRadioOffForIdle ? 1 : 0,
               g_lastWifiCredentialIndex < kWifiNetworkCount ? (int)g_lastWifiCredentialIndex : -1,
               (unsigned)currentPollIntervalMs(),
               (unsigned)POLL_INTERVAL_MS,
               (unsigned)TOKENGOCHI_PASSIVE_POLL_INTERVAL_MS,
               (unsigned)TOKENGOCHI_WIFI_RECONNECT_MS,
               (unsigned)TOKENGOCHI_WIFI_SLEEP_RECONNECT_MS,
               (unsigned)TOKENGOCHI_WIFI_IDLE_OFF_MS,
               (unsigned)TOKENGOCHI_IDLE_LOOP_DELAY_MS,
               (unsigned)TOKENGOCHI_ACTIVE_LOOP_DELAY_MS,
               (unsigned)TOKENGOCHI_SLEEP_LOOP_DELAY_MS,
               (unsigned)TOKENGOCHI_PASSIVE_LIGHT_SLEEP_MS);
    out.flush();
    out.printf("TGPOWER display=%s sleeping=%d brightness=%u dim=%u dim_timeout_ms=%lu sleep_timeout_ms=%u\n",
               displayStateLabel(g_displayState),
               g_displaySleeping ? 1 : 0,
               (unsigned)g_settings.brightnessPercent,
               (unsigned)g_settings.dimBrightnessPercent,
               (unsigned long)g_settings.autoDimTimeoutMs,
               (unsigned)TOKENGOCHI_DISPLAY_SLEEP_MS);
    out.flush();
    out.printf("TGPOWER audio speaker_running=%d mic_running=%d mic_active=%d volume=%u battery_sample_ms=%u\n",
               M5.Speaker.isRunning() ? 1 : 0,
               M5.Mic.isRunning() ? 1 : 0,
               audio::micActive() ? 1 : 0,
               (unsigned)g_settings.volumePercent,
               (unsigned)TOKENGOCHI_BATTERY_SAMPLE_MS);
    out.flush();
    const uint32_t totalSleepMsLow = (uint32_t)(g_lightSleepTotalMs & 0xffffffffULL);
    const uint32_t totalSleepMsHigh = (uint32_t)(g_lightSleepTotalMs >> 32);
    out.printf("TGPOWER sleep light_enabled=%d serial_guard=%d serial_connected=%d entries=%lu total_ms_low=%lu total_ms_high=%lu last_ms=%u last_wake=%d last_err=%d timer_err=%d uart_err=%d min_ms=%u\n",
               TOKENGOCHI_LIGHT_SLEEP ? 1 : 0,
               TOKENGOCHI_LIGHT_SLEEP_WHEN_SERIAL_CONNECTED ? 0 : 1,
               Serial ? 1 : 0,
               (unsigned long)g_lightSleepEntries,
               (unsigned long)totalSleepMsLow,
               (unsigned long)totalSleepMsHigh,
               (unsigned)g_lastLightSleepMs,
               g_lastLightSleepWake,
               g_lastLightSleepErr,
               g_lastLightSleepTimerErr,
               g_lastLightSleepUartErr,
               (unsigned)TOKENGOCHI_LIGHT_SLEEP_MIN_MS);
    out.flush();
    const uint32_t now = millis();
    const uint32_t serialAwakeRemaining =
        g_serialAwakeUntilMs && (int32_t)(g_serialAwakeUntilMs - now) > 0
            ? g_serialAwakeUntilMs - now
            : 0;
    out.printf("TGPOWER serial awake_remaining_ms=%u wake_grace_ms=%u\n",
               (unsigned)serialAwakeRemaining,
               (unsigned)TOKENGOCHI_SERIAL_WAKE_AWAKE_MS);
    out.flush();
    out.printf("TGPOWER battery percent=%d percent_known=%d voltage_mv=%d voltage_known=%d current_ma=%ld current_known=%d charge=%s warning=%s sampled_ms=%lu\n",
               (int)g_battery.percent,
               g_battery.percentKnown ? 1 : 0,
               (int)g_battery.voltageMv,
               g_battery.voltageKnown ? 1 : 0,
               (long)g_battery.currentMa,
               g_battery.currentKnown ? 1 : 0,
               battery_status::chargeLabel(g_battery.charge),
               battery_status::warningLabel(g_batteryWarning),
               (unsigned long)g_battery.sampledAtMs);
    out.println("TGPOWER END");
    out.flush();
}

static bool displayVisible() {
    return !g_displaySleeping && g_displayState != display_policy::DisplayState::Asleep;
}

static uint32_t loopDelayMs() {
    if (g_mode == Mode::RECORDING) return TOKENGOCHI_ACTIVE_LOOP_DELAY_MS;
    if (g_mode == Mode::TRANSCRIBING) return TOKENGOCHI_ACTIVE_LOOP_DELAY_MS;
    if (passiveIdlePowerMode()) return TOKENGOCHI_PASSIVE_LIGHT_SLEEP_MS;
    if (!displayVisible()) return TOKENGOCHI_SLEEP_LOOP_DELAY_MS;
    switch (g_mode) {
        case Mode::IDLE:
        case Mode::VOICE_IDLE:
        case Mode::STATS:
            return TOKENGOCHI_IDLE_LOOP_DELAY_MS;
        default:
            return TOKENGOCHI_ACTIVE_LOOP_DELAY_MS;
    }
}

static bool passiveIdlePowerMode() {
    if (displayVisible()) return false;
    if (g_mode != Mode::IDLE && g_mode != Mode::VOICE_IDLE) return false;
    return !M5.Speaker.isRunning() && !M5.Mic.isRunning() && !audio::micActive();
}

static uint32_t wifiReconnectIntervalMs() {
    return passiveIdlePowerMode()
        ? TOKENGOCHI_WIFI_SLEEP_RECONNECT_MS
        : TOKENGOCHI_WIFI_RECONNECT_MS;
}

static void applyRuntimePowerPolicy(const char* source) {
    const uint32_t now = millis();
    const bool passive = passiveIdlePowerMode();
    const bool connected = g_wifiUp && WiFi.status() == WL_CONNECTED;
    if (passive &&
        connected &&
        !statePollDue(now) &&
        now - g_lastInteractionMs >= TOKENGOCHI_WIFI_IDLE_OFF_MS) {
        stopWifiRadio("passive idle", true, true);
    }

    const bool offline = !g_wifiUp || WiFi.status() != WL_CONNECTED;
    const uint32_t targetCpuMhz =
        passive && offline
            ? TOKENGOCHI_SLEEP_CPU_MHZ
            : TOKENGOCHI_CPU_MHZ;
    applyCpuFrequency(targetCpuMhz, source);
}

static bool lightSleepEligible(uint32_t delayMs) {
    if (!TOKENGOCHI_LIGHT_SLEEP) return false;
    if (delayMs < TOKENGOCHI_LIGHT_SLEEP_MIN_MS) return false;
    if (!passiveIdlePowerMode()) return false;
    if (WiFi.getMode() != WIFI_OFF) return false;
#if !TOKENGOCHI_LIGHT_SLEEP_WHEN_SERIAL_CONNECTED
    if (Serial) return false;
#endif
    if (g_wifiReconnectRequested) return false;
    if (Serial.available() > 0) return false;
    if (g_serialAwakeUntilMs && (int32_t)(g_serialAwakeUntilMs - millis()) > 0) return false;
    return true;
}

static void sleepForLoopDelay(uint32_t delayMs) {
    if (!lightSleepEligible(delayMs)) {
        delay(delayMs);
        return;
    }

    Serial.flush();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    g_lastLightSleepTimerErr = esp_sleep_enable_timer_wakeup((uint64_t)delayMs * 1000ULL);
    g_lastLightSleepUartErr = ESP_OK;
    if (TOKENGOCHI_SERIAL_WAKEUP_EDGES > 0) {
        g_lastLightSleepUartErr = uart_set_wakeup_threshold(UART_NUM_0, TOKENGOCHI_SERIAL_WAKEUP_EDGES);
        if (g_lastLightSleepUartErr == ESP_OK) {
            g_lastLightSleepUartErr = esp_sleep_enable_uart_wakeup(UART_NUM_0);
        }
    }

    if (g_lastLightSleepTimerErr != ESP_OK) {
        delay(delayMs);
        return;
    }

    if (!g_lightSleepAnnounced) {
        Serial.printf("[power] light_sleep enabled interval_ms=%u uart_err=%d\n",
                      (unsigned)delayMs,
                      g_lastLightSleepUartErr);
        g_lightSleepAnnounced = true;
        Serial.flush();
    }

    const uint32_t before = millis();
    g_lastLightSleepErr = esp_light_sleep_start();
    const uint32_t sleptMs = millis() - before;
    if (g_lastLightSleepErr == ESP_OK) {
        g_lightSleepEntries++;
        g_lightSleepTotalMs += sleptMs;
        g_lastLightSleepMs = sleptMs;
        g_lastLightSleepWake = esp_sleep_get_wakeup_cause();
        if (g_lastLightSleepWake == ESP_SLEEP_WAKEUP_UART) {
            g_serialAwakeUntilMs = millis() + TOKENGOCHI_SERIAL_WAKE_AWAKE_MS;
        }
        return;
    }

    delay(delayMs);
}

static uint32_t currentEpochSec() {
    if (g_clockEpochSec == 0) return 0;
    return g_clockEpochSec + (millis() - g_clockSyncedAtMs) / 1000UL;
}

static void syncClock(uint32_t epochSec) {
    if (epochSec < 1600000000UL) return;
    g_clockEpochSec = epochSec;
    g_clockSyncedAtMs = millis();
    transcript_log::prune(currentEpochSec());
}

static uint32_t wavDurationMs(size_t wavSize) {
    if (wavSize <= 44) return 0;
    return (uint32_t)(((wavSize - 44) * 1000UL) / 32000UL);
}

static void logBodyPreview(const char* tag, const char* body, size_t bodyLen) {
    if (!body || bodyLen == 0) return;
    size_t previewLen = bodyLen;
    if (previewLen > 240) previewLen = 240;

    char preview[241];
    memcpy(preview, body, previewLen);
    preview[previewLen] = '\0';
    for (size_t i = 0; i < previewLen; ++i) {
        if (preview[i] == '\r' || preview[i] == '\n') preview[i] = ' ';
    }
    Serial.printf("[transcribe:%s] body_preview=\"%s\"%s\n",
                  tag,
                  preview,
                  bodyLen > previewLen ? "..." : "");
}

static void drawBootScreen(const char* line2, uint16_t color) {
    ui::clearToBlack();
#if TOKENGOCHI_COMPACT_UI
    ui::target().setTextSize(2);
    ui::target().setTextColor(0x87F0, 0x0000);
    int w = ui::target().textWidth("TokenGochi");
    ui::target().setCursor(SCREEN_CX - w / 2, 18);
    ui::target().print("TokenGochi");

    ui::target().setTextSize(1);
    ui::target().setTextColor(0x7BEF, 0x0000);
    w = ui::target().textWidth(TOKENGOCHI_DEVICE_NAME);
    ui::target().setCursor(SCREEN_CX - w / 2, 48);
    ui::target().print(TOKENGOCHI_DEVICE_NAME);

    ui::target().setTextColor(color, 0x0000);
    w = ui::target().textWidth(line2);
    ui::target().setCursor(SCREEN_CX - w / 2, 74);
    ui::target().print(line2);
    ui::flush();
#else
    ui::target().setTextSize(3);
    ui::target().setTextColor(0x87F0, 0x0000);
    int w = ui::target().textWidth("TokenGochi");
    ui::target().setCursor(SCREEN_CX - w / 2, 108);
    ui::target().print("TokenGochi");

    ui::target().setTextSize(1);
    ui::target().setTextColor(0x7BEF, 0x0000);
    w = ui::target().textWidth("connecting");
    ui::target().setCursor(SCREEN_CX - w / 2, 160);
    ui::target().print("connecting");

    ui::target().setTextSize(2);
    ui::target().setTextColor(color, 0x0000);
    w = ui::target().textWidth(line2);
    ui::target().setCursor(SCREEN_CX - w / 2, 198);
    ui::target().print(line2);
    ui::flush();
#endif
}

static void handleSerialCommands() {
    static char line[32];
    static size_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0';
            if (handleImmediateSerialCommand(line)) {
                // handled
            } else if (strcmp(line, "TGSETTINGS") == 0) {
                enterSettings("serial");
            } else if (strcmp(line, "TGSETTING VOICE") == 0) {
                enterSettings("serial");
                openSettingsItem(0, "serial");
            } else if (strcmp(line, "TGSETTING BRIGHT") == 0) {
                enterSettings("serial");
                openSettingsItem(1, "serial");
            } else if (strcmp(line, "TGSETTING VOLUME") == 0) {
                enterSettings("serial");
                openSettingsItem(2, "serial");
            } else if (strcmp(line, "TGSETTING FEEDBACK") == 0) {
                enterSettings("serial");
                openSettingsItem(3, "serial");
            } else if (strcmp(line, "TGSETTING DIM") == 0) {
                enterSettings("serial");
                openSettingsItem(4, "serial");
            } else if (strcmp(line, "TGSETTING BATTERY") == 0) {
                enterSettings("serial");
                openSettingsItem(5, "serial");
            }
            len = 0;
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = c;
            line[len] = '\0';
            if (handleImmediateSerialCommand(line)) {
                len = 0;
            }
        } else {
            len = 0;
        }
    }
}

static void registerWifiEvents() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    // Surface the actual reason WiFi fails by listening to events.
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
        switch (e) {
            case ARDUINO_EVENT_WIFI_STA_START:       Serial.println("[wifi] STA start"); break;
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:  Serial.printf("[wifi] connected to AP (channel %d)\n", info.wifi_sta_connected.channel); break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                g_wifiUp = true;
                Serial.printf("[wifi] got ip: %s\n", WiFi.localIP().toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                g_wifiUp = false;
                g_bridgeUp = false;
                Serial.printf("[wifi] DISCONNECTED reason=%d/%s (SSID='%s')\n",
                              info.wifi_sta_disconnected.reason,
                              WiFi.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason),
                              info.wifi_sta_disconnected.ssid);
                break;
            }
            default: break;
        }
    });
}

static bool connectWifiCredential(const WifiCredential& credential, size_t index, uint32_t timeoutMs) {
    Serial.printf("[wifi] connecting to saved network %u/%u: '%s'\n",
                  (unsigned)(index + 1), (unsigned)kWifiNetworkCount, credential.ssid);
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(credential.ssid, credential.pass);
    applyWifiPowerPolicy("begin");
    const uint32_t stepMs = 500;
    const uint32_t attempts = (timeoutMs + stepMs - 1) / stepMs;
    for (uint32_t i = 0; i < attempts; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            applyWifiPowerPolicy("connected");
            g_wifiUp = true;
            g_lastWifiCredentialIndex = index;
            return true;
        }
        delay(stepMs);
        Serial.printf("[wifi] wait %u/%u: status=%d\n",
                      (unsigned)(i + 1),
                      (unsigned)attempts,
                      (int)WiFi.status());
    }
    return false;
}

static void stopWifiRadio(const char* source, bool preserveBridge, bool idleSleep) {
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_OFF);
        Serial.printf("[wifi] radio off source=%s\n", source);
    }
    g_wifiUp = false;
    if (!preserveBridge) {
        g_bridgeUp = false;
    }
    g_wifiRadioOffForIdle = idleSleep;
}

static void connectWifi() {
    g_wifiRadioOffForIdle = false;
    applyCpuFrequency(TOKENGOCHI_CPU_MHZ, "wifi");
    registerWifiEvents();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);

    if (g_lastWifiCredentialIndex < kWifiNetworkCount) {
        Serial.printf("[wifi] direct reconnect to saved network %u/%u: '%s'\n",
                      (unsigned)(g_lastWifiCredentialIndex + 1),
                      (unsigned)kWifiNetworkCount,
                      kWifiNetworks[g_lastWifiCredentialIndex].ssid);
        if (connectWifiCredential(kWifiNetworks[g_lastWifiCredentialIndex],
                                  g_lastWifiCredentialIndex,
                                  TOKENGOCHI_WIFI_DIRECT_CONNECT_TIMEOUT_MS)) {
            return;
        }
        Serial.println("[wifi] direct reconnect failed; scanning...");
        g_lastWifiCredentialIndex = kWifiNetworkCount;
    }

    bool visible[kWifiNetworkCount] = {};
    Serial.printf("[wifi] scanning for %u saved network(s)...\n", (unsigned)kWifiNetworkCount);
    int found = WiFi.scanNetworks(false, true);  // async=false, show_hidden=true
    if (found < 0) {
        Serial.printf("[wifi] scan failed: %d\n", found);
        found = 0;
    }
    for (int scanIndex = 0; scanIndex < found; scanIndex++) {
        for (size_t netIndex = 0; netIndex < kWifiNetworkCount; netIndex++) {
            if (WiFi.SSID(scanIndex) == kWifiNetworks[netIndex].ssid) {
                visible[netIndex] = true;
                Serial.printf("[wifi] found saved network %u/%u: '%s', ch=%d, rssi=%d\n",
                              (unsigned)(netIndex + 1), (unsigned)kWifiNetworkCount,
                              kWifiNetworks[netIndex].ssid,
                              WiFi.channel(scanIndex), WiFi.RSSI(scanIndex));
            }
        }
    }
    WiFi.scanDelete();

    bool sawAny = false;
    for (size_t i = 0; i < kWifiNetworkCount; i++) {
        sawAny = sawAny || visible[i];
        if (!visible[i]) {
            Serial.printf("[wifi] saved network %u/%u not visible: '%s'\n",
                          (unsigned)(i + 1), (unsigned)kWifiNetworkCount, kWifiNetworks[i].ssid);
            continue;
        }
        if (connectWifiCredential(kWifiNetworks[i], i, TOKENGOCHI_WIFI_CONNECT_TIMEOUT_MS)) return;
    }

    if (!sawAny) Serial.println("[wifi] !!! no saved SSIDs visible to ESP32 (2.4 GHz only)");
    stopWifiRadio("connect failed");
}

static bool criticalBatteryActive() {
    return g_settings.lowBatteryWarning &&
           g_batteryWarning == battery_status::WarningState::Critical;
}

static void applyBrightnessForDisplayState(const char* source) {
    if (g_displayState == display_policy::DisplayState::Asleep) {
        if (!g_displaySleeping) {
            M5.Display.sleep();
            g_displaySleeping = true;
            Serial.printf("[display] source=%s state=asleep\n", source);
        }
        return;
    }

    if (g_displaySleeping) {
        M5.Display.wakeup();
        g_displaySleeping = false;
    }

    const uint8_t percent = display_policy::targetBrightnessPercent(g_displayState,
                                                                    g_settings.brightnessPercent,
                                                                    g_settings.dimBrightnessPercent,
                                                                    criticalBatteryActive());
    M5.Display.setBrightness(device_settings::brightnessToHardware(percent));
    Serial.printf("[display] brightness source=%s state=%s percent=%u\n",
                  source,
                  displayStateLabel(g_displayState),
                  (unsigned)percent);
}

static void applyDeviceSettings(const char* source) {
    g_settings = device_settings::normalize(g_settings);
    g_recordSeconds = device_settings::effectiveRecordSeconds(g_settings);
    audio::setVolumePercent(g_settings.volumePercent);
    g_displayState = display_policy::DisplayState::Bright;
    applyBrightnessForDisplayState(source);
    Serial.printf("[settings] apply source=%s voice=%s cap=%lus brightness=%u volume=%u sound=%d vibe=%d dim=%s\n",
                  source,
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds,
                  (unsigned)g_settings.brightnessPercent,
                  (unsigned)g_settings.volumePercent,
                  g_settings.buttonSound ? 1 : 0,
                  g_settings.vibration ? 1 : 0,
                  device_settings::autoDimLabel(g_settings));
}

static bool saveSettings(const char* source) {
    g_settings = device_settings::normalize(g_settings);
    const bool ok = device_settings::save(g_settings);
    applyDeviceSettings(source);
    Serial.printf("[settings] save source=%s ok=%d\n", source, ok ? 1 : 0);
    return ok;
}

static void noteInteraction(const char* source) {
    g_lastInteractionMs = millis();
    if (g_displayState != display_policy::DisplayState::Bright || g_displaySleeping) {
        g_displayState = display_policy::DisplayState::Bright;
        applyBrightnessForDisplayState(source);
    }
    if (!g_wifiUp || WiFi.status() != WL_CONNECTED) {
        g_wifiReconnectRequested = true;
    }
}

static display_policy::ActivityClass displayActivityForMode(Mode mode) {
    switch (mode) {
        case Mode::IDLE: return display_policy::ActivityClass::Idle;
        case Mode::VOICE_IDLE: return display_policy::ActivityClass::VoiceIdle;
        case Mode::STATS: return display_policy::ActivityClass::Stats;
        default: return display_policy::ActivityClass::Foreground;
    }
}

static void updateDisplayPolicy() {
    const uint32_t idleMs = millis() - g_lastInteractionMs;
    const display_policy::DisplayState target =
        display_policy::targetState(displayActivityForMode(g_mode),
                                    idleMs,
                                    g_settings.autoDimEnabled,
                                    g_settings.autoDimTimeoutMs,
                                    TOKENGOCHI_DISPLAY_SLEEP_MS);
    if (target != g_displayState) {
        g_displayState = target;
        applyBrightnessForDisplayState("policy");
    }
}

static bool batteryWarningVisibleInCurrentMode() {
    if (!g_settings.lowBatteryWarning) return false;
    if (!battery_status::isWarning(g_batteryWarning)) return false;
    return g_mode == Mode::IDLE || g_mode == Mode::VOICE_IDLE || g_mode == Mode::STATS;
}

static void sampleBattery(bool force, const char* source) {
    const uint32_t now = millis();
    if (!force && g_lastBatterySampleMs && now - g_lastBatterySampleMs < TOKENGOCHI_BATTERY_SAMPLE_MS) return;

    g_battery = battery_status::readHardware(now);
    g_batteryWarning = battery_status::warningFor(g_battery,
                                                  g_settings.lowBatteryPercent,
                                                  g_settings.criticalBatteryPercent);
    g_lastBatterySampleMs = now;
    Serial.printf("[battery] source=%s percent=%d known=%d voltage=%d known=%d current=%ld known=%d charge=%s warning=%s\n",
                  source,
                  (int)g_battery.percent,
                  g_battery.percentKnown ? 1 : 0,
                  (int)g_battery.voltageMv,
                  g_battery.voltageKnown ? 1 : 0,
                  (long)g_battery.currentMa,
                  g_battery.currentKnown ? 1 : 0,
                  battery_status::chargeLabel(g_battery.charge),
                  battery_status::warningLabel(g_batteryWarning));
}

static void chirpOk() {
    if (g_settings.buttonSound && g_settings.volumePercent > 0) {
        audio::chirp(1200, 80);
    }
}

static void chirpFail() {
    if (g_settings.buttonSound && g_settings.volumePercent > 0) {
        audio::chirp(400, 200);
    }
}

static void vibrate(uint8_t strength, uint16_t durationMs) {
#if TOKENGOCHI_HAS_VIBRATION
    if (!g_settings.vibration || strength == 0 || durationMs == 0) return;
    M5.Power.setVibration(strength);
    delay(durationMs);
    M5.Power.setVibration(0);
#else
    (void)strength;
    (void)durationMs;
#endif
}

static bool buttonsSuppressed() {
    if (g_suppressButtonsUntilRelease) {
        if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
            g_suppressButtonsUntilRelease = false;
            g_suppressButtonsUntilMs = millis() + 80;
        }
        return true;
    }
    return millis() < g_suppressButtonsUntilMs;
}

static bool btnAClicked() {
    if (buttonsSuppressed()) return false;
    if (M5.BtnA.wasClicked()) {
        noteInteraction("A");
        return true;
    }
    return false;
}

static bool btnBClicked() {
    if (buttonsSuppressed()) return false;
    if (M5.BtnB.wasClicked()) {
        noteInteraction("B");
        return true;
    }
    return false;
}

static bool btnBHold() {
    if (buttonsSuppressed()) return false;
    if (M5.BtnB.wasHold()) {
        g_suppressButtonsUntilRelease = true;
        noteInteraction("B hold");
        return true;
    }
    return false;
}

static bool settingsEntryAllowed() {
    return g_mode == Mode::IDLE || g_mode == Mode::VOICE_IDLE || g_mode == Mode::STATS;
}

static uint32_t clampRecordSeconds(uint32_t seconds) {
    if (seconds <= 10) return 10;
    if (seconds <= 20) return 20;
    return 30;
}

static const char* settingsItemName(uint8_t index) {
    switch (index) {
        case 0: return "voice";
        case 1: return "brightness";
        case 2: return "volume";
        case 3: return "feedback";
        case 4: return "auto dim";
        case 5: return "battery";
        default: break;
    }
    return "settings";
}

static SettingsView settingsViewForIndex(uint8_t index) {
    switch (index) {
        case 0: return SettingsView::VOICE;
        case 1: return SettingsView::BRIGHTNESS;
        case 2: return SettingsView::VOLUME;
        case 3: return SettingsView::FEEDBACK;
        case 4: return SettingsView::AUTO_DIM;
        case 5: return SettingsView::BATTERY;
        default: break;
    }
    return SettingsView::MENU;
}

static void drawSettings() {
    ui::clearToBlack();
    sampleBattery(false, "settings");
    switch (g_settingsView) {
        case SettingsView::MENU:
            ui::drawSettingsMenu(g_settings, g_battery, g_batteryWarning, g_settingsIndex);
            break;
        case SettingsView::VOICE:
            ui::drawDurationSettings(g_recordSeconds, g_settings.recordAuto);
            break;
        case SettingsView::BRIGHTNESS:
            ui::drawPercentSetting("BRIGHT", g_settings.brightnessPercent, "tap -/+ | B +10");
            break;
        case SettingsView::VOLUME:
            ui::drawPercentSetting("VOLUME", g_settings.volumePercent, "tap -/+ | B +10");
            break;
        case SettingsView::FEEDBACK:
            ui::drawFeedbackSettings(g_settings);
            break;
        case SettingsView::AUTO_DIM:
            ui::drawAutoDimSettings(g_settings);
            break;
        case SettingsView::BATTERY:
            sampleBattery(true, "battery view");
            ui::drawBatterySettings(g_battery, g_batteryWarning, g_settings.lowBatteryWarning);
            break;
    }
}

static void setRecordSeconds(uint32_t seconds, const char* source) {
    g_settings.recordSeconds = clampRecordSeconds(seconds);
    g_settings.recordAuto = false;
    saveSettings(source);
    Serial.printf("[settings] recording_mode=%s cap=%lus source=%s\n",
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds,
                  source);
}

static void setRecordAuto(const char* source) {
    g_settings.recordAuto = true;
    g_settings.recordSeconds = device_settings::RECORD_SECONDS_DEFAULT;
    saveSettings(source);
    Serial.printf("[settings] recording_mode=%s cap=%lus source=%s\n",
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds,
                  source);
}

static void cycleRecordMode(const char* source) {
    g_settings = device_settings::cycleRecordMode(g_settings);
    saveSettings(source);
    Serial.printf("[settings] recording_mode=%s cap=%lus source=%s\n",
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds,
                  source);
}

static void showSettingsSaved(const char* label, SettingsView returnView, const char* source) {
    Serial.printf("[settings] saved label=%s source=%s\n",
                  label ? label : "settings",
                  source);
    ui::clearToBlack();
    ui::drawSettingsSaved(label);
    g_mode = Mode::SETTINGS_SAVED;
    g_settingsSavedReturnView = returnView;
    g_settingsSavedUntilMs = millis() + SETTINGS_SAVED_MS;
}

static void showDurationSaved(const char* source) {
    char label[24];
    snprintf(label, sizeof(label), "%s voice", device_settings::recordModeLabel(g_settings));
    showSettingsSaved(label, SettingsView::MENU, source);
}

static void enterSettings(const char* source) {
    Serial.printf("[btn] %s -> settings (voice=%s cap=%lus)\n",
                  source,
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds);
    ui::pageReset();
    g_mode = Mode::SETTINGS;
    g_settingsView = SettingsView::MENU;
    g_settingsIndex = 0;
    g_settingsAtMs = millis();
    sampleBattery(true, "settings entry");
    drawSettings();
}

static void openSettingsItem(uint8_t index, const char* source) {
    if (index >= SETTINGS_ITEM_COUNT) index = 0;
    g_settingsIndex = index;
    g_settingsView = settingsViewForIndex(index);
    g_settingsAtMs = millis();
    Serial.printf("[settings] open item=%s source=%s\n", settingsItemName(index), source);
    drawSettings();
}

static bool handleSettingsChord() {
    if (!settingsEntryAllowed()) {
        g_abHoldStartMs = 0;
        return false;
    }

    const bool bothPressed = M5.BtnA.isPressed() && M5.BtnB.isPressed();
    if (!bothPressed) {
        g_abHoldStartMs = 0;
        return false;
    }

    if (g_abHoldStartMs == 0) {
        g_abHoldStartMs = millis();
    }

    if (millis() - g_abHoldStartMs >= SETTINGS_CHORD_MS) {
        g_abHoldStartMs = 0;
        g_suppressButtonsUntilRelease = true;
        noteInteraction("A+B hold");
        enterSettings("A+B hold");
    }

    return true;
}

static bool touchClicked(int16_t* x, int16_t* y) {
#if !TOKENGOCHI_HAS_TOUCH
    (void)x;
    (void)y;
    return false;
#else
    if (!M5.Touch.isEnabled()) return false;
    const int count = M5.Touch.getCount();
    for (int i = 0; i < count; ++i) {
        auto detail = M5.Touch.getDetail(i);
        if (detail.wasClicked()) {
            *x = detail.x;
            *y = detail.y;
            noteInteraction("touch");
            return true;
        }
    }
    return false;
#endif
}

static bool pointInCircle(int16_t x, int16_t y, int cx, int cy, int r) {
    const int32_t dx = (int32_t)x - cx;
    const int32_t dy = (int32_t)y - cy;
    return dx * dx + dy * dy <= (int32_t)r * r;
}

static bool pointInHomeRing(int16_t x, int16_t y) {
    const int32_t dx = (int32_t)x - SCREEN_CX;
    const int32_t dy = (int32_t)y - SCREEN_CY;
    const int32_t d2 = dx * dx + dy * dy;
    return d2 >= (int32_t)178 * 178 && d2 <= (int32_t)233 * 233;
}

static int settingsMenuItemFromTouch(int16_t x, int16_t y) {
    static constexpr int CIRCLE_R = 54;
    static constexpr int OPTION_X[] = {142, 324, 142, 324, 142, 324};
    static constexpr int OPTION_Y[] = {160, 160, 250, 250, 340, 340};
    for (uint8_t i = 0; i < SETTINGS_ITEM_COUNT; ++i) {
        if (pointInCircle(x, y, OPTION_X[i], OPTION_Y[i], CIRCLE_R)) {
            return i;
        }
    }
    return -1;
}

static int percentAdjustmentFromTouch(int16_t x, int16_t y) {
    if (pointInCircle(x, y, SCREEN_CX - 86, SCREEN_CY + 74, 58)) return -10;
    if (pointInCircle(x, y, SCREEN_CX + 86, SCREEN_CY + 74, 58)) return 10;
    return 0;
}

static uint32_t durationFromTouch(int16_t x, int16_t y) {
    static constexpr int CIRCLE_R = 54;
    static constexpr int OPTION_X[] = {233, 112, 233, 354};
    static constexpr int OPTION_Y[] = {176, 272, 272, 272};
    static constexpr uint32_t OPTIONS[] = {0, 10, 20, 30};

    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); ++i) {
        if (pointInCircle(x, y, OPTION_X[i], OPTION_Y[i], CIRCLE_R)) {
            return OPTIONS[i];
        }
    }
    return UINT32_MAX;
}

static void changeBrightness(int delta, const char* source) {
    g_settings.brightnessPercent = device_settings::adjustBrightness(g_settings.brightnessPercent, delta);
    saveSettings(source);
    g_settingsAtMs = millis();
    drawSettings();
}

static void changeVolume(int delta, const char* source) {
    g_settings.volumePercent = device_settings::adjustVolume(g_settings.volumePercent, delta);
    saveSettings(source);
    chirpOk();
    g_settingsAtMs = millis();
    drawSettings();
}

static void cycleFeedback(const char* source) {
    if (g_settings.buttonSound && g_settings.vibration) {
        g_settings.buttonSound = false;
        g_settings.vibration = true;
    } else if (!g_settings.buttonSound && g_settings.vibration) {
        g_settings.buttonSound = true;
        g_settings.vibration = false;
    } else if (g_settings.buttonSound && !g_settings.vibration) {
        g_settings.buttonSound = false;
        g_settings.vibration = false;
    } else {
        g_settings.buttonSound = true;
        g_settings.vibration = true;
    }
    saveSettings(source);
    g_settingsAtMs = millis();
    drawSettings();
}

static void cycleAutoDim(const char* source) {
    g_settings = device_settings::cycleAutoDim(g_settings);
    saveSettings(source);
    g_settingsAtMs = millis();
    drawSettings();
}

static void toggleBatteryWarnings(const char* source) {
    g_settings.lowBatteryWarning = !g_settings.lowBatteryWarning;
    saveSettings(source);
    sampleBattery(true, source);
    g_settingsAtMs = millis();
    drawSettings();
}

static void pageTranscript(const char* source) {
    Serial.printf("[transcript] next page source=%s\n", source);
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::pageNext();
    ui::drawTranscript(g_lastTranscript, g_transcriptTitle, g_transcriptFooter);
}

static void formatHistoryAge(const transcript_log::Entry& entry, char* out, size_t outSize) {
    uint32_t nowSec = currentEpochSec();
    if (entry.createdAtSec > 0 && nowSec >= entry.createdAtSec) {
        uint32_t age = nowSec - entry.createdAtSec;
        if (age < 60) {
            snprintf(out, outSize, "now");
        } else if (age < 3600) {
            snprintf(out, outSize, "%lum ago", (unsigned long)(age / 60));
        } else if (age < 86400) {
            snprintf(out, outSize, "%luh ago", (unsigned long)(age / 3600));
        } else {
            snprintf(out, outSize, "%lud ago", (unsigned long)(age / 86400));
        }
        return;
    }

    if (entry.uptimeMs > 0 && millis() >= entry.uptimeMs) {
        uint32_t age = (millis() - entry.uptimeMs) / 1000UL;
        if (age < 60) {
            snprintf(out, outSize, "this boot");
        } else if (age < 3600) {
            snprintf(out, outSize, "%lum this boot", (unsigned long)(age / 60));
        } else {
            snprintf(out, outSize, "%luh this boot", (unsigned long)(age / 3600));
        }
        return;
    }

    snprintf(out, outSize, "saved");
}

static void formatHistoryMeta(const transcript_log::Entry& entry, char* out, size_t outSize) {
    char age[20];
    formatHistoryAge(entry, age, sizeof(age));

    char duration[12] = "";
    if (entry.durationSecondsX10 > 0) {
        snprintf(duration, sizeof(duration), "  %u.%us",
                 (unsigned)(entry.durationSecondsX10 / 10),
                 (unsigned)(entry.durationSecondsX10 % 10));
    }

    if (entry.lang[0]) {
        snprintf(out, outSize, "%s%s  %s", age, duration, entry.lang);
    } else {
        snprintf(out, outSize, "%s%s", age, duration);
    }
}

static void drawHistoryListView(const char* source) {
    transcript_log::prune(currentEpochSec());
    const size_t total = transcript_log::count();
    if (total == 0) {
        g_historyIndex = 0;
        Serial.printf("[history] draw empty source=%s\n", source);
        ui::clearToBlack();
        ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
        ui::drawHistoryList(0, 0, "", "");
        return;
    }

    if (g_historyIndex >= total) g_historyIndex = 0;
    transcript_log::Entry entry;
    if (!transcript_log::getNewest(g_historyIndex, entry)) return;

    char meta[48];
    formatHistoryMeta(entry, meta, sizeof(meta));
    Serial.printf("[history] draw index=%u total=%u source=%s\n",
                  (unsigned)g_historyIndex,
                  (unsigned)total,
                  source);
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawHistoryList(total, g_historyIndex, meta, entry.text);
}

static void enterHistoryList(const char* source) {
    Serial.printf("[btn] %s -> history\n", source);
    ui::pageReset();
    g_mode = Mode::HISTORY_LIST;
    g_historyIndex = 0;
    drawHistoryListView(source);
}

static void openHistoryEntry(const char* source) {
    const size_t total = transcript_log::count();
    if (total == 0) {
        returnToPet("history empty");
        return;
    }
    if (g_historyIndex >= total) g_historyIndex = 0;

    transcript_log::Entry entry;
    if (!transcript_log::getNewest(g_historyIndex, entry)) return;

    char age[20];
    formatHistoryAge(entry, age, sizeof(age));
    snprintf(g_transcriptTitle, sizeof(g_transcriptTitle), "%u/%u  %s",
             (unsigned)(g_historyIndex + 1),
             (unsigned)total,
             age);
    snprintf(g_transcriptFooter, sizeof(g_transcriptFooter), "A/tap page  B list");
    strncpy(g_lastTranscript, entry.text, sizeof(g_lastTranscript) - 1);
    g_lastTranscript[sizeof(g_lastTranscript) - 1] = '\0';

    Serial.printf("[history] open index=%u total=%u source=%s\n",
                  (unsigned)g_historyIndex,
                  (unsigned)total,
                  source);
    ui::pageReset();
    g_mode = Mode::HISTORY_READING;
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawTranscript(g_lastTranscript, g_transcriptTitle, g_transcriptFooter);
}

// Pull transcription fields out of a JSON response like
// `{"text": "...", "duration_s": 1.2, "lang": "en", ...}`.
static void extractTranscriptResult(const char* json,
                                    char* out,
                                    size_t outSize,
                                    float* durationSeconds,
                                    char* langOut,
                                    size_t langOutSize) {
    out[0] = '\0';
    if (durationSeconds) *durationSeconds = 0.0f;
    if (langOut && langOutSize) langOut[0] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    const char* t = doc["text"] | "";
    strncpy(out, t, outSize - 1);
    out[outSize - 1] = '\0';
    if (durationSeconds) *durationSeconds = doc["duration_s"] | 0.0f;
    if (langOut && langOutSize) {
        const char* lang = doc["lang"] | "";
        strncpy(langOut, lang, langOutSize - 1);
        langOut[langOutSize - 1] = '\0';
    }
}

static void beginRecording(const char* source) {
    Serial.printf("[btn] %s -> arming mic (voice=%s cap=%lus)\n",
                  source,
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)g_recordSeconds);
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawArming();
    audio::startRecording(g_recordSeconds);
    audio::pumpRecording();
    Serial.printf("[rec] ready mode=%s cap_s=%lu max_wav_bytes=%u\n",
                  device_settings::recordModeLabel(g_settings),
                  (unsigned long)audio::maxDurationSeconds(),
                  (unsigned)audio::MAX_WAV_BYTES);
    g_mode = Mode::RECORDING;

    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawRec(0, g_recordSeconds, g_settings.recordAuto);
    g_lastRecRedraw = 0;
}

static void drawPetHome() {
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    if (g_bridgeUp) {
        ui::drawMood(g_state);
    } else {
        ui::drawOffline("retrying");
    }
    if (batteryWarningVisibleInCurrentMode()) {
        ui::drawHintLine(g_batteryWarning == battery_status::WarningState::Critical
                             ? "battery critical"
                             : "battery low");
    }
}

static void returnToPet(const char* source) {
    Serial.printf("[btn] %s -> pet\n", source);
    ui::pageReset();
    g_mode = Mode::IDLE;
    g_lastPoll = 0;
    drawPetHome();
}

static void enterVoiceIdle(const char* source) {
    Serial.printf("[btn] %s -> voice idle (mic=%d)\n", source, audio::micActive() ? 1 : 0);
    ui::pageReset();
    g_mode = Mode::VOICE_IDLE;
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawVoiceReady();
}

static void finishRecording(const char* source, bool buzz) {
    Serial.printf("[btn] %s -> transcribe\n", source);
    if (buzz) {
        vibrate(120, 80);
    }
    const uint8_t* wav = nullptr;
    size_t sz = 0;
    audio::stopRecording(&wav, &sz);
    if (wav && sz > 44) {
        const audio::CaptureStats& stats = audio::lastStats();
        if (!audio::lastClipHasSpeech()) {
            Serial.printf("[rec] quiet clip rejected raw_ms=%u speech_ms=%u voice_slots=%u\n",
                          (unsigned)stats.rawDurationMs,
                          (unsigned)stats.speechMs,
                          (unsigned)stats.voiceSlots);
            strncpy(g_lastError, "quiet", sizeof(g_lastError) - 1);
            g_lastError[sizeof(g_lastError) - 1] = '\0';
            g_mode = Mode::ERROR;
            g_errorAtMs = millis();
            chirpFail();
            return;
        }
        transcribeAndShow(wav, sz);
    } else {
        Serial.println("[rec] nothing captured");
        chirpFail();
        g_mode = Mode::IDLE;
        drawPetHome();
    }
}

static void cancelRecording(const char* source) {
    Serial.printf("[btn] %s -> cancel recording\n", source);
    audio::cancelRecording();
    returnToPet(source);
}

// POST a recorded clip and update the FSM.
static void transcribeAndShow(const uint8_t* wav, size_t size) {
    g_mode = Mode::TRANSCRIBING;
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, true);
    ui::drawThinking();

    char body[1024];
    size_t bodyLen = 0;
    const audio::CaptureStats& stats = audio::lastStats();
    Serial.printf("[rec:wav] slots=%u raw_ms=%u trim_start_ms=%u send_ms=%u speech_ms=%u voice_slots=%u trimmed=%d min=%d max=%d peak=%u rms=%u zc=%u\n",
                  (unsigned)stats.slots,
                  (unsigned)stats.rawDurationMs,
                  (unsigned)stats.trimStartMs,
                  (unsigned)stats.durationMs,
                  (unsigned)stats.speechMs,
                  (unsigned)stats.voiceSlots,
                  stats.trimmed ? 1 : 0,
                  (int)stats.minSample,
                  (int)stats.maxSample,
                  (unsigned)stats.peakAbs,
                  (unsigned)stats.rms,
                  (unsigned)stats.zeroCrossings);
    Serial.printf("[transcribe:req] bytes=%u duration_ms=%u timeout_ms=%u endpoint=%s/transcribe\n",
                  (unsigned)size,
                  (unsigned)wavDurationMs(size),
                  (unsigned)TRANSCRIBE_TIMEOUT_MS,
                  PROXY_URL);
    int code = net::postTranscribe(wav, size, body, sizeof(body), &bodyLen);
    Serial.printf("[transcribe:res] wav=%u code=%d bodyLen=%u\n",
                  (unsigned)size, code, (unsigned)bodyLen);
    logBodyPreview(code == 200 ? "res" : "err", body, bodyLen);

    if (code == 200) {
        char text[TRANSCRIPT_LOG_TEXT_BYTES];
        char lang[TRANSCRIPT_LOG_LANG_BYTES];
        float durationSeconds = 0.0f;
        extractTranscriptResult(body, text, sizeof(text),
                                &durationSeconds, lang, sizeof(lang));
        if (text[0]) {
            Serial.printf("[transcribe] text=%s\n", text);
            strncpy(g_lastTranscript, text, sizeof(g_lastTranscript) - 1);
            g_lastTranscript[sizeof(g_lastTranscript) - 1] = '\0';
            snprintf(g_transcriptTitle, sizeof(g_transcriptTitle), "transcript");
            snprintf(g_transcriptFooter, sizeof(g_transcriptFooter), "A/tap page  B done");
            if (!transcript_log::add(text, durationSeconds, lang, currentEpochSec())) {
                Serial.println("[tlog] add failed");
            }
            g_mode = Mode::SHOWING;
            chirpOk();
            vibrate(120, 80);
        } else {
            strncpy(g_lastError, "empty", sizeof(g_lastError) - 1);
            g_mode = Mode::ERROR;
            g_errorAtMs = millis();
            chirpFail();
        }
    } else {
        snprintf(g_lastError, sizeof(g_lastError), "HTTP %d", code);
        g_mode = Mode::ERROR;
        g_errorAtMs = millis();
        chirpFail();
    }
}

// --- setup -----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] token tamagotchi");

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.output_power = false;
    cfg.led_brightness = 0;
    cfg.internal_imu = false;
    cfg.external_imu = false;
    cfg.internal_rtc = false;
    cfg.external_rtc = false;
#if defined(TOKENGOCHI_DEVICE_M5STICKC_PLUS2)
    cfg.fallback_board = m5::board_t::board_M5StickCPlus2;
#endif
    M5.begin(cfg);
    applyCpuFrequency(TOKENGOCHI_CPU_MHZ, "boot");
#if defined(TOKENGOCHI_DEVICE_M5STICKC_PLUS2)
    M5.Display.setRotation(1);
#endif
    Serial.printf("[boot] M5 ok, device=%s board=%d display=%dx%d heap=%u psram=%u\n",
                  TOKENGOCHI_DEVICE_NAME,
                  (int)M5.getBoard(),
                  (int)M5.Display.width(),
                  (int)M5.Display.height(),
                  ESP.getFreeHeap(), ESP.getPsramSize());

    ui::init();
    ui::clearToBlack();

    audio::init();
    if (!device_settings::load(g_settings)) {
        Serial.println("[settings] load failed, using defaults");
    }
    applyDeviceSettings("boot");
    g_lastInteractionMs = millis();
    sampleBattery(true, "boot");
    transcript_log::init(0);

    drawBootScreen("", 0xFFFF);
    connectWifi();
    if (!g_wifiUp) {
        drawBootScreen("wifi failed - check secrets.h", 0xF800);
        return;
    }
    drawBootScreen(WiFi.localIP().toString().c_str(), 0x07E0);
    Serial.printf("[boot] wifi up, ip=%s\n", WiFi.localIP().toString().c_str());
    delay(800);
    ui::clearToBlack();

    g_bridgeUp = net::pingBridge();
    g_lastPoll = millis();
    Serial.printf("[boot] bridge %s (http=%d)\n",
                  g_bridgeUp ? "ok" : "down", net::lastStatus());

    // First real poll: land directly on the live home screen.
    if (g_bridgeUp) {
        PetState s;
        if (net::fetchPetState(s)) {
            g_state = s;
            g_lastPoll = millis();
            syncClock(g_state.ts);
            g_greetingUntilMs = 0;
            drawPetHome();
        }
    }
}

// --- loop ------------------------------------------------------------------
void loop() {
    M5.update();
    handleSerialCommands();
    sampleBattery(false, "loop");

    if (handleSettingsChord()) {
        sleepForLoopDelay(loopDelayMs());
        return;
    }

    // --- WiFi watchdog ------------------------------------------------------
    if (!g_wifiUp || WiFi.status() != WL_CONNECTED) {
        g_wifiUp = false;
        g_bridgeUp = false;
        if (passiveIdlePowerMode() && WiFi.getMode() != WIFI_OFF) {
            stopWifiRadio("offline idle");
        }

        const uint32_t now = millis();
        const bool pollDueForIdleRadio =
            g_wifiRadioOffForIdle && g_mode == Mode::IDLE && statePollDue(now);
        const bool waitingForPollOrInteraction =
            g_wifiRadioOffForIdle && passiveIdlePowerMode();
        const uint32_t reconnectIntervalMs = wifiReconnectIntervalMs();
        const bool periodicReconnectDue =
            !waitingForPollOrInteraction && now - g_lastRecon > reconnectIntervalMs;
        if (g_wifiReconnectRequested ||
            pollDueForIdleRadio ||
            periodicReconnectDue) {
            g_wifiReconnectRequested = false;
            g_lastRecon = now;
            Serial.printf("[wifi] reconnecting interval_ms=%u...\n",
                          (unsigned)(pollDueForIdleRadio ? 0 : reconnectIntervalMs));
            connectWifi();
        }
    }

    // --- state machine ------------------------------------------------------
    switch (g_mode) {

    case Mode::IDLE: {
        // Poll /pet/state every POLL_INTERVAL_MS
        if (g_wifiUp && statePollDue(millis())) {
            g_lastPoll = millis();
            PetState s;
            if (net::fetchPetState(s)) {
                g_state    = s;
                g_bridgeUp = true;
                syncClock(g_state.ts);
                Serial.printf("[poll] mood=%s food=%ld age=%ld ts=%ld\n",
                              s.mood, (long)s.food_today, (long)s.age_s, (long)s.ts);
                if (displayVisible()) drawPetHome();
            } else {
                g_bridgeUp = false;
                Serial.printf("[poll] bridge down (http=%d)\n", net::lastStatus());
                if (displayVisible()) drawPetHome();
            }
        }

        // KEYB hold -> stats view
        if (g_bridgeUp && btnBHold()) {
            Serial.println("[btn] B hold -> stats");
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            break;
        }

        // KEYA short -> device-only transcript history.
        if (btnAClicked()) {
            enterHistoryList("A click");
            break;
        }

        // KEYB short -> voice input mode. Recording starts only on the next B.
        if (btnBClicked()) {
            enterVoiceIdle("B click");
            break;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;
        if (g_bridgeUp && touchClicked(&touchX, &touchY) && pointInHomeRing(touchX, touchY)) {
            Serial.println("[touch] home ring -> stats");
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            break;
        }

        // Animate the blink on the pet
        if (g_bridgeUp &&
            !g_greetingUntilMs &&
            g_displayState == display_policy::DisplayState::Bright &&
            displayVisible() &&
            pet_sprite::tickBlink()) {
            pet_sprite::drawCentered(pet_sprite::moodIndex(g_state.mood),
                                     pet_sprite::currentFrame(),
                                     TOKENGOCHI_HOME_PET_SCALE);
        }

        // Hide the greeting overlay once the timer expires
        if (g_greetingUntilMs && millis() > g_greetingUntilMs && displayVisible()) {
            g_greetingUntilMs = 0;
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            ui::drawMood(g_state);
        }
        break;
    }

    case Mode::VOICE_IDLE: {
        if (btnAClicked()) {
            returnToPet("A click from voice");
            break;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;
        if (g_wifiUp && touchClicked(&touchX, &touchY) &&
            pointInCircle(touchX, touchY, SCREEN_CX, SCREEN_CY + 18, 92)) {
            beginRecording("touch mic from voice");
            break;
        }

        if (g_wifiUp && btnBClicked()) {
            beginRecording("B click from voice");
            break;
        }
        break;
    }

    case Mode::STATS: {
        // KEYB short -> confirm overlay (reset prompt)
        if (btnBClicked()) {
            Serial.println("[btn] B click -> confirm");
            g_mode = Mode::CONFIRM;
            g_confirmAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            ui::drawConfirmReset();
            break;
        }
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (touchClicked(&touchX, &touchY)) {
            returnToPet("touch from stats");
            break;
        }
        // A press -> back to home
        if (btnAClicked()) {
            returnToPet("A click from stats");
            break;
        }
        // Auto-dismiss
        if (millis() - g_statsAtMs > STATS_TIMEOUT_MS) {
            returnToPet("stats timeout");
        }
        break;
    }

    case Mode::CONFIRM: {
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (touchClicked(&touchX, &touchY)) {
            if (pointInCircle(touchX, touchY, SCREEN_CX - 70, SCREEN_CY + 38, 48)) {
                Serial.println("[touch] confirm yes -> reset");
                ui::clearToBlack();
                ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
                ui::target().setTextSize(2);
                ui::target().setTextColor(0x07E0, 0x0000);
                const char* t = "resetting...";
                int w = ui::target().textWidth(t);
                ui::target().setCursor(SCREEN_CX - w / 2, SCREEN_CY);
                ui::target().print(t);
                ui::flush();

                PetState fresh;
                int code = net::postReset(fresh);
                if (code == 200) {
                    g_state = fresh;
                    syncClock(g_state.ts);
                    g_mode = Mode::IDLE;
                    g_lastPoll = 0;
                    chirpOk();
                    vibrate(140, 120);
                    drawPetHome();
                } else {
                    snprintf(g_lastError, sizeof(g_lastError), "HTTP %d", code);
                    g_mode = Mode::ERROR;
                    g_errorAtMs = millis();
                    chirpFail();
                }
                break;
            }
            if (pointInCircle(touchX, touchY, SCREEN_CX + 70, SCREEN_CY + 38, 48)) {
                Serial.println("[touch] confirm no -> stats");
                g_mode = Mode::STATS;
                g_statsAtMs = millis();
                ui::clearToBlack();
                ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
                break;
            }
        }

        if (btnAClicked()) {
            Serial.println("[btn] A click -> reset!");
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            ui::target().setTextSize(2);
            ui::target().setTextColor(0x07E0, 0x0000);
            const char* t = "resetting...";
            int w = ui::target().textWidth(t);
            ui::target().setCursor(SCREEN_CX - w / 2, SCREEN_CY);
            ui::target().print(t);
            ui::flush();

            PetState fresh;
            int code = net::postReset(fresh);
            if (code == 200) {
                g_state = fresh;
                syncClock(g_state.ts);
                g_mode = Mode::IDLE;
                g_lastPoll = 0;
                chirpOk();
                vibrate(140, 120);
                drawPetHome();
            } else {
                snprintf(g_lastError, sizeof(g_lastError), "HTTP %d", code);
                g_mode = Mode::ERROR;
                g_errorAtMs = millis();
                chirpFail();
            }
            break;
        }
        if (btnBClicked()) {
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            break;
        }
        if (millis() - g_confirmAtMs > CONFIRM_TIMEOUT_MS) {
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
        }
        break;
    }

    case Mode::SETTINGS: {
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (touchClicked(&touchX, &touchY)) {
            if (g_settingsView == SettingsView::MENU) {
                int item = settingsMenuItemFromTouch(touchX, touchY);
                if (item >= 0) {
                    openSettingsItem((uint8_t)item, "touch");
                    break;
                }
            } else if (g_settingsView == SettingsView::VOICE) {
                uint32_t seconds = durationFromTouch(touchX, touchY);
                if (seconds != UINT32_MAX) {
                    if (seconds == 0) {
                        setRecordAuto("touch");
                    } else {
                        setRecordSeconds(seconds, "touch");
                    }
                    chirpOk();
                    vibrate(90, 70);
                    showDurationSaved("touch");
                    break;
                }
            } else if (g_settingsView == SettingsView::BRIGHTNESS) {
                int delta = percentAdjustmentFromTouch(touchX, touchY);
                if (delta != 0) {
                    changeBrightness(delta, "touch brightness");
                    break;
                }
            } else if (g_settingsView == SettingsView::VOLUME) {
                int delta = percentAdjustmentFromTouch(touchX, touchY);
                if (delta != 0) {
                    changeVolume(delta, "touch volume");
                    break;
                }
            } else if (g_settingsView == SettingsView::FEEDBACK) {
                if (pointInCircle(touchX, touchY, SCREEN_CX - 76, SCREEN_CY + 12, 72)) {
                    g_settings.buttonSound = !g_settings.buttonSound;
                    saveSettings("touch sound");
                    g_settingsAtMs = millis();
                    drawSettings();
                    break;
                }
                if (pointInCircle(touchX, touchY, SCREEN_CX + 76, SCREEN_CY + 12, 72)) {
                    g_settings.vibration = !g_settings.vibration;
                    saveSettings("touch vibration");
                    g_settingsAtMs = millis();
                    drawSettings();
                    break;
                }
            } else if (g_settingsView == SettingsView::AUTO_DIM) {
                cycleAutoDim("touch auto dim");
                break;
            } else if (g_settingsView == SettingsView::BATTERY) {
                sampleBattery(true, "touch battery refresh");
                g_settingsAtMs = millis();
                drawSettings();
                break;
            }
            g_settingsAtMs = millis();
        }

        if (btnBHold()) {
            if (g_settingsView == SettingsView::MENU) {
                openSettingsItem(g_settingsIndex, "B hold");
            } else {
                g_settingsView = SettingsView::MENU;
                g_settingsAtMs = millis();
                drawSettings();
            }
            break;
        }

        if (btnBClicked()) {
            if (g_settingsView == SettingsView::MENU) {
                g_settingsIndex = (g_settingsIndex + 1) % SETTINGS_ITEM_COUNT;
                drawSettings();
            } else if (g_settingsView == SettingsView::VOICE) {
                cycleRecordMode("B duration");
                drawSettings();
            } else if (g_settingsView == SettingsView::BRIGHTNESS) {
                changeBrightness(10, "B brightness");
            } else if (g_settingsView == SettingsView::VOLUME) {
                changeVolume(10, "B volume");
            } else if (g_settingsView == SettingsView::FEEDBACK) {
                cycleFeedback("B feedback");
            } else if (g_settingsView == SettingsView::AUTO_DIM) {
                cycleAutoDim("B auto dim");
            } else if (g_settingsView == SettingsView::BATTERY) {
                toggleBatteryWarnings("B battery warning");
            }
            g_settingsAtMs = millis();
            break;
        }

        if (btnAClicked()) {
            if (g_settingsView == SettingsView::MENU) {
                returnToPet("A click from settings");
            } else {
                g_settingsView = SettingsView::MENU;
                g_settingsAtMs = millis();
                drawSettings();
            }
            break;
        }

        if (millis() - g_settingsAtMs > SETTINGS_TIMEOUT_MS) {
            returnToPet("settings timeout");
        }
        break;
    }

    case Mode::SETTINGS_SAVED: {
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (millis() >= g_settingsSavedUntilMs ||
            btnAClicked() ||
            btnBClicked() ||
            touchClicked(&touchX, &touchY)) {
            g_mode = Mode::SETTINGS;
            g_settingsView = g_settingsSavedReturnView;
            g_settingsAtMs = millis();
            drawSettings();
        }
        break;
    }

    case Mode::RECORDING: {
        audio::pumpRecording();

        // Redraw only when the visible countdown changes. Avoid pushing
        // black/status-only frames while the mic is running.
        const uint32_t elapsedS = audio::elapsedSeconds();
        if (elapsedS != g_lastRecRedraw) {
            g_lastRecRedraw = elapsedS;
            ui::drawRec(elapsedS, g_recordSeconds, g_settings.recordAuto);
        }

        // A returns to TokenGochi without uploading an unintended clip.
        if (M5.BtnA.wasPressed()) {
            cancelRecording("A press while recording");
            break;
        }

        // B completes the voice capture and sends it for transcription.
        if (btnBClicked()) {
            finishRecording("B click while recording", false);
            break;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;
        if (touchClicked(&touchX, &touchY) &&
            pointInCircle(touchX, touchY, SCREEN_CX, SCREEN_CY + 10, 92)) {
            finishRecording("touch mic while recording", false);
            break;
        }

        if (g_settings.recordAuto && audio::autoStopReady()) {
            Serial.println("[rec] auto voice pause stop");
            finishRecording("voice pause", true);
            break;
        }

        // Force-stop at the selected duration.
        if (audio::elapsedMillis() >= g_recordSeconds * 1000UL) {
            Serial.printf("[rec] force-stop at %lus\n", (unsigned long)g_recordSeconds);
            char reason[24];
            snprintf(reason, sizeof(reason), "%lus auto-stop", (unsigned long)g_recordSeconds);
            finishRecording(reason, true);
        }
        break;
    }

    case Mode::TRANSCRIBING:
        // The thinking overlay is drawn in transcribeAndShow; we just wait
        // for the HTTP call to return (it blocks within transcribeAndShow,
        // so this state is transient).
        break;

    case Mode::SHOWING: {
        // Redraw the transcript on first entry. Short A pages, B dismisses.
        static bool s_drawn = false;
        if (!s_drawn) {
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            ui::drawTranscript(g_lastTranscript, g_transcriptTitle, g_transcriptFooter);
            s_drawn = true;
        }
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (btnAClicked()) {
            pageTranscript("A");
        } else if (touchClicked(&touchX, &touchY)) {
            pageTranscript("touch");
        }
        if (btnBClicked()) {
            s_drawn = false;
            ui::pageReset();
            g_mode = Mode::IDLE;
            // Force a fresh poll on the next iteration
            g_lastPoll = 0;
        }
        break;
    }

    case Mode::HISTORY_LIST: {
        if (transcript_log::count() == 0) {
            if (btnAClicked() || btnBClicked()) {
                returnToPet("history empty");
            }
            break;
        }

        if (btnBHold()) {
            returnToPet("B hold from history");
            break;
        }

        if (btnAClicked()) {
            openHistoryEntry("A click");
            break;
        }

        if (btnBClicked()) {
            g_historyIndex++;
            if (g_historyIndex >= transcript_log::count()) g_historyIndex = 0;
            drawHistoryListView("B cycle");
            break;
        }

        int16_t touchX = 0;
        int16_t touchY = 0;
        if (touchClicked(&touchX, &touchY)) {
            openHistoryEntry("touch");
        }
        break;
    }

    case Mode::HISTORY_READING: {
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (btnAClicked()) {
            pageTranscript("A history");
        } else if (touchClicked(&touchX, &touchY)) {
            pageTranscript("touch history");
        }
        if (btnBClicked()) {
            ui::pageReset();
            g_mode = Mode::HISTORY_LIST;
            drawHistoryListView("B from history entry");
        }
        break;
    }

    case Mode::ERROR: {
        static bool s_drawn = false;
        if (!s_drawn) {
            ui::clearToBlack();
            ui::drawOffline(g_lastError);
            s_drawn = true;
        }
        if (millis() - g_errorAtMs > ERROR_HOLD_MS) {
            s_drawn = false;
            g_mode = Mode::IDLE;
            g_lastPoll = 0;
        }
        // Any button press to dismiss early
        int16_t touchX = 0;
        int16_t touchY = 0;
        if (btnAClicked() || btnBClicked() || touchClicked(&touchX, &touchY)) {
            s_drawn = false;
            g_mode = Mode::IDLE;
            g_lastPoll = 0;
        }
        break;
    }
    }

    // Cheap liveness ping for the watchdog.
    updateDisplayPolicy();
    applyRuntimePowerPolicy("loop");
    sleepForLoopDelay(loopDelayMs());
}
