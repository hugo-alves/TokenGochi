#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"
#include "pet_state.h"
#include "net.h"
#include "ui.h"
#include "pet_sprite.h"
#include "audio.h"
#include "transcript_log.h"

#include <ArduinoJson.h>

// --- globals ---------------------------------------------------------------
static PetState     g_state;
static bool         g_wifiUp     = false;
static bool         g_bridgeUp   = false;
static uint32_t     g_lastPoll   = 0;
static uint32_t     g_lastRecon  = 0;
static uint32_t     g_lastRecRedraw = 0;
static uint32_t     g_recordSeconds = audio::DEFAULT_SECONDS;
static uint32_t     g_clockEpochSec = 0;
static uint32_t     g_clockSyncedAtMs = 0;
static size_t       g_historyIndex = 0;

enum class Mode : uint8_t { IDLE, VOICE_IDLE, RECORDING, TRANSCRIBING, SHOWING, HISTORY_LIST, HISTORY_READING, STATS, CONFIRM, SETTINGS, SETTINGS_SAVED, ERROR };
static Mode         g_mode = Mode::IDLE;
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

// --- helpers ---------------------------------------------------------------
static void transcribeAndShow(const uint8_t* wav, size_t size);
static void returnToPet(const char* source);

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
}

static void handleSerialCommands() {
    static char line[32];
    static size_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0';
            if (strcmp(line, "TGSHOT") == 0) {
                ui::writeScreenshot(Serial);
            }
            len = 0;
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = c;
        } else {
            len = 0;
        }
    }
}

static void connectWifi() {
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

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    Serial.printf("[wifi] scanning for SSID '%s'...\n", WIFI_SSID);
    int found = WiFi.scanNetworks(false, true);  // async=false, show_hidden=true
    bool saw = false;
    for (int i = 0; i < found; i++) {
        if (WiFi.SSID(i) == WIFI_SSID) {
            Serial.printf("[wifi] found target SSID, ch=%d, rssi=%d\n", WiFi.channel(i), WiFi.RSSI(i));
            saw = true;
        }
    }
    WiFi.scanDelete();
    if (!saw) Serial.println("[wifi] !!! target SSID NOT visible to ESP32 (2.4 GHz only)");

    Serial.println("[wifi] connecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40; i++) {
        if (WiFi.status() == WL_CONNECTED) { g_wifiUp = true; return; }
        delay(500);
        Serial.printf("[wifi] wait %d: status=%d\n", i + 1, (int)WiFi.status());
    }
    g_wifiUp = false;
}

static void chirpOk()   { audio::chirp(1200, 80); }
static void chirpFail() { audio::chirp(400, 200); }

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
    return M5.BtnA.wasClicked();
}

static bool btnBClicked() {
    if (buttonsSuppressed()) return false;
    return M5.BtnB.wasClicked();
}

static bool btnBHold() {
    if (buttonsSuppressed()) return false;
    if (M5.BtnB.wasHold()) {
        g_suppressButtonsUntilRelease = true;
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

static void drawSettings() {
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawDurationSettings(g_recordSeconds);
}

static void setRecordSeconds(uint32_t seconds, const char* source) {
    g_recordSeconds = clampRecordSeconds(seconds);
    Serial.printf("[settings] recording_duration=%lus source=%s\n",
                  (unsigned long)g_recordSeconds,
                  source);
}

static void showDurationSaved(const char* source) {
    Serial.printf("[settings] saved recording_duration=%lus source=%s\n",
                  (unsigned long)g_recordSeconds,
                  source);
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawDurationSaved(g_recordSeconds);
    g_mode = Mode::SETTINGS_SAVED;
    g_settingsSavedUntilMs = millis() + SETTINGS_SAVED_MS;
}

static void enterDurationSettings(const char* source) {
    Serial.printf("[btn] %s -> settings (recording_duration=%lus)\n",
                  source,
                  (unsigned long)g_recordSeconds);
    ui::pageReset();
    g_mode = Mode::SETTINGS;
    g_settingsAtMs = millis();
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
        enterDurationSettings("A+B hold");
    }

    return true;
}

static bool touchClicked(int16_t* x, int16_t* y) {
    if (!M5.Touch.isEnabled()) return false;
    const int count = M5.Touch.getCount();
    for (int i = 0; i < count; ++i) {
        auto detail = M5.Touch.getDetail(i);
        if (detail.wasClicked()) {
            *x = detail.x;
            *y = detail.y;
            return true;
        }
    }
    return false;
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

static uint32_t durationFromTouch(int16_t x, int16_t y) {
    static constexpr int CIRCLE_R = 49;
    static constexpr int OPTION_X[] = {142, 233, 324};
    static constexpr int OPTION_Y[] = {248, 184, 248};
    static constexpr uint32_t OPTIONS[] = {10, 20, 30};

    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); ++i) {
        if (pointInCircle(x, y, OPTION_X[i], OPTION_Y[i], CIRCLE_R)) {
            return OPTIONS[i];
        }
    }
    return 0;
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
    Serial.printf("[btn] %s -> arming mic (recording_duration=%lus)\n",
                  source,
                  (unsigned long)g_recordSeconds);
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawArming();
    audio::startRecording(g_recordSeconds);
    audio::pumpRecording();
    Serial.printf("[rec] ready cap_s=%lu max_wav_bytes=%u\n",
                  (unsigned long)audio::maxDurationSeconds(),
                  (unsigned)audio::MAX_WAV_BYTES);
    g_mode = Mode::RECORDING;

    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
    ui::drawRec(0, g_recordSeconds);
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
        M5.Power.setVibration(120);
        delay(80);
        M5.Power.setVibration(0);
    }
    const uint8_t* wav = nullptr;
    size_t sz = 0;
    audio::stopRecording(&wav, &sz);
    if (wav && sz > 44) {
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
    Serial.printf("[rec:wav] slots=%u samples=%u duration_ms=%u min=%d max=%d peak=%u rms=%u zc=%u\n",
                  (unsigned)stats.slots,
                  (unsigned)stats.samples,
                  (unsigned)stats.durationMs,
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
            M5.Power.setVibration(120);
            delay(80);
            M5.Power.setVibration(0);
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
    M5.begin(cfg);
    Serial.printf("[boot] M5 ok, heap=%u psram=%u\n",
                  ESP.getFreeHeap(), ESP.getPsramSize());

    ui::init();
    ui::clearToBlack();

    audio::init();
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
    Serial.printf("[boot] bridge %s (http=%d)\n",
                  g_bridgeUp ? "ok" : "down", net::lastStatus());

    // First real poll: show a quick greeting with last_msg if there is one.
    if (g_bridgeUp) {
        PetState s;
        if (net::fetchPetState(s)) {
            g_state = s;
            syncClock(g_state.ts);
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            if (g_state.last_msg[0]) {
                ui::drawGreeting(g_state.last_msg);
                g_greetingUntilMs = millis() + GREETING_HOLD_MS;
            } else {
                ui::drawMood(g_state);
            }
        }
    }
}

// --- loop ------------------------------------------------------------------
void loop() {
    M5.update();
    handleSerialCommands();

    if (handleSettingsChord()) {
        delay(20);
        return;
    }

    // --- WiFi watchdog ------------------------------------------------------
    if (!g_wifiUp || WiFi.status() != WL_CONNECTED) {
        g_wifiUp = false;
        if (millis() - g_lastRecon > 10000) {
            g_lastRecon = millis();
            Serial.println("[wifi] reconnecting...");
            WiFi.reconnect();
        }
    }

    // --- state machine ------------------------------------------------------
    switch (g_mode) {

    case Mode::IDLE: {
        // Poll /pet/state every POLL_INTERVAL_MS
        if (g_wifiUp && (millis() - g_lastPoll) > POLL_INTERVAL_MS) {
            g_lastPoll = millis();
            PetState s;
            if (net::fetchPetState(s)) {
                g_state    = s;
                g_bridgeUp = true;
                syncClock(g_state.ts);
                Serial.printf("[poll] mood=%s food=%ld age=%ld ts=%ld\n",
                              s.mood, (long)s.food_today, (long)s.age_s, (long)s.ts);
                ui::clearToBlack();
                ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
                if (g_bridgeUp) {
                    ui::drawMood(g_state);
                } else {
                    ui::drawOffline("retrying");
                }
            } else {
                g_bridgeUp = false;
                Serial.printf("[poll] bridge down (http=%d)\n", net::lastStatus());
                ui::clearToBlack();
                ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
                ui::drawOffline("retrying");
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
        if (g_bridgeUp && pet_sprite::tickBlink()) {
            pet_sprite::drawCentered(pet_sprite::moodIndex(g_state.mood),
                                     pet_sprite::currentFrame(),
                                     2);
        }

        // Hide the greeting overlay once the timer expires
        if (g_greetingUntilMs && millis() > g_greetingUntilMs) {
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
                    M5.Power.setVibration(140);
                    delay(120);
                    M5.Power.setVibration(0);
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
                M5.Power.setVibration(140);
                delay(120);
                M5.Power.setVibration(0);
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
            uint32_t seconds = durationFromTouch(touchX, touchY);
            if (seconds) {
                setRecordSeconds(seconds, "touch");
                chirpOk();
                M5.Power.setVibration(90);
                delay(70);
                M5.Power.setVibration(0);
                showDurationSaved("touch");
                break;
            }
            g_settingsAtMs = millis();
        }

        if (btnBClicked()) {
            uint32_t next = g_recordSeconds == 10 ? 20 : (g_recordSeconds == 20 ? 30 : 10);
            setRecordSeconds(next, "B cycle");
            g_settingsAtMs = millis();
            drawSettings();
            break;
        }

        if (btnAClicked()) {
            returnToPet("A click from settings");
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
            returnToPet("settings saved");
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
            ui::drawRec(elapsedS, g_recordSeconds);
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
    delay(20);
}
