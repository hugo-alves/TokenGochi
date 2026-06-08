#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"
#include "pet_state.h"
#include "net.h"
#include "ui.h"
#include "pet_sprite.h"
#include "audio.h"

#include <ArduinoJson.h>

// --- globals ---------------------------------------------------------------
static PetState     g_state;
static bool         g_wifiUp     = false;
static bool         g_bridgeUp   = false;
static uint32_t     g_lastPoll   = 0;
static uint32_t     g_lastRecon  = 0;
static uint32_t     g_lastRecRedraw = 0;

enum class Mode : uint8_t { IDLE, RECORDING, TRANSCRIBING, SHOWING, STATS, CONFIRM, ERROR };
static Mode         g_mode = Mode::IDLE;
static char         g_lastTranscript[512];
static char         g_lastError[64];
static uint32_t     g_errorAtMs = 0;
static uint32_t     g_statsAtMs = 0;       // when stats view opened
static uint32_t     g_greetingUntilMs = 0; // hide greeting after this
static bool         g_greetOnFirstPoll = true;

static constexpr uint32_t RECORD_HOLD_MS = 600;  // hold A this long to start
static constexpr uint32_t ERROR_HOLD_MS  = 3000; // show error then return
static constexpr uint32_t STATS_TIMEOUT_MS = 8000; // auto-dismiss stats view
static constexpr uint32_t CONFIRM_TIMEOUT_MS = 5000;
static constexpr uint32_t GREETING_HOLD_MS = 3000;
static constexpr uint32_t HTTP_TIMEOUT_MS_LONG = 15000; // Groq can be slow

// --- helpers ---------------------------------------------------------------
static void drawBootScreen(const char* line2, uint16_t color) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setCursor(20, 20);
    M5.Display.print("token tamagotchi");
    M5.Display.setCursor(20, 50);
    M5.Display.printf("ssid: %s", WIFI_SSID);
    M5.Display.setCursor(20, 80);
    M5.Display.setTextColor(color, 0x0000);
    M5.Display.print(line2);
}

static void connectWifi() {
    // Surface the actual reason WiFi fails by listening to events.
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
        switch (e) {
            case ARDUINO_EVENT_WIFI_STA_START:       Serial.println("[wifi] STA start"); break;
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:  Serial.printf("[wifi] connected to AP (channel %d)\n", info.wifi_sta_connected.channel); break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:     Serial.printf("[wifi] got ip: %s\n", WiFi.localIP().toString().c_str()); break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                Serial.printf("[wifi] DISCONNECTED reason=%d (SSID='%s')\n",
                              info.wifi_sta_disconnected.reason,
                              info.wifi_sta_disconnected.ssid);
                break;
            }
            default: break;
        }
    });
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

    for (int i = 0; i < 40; i++) {
        if (WiFi.status() == WL_CONNECTED) { g_wifiUp = true; return; }
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        delay(500);
        Serial.printf("[wifi] attempt %d: status=%d\n", i + 1, (int)WiFi.status());
    }
    g_wifiUp = false;
}

static void chirpOk()   { audio::chirp(1200, 80); }
static void chirpFail() { audio::chirp(400, 200); }

// Pull the "text" field out of a JSON response like `{"text": "...", ...}`.
static void extractTranscript(const char* json, char* out, size_t outSize) {
    out[0] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    const char* t = doc["text"] | "";
    strncpy(out, t, outSize - 1);
    out[outSize - 1] = '\0';
}

// POST a recorded clip and update the FSM.
static void transcribeAndShow(const uint8_t* wav, size_t size) {
    g_mode = Mode::TRANSCRIBING;
    ui::clearToBlack();
    ui::drawStatus(g_state, g_wifiUp, true);
    ui::drawThinking();

    char body[1024];
    size_t bodyLen = 0;
    int code = net::postTranscribe(wav, size, body, sizeof(body), &bodyLen);

    if (code == 200) {
        char text[512];
        extractTranscript(body, text, sizeof(text));
        if (text[0]) {
            strncpy(g_lastTranscript, text, sizeof(g_lastTranscript) - 1);
            g_lastTranscript[sizeof(g_lastTranscript) - 1] = '\0';
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

        // Hold A to start recording
        if (g_wifiUp && g_bridgeUp && M5.BtnA.pressedFor(RECORD_HOLD_MS)) {
            Serial.println("[btn] A hold -> recording");
            audio::startRecording();
            g_mode = Mode::RECORDING;
            chirpOk();
        }

        // KEYB short -> stats view
        if (g_bridgeUp && M5.BtnB.wasClicked()) {
            Serial.println("[btn] B click -> stats");
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
        }

        // Animate the blink on the pet
        if (g_bridgeUp && pet_sprite::tickBlink()) {
            pet_sprite::drawCentered(pet_sprite::moodIndex(g_state.mood),
                                     pet_sprite::currentFrame());
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

    case Mode::STATS: {
        // KEYB short -> confirm overlay (reset prompt)
        if (M5.BtnB.wasClicked()) {
            Serial.println("[btn] B click -> confirm");
            g_mode = Mode::CONFIRM;
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            ui::drawConfirmReset();
            break;
        }
        // A press -> back to home
        if (M5.BtnA.wasClicked()) {
            Serial.println("[btn] A click -> home");
            g_mode = Mode::IDLE;
            g_lastPoll = 0;
            break;
        }
        // Auto-dismiss
        if (millis() - g_statsAtMs > STATS_TIMEOUT_MS) {
            g_mode = Mode::IDLE;
            g_lastPoll = 0;
        }
        break;
    }

    case Mode::CONFIRM: {
        if (M5.BtnA.wasClicked()) {
            Serial.println("[btn] A click -> reset!");
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(0x07E0, 0x0000);
            const char* t = "resetting...";
            int w = M5.Display.textWidth(t);
            M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_CY);
            M5.Display.print(t);

            PetState fresh;
            int code = net::postReset(fresh);
            if (code == 200) {
                g_state = fresh;
                g_mode = Mode::IDLE;
                g_lastPoll = 0;
                chirpOk();
                M5.Power.setVibration(140);
                delay(120);
                M5.Power.setVibration(0);
            } else {
                snprintf(g_lastError, sizeof(g_lastError), "HTTP %d", code);
                g_mode = Mode::ERROR;
                g_errorAtMs = millis();
                chirpFail();
            }
            break;
        }
        if (M5.BtnB.wasClicked()) {
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
            ui::clearToBlack();
            ui::drawStats(g_state, WiFi.RSSI(), PROXY_URL);
            break;
        }
        if (millis() - g_statsAtMs > CONFIRM_TIMEOUT_MS) {
            g_mode = Mode::STATS;
            g_statsAtMs = millis();
        }
        break;
    }

    case Mode::RECORDING: {
        audio::pumpRecording();

        // Redraw the REC overlay ~5x/sec
        if (millis() - g_lastRecRedraw > 200) {
            g_lastRecRedraw = millis();
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            ui::drawRec(audio::elapsedSeconds());
            // Also show the face so it doesn't disappear
            pet_sprite::drawCentered(pet_sprite::moodIndex(g_state.mood), 0);
        }

        // Release -> stop and transcribe
        if (M5.BtnA.wasReleased()) {
            Serial.println("[btn] A release -> transcribe");
            const uint8_t* wav = nullptr;
            size_t sz = 0;
            audio::stopRecording(&wav, &sz);
            if (wav && sz > 44) {
                transcribeAndShow(wav, sz);
            } else {
                Serial.println("[rec] nothing captured");
                chirpFail();
                g_mode = Mode::IDLE;
            }
        }

        // Force-stop after 10s
        if (audio::elapsedSeconds() >= 10) {
            Serial.println("[rec] force-stop at 10s");
            M5.Power.setVibration(120);
            delay(80);
            M5.Power.setVibration(0);
            const uint8_t* wav = nullptr;
            size_t sz = 0;
            audio::stopRecording(&wav, &sz);
            if (wav && sz > 44) {
                transcribeAndShow(wav, sz);
            } else {
                g_mode = Mode::IDLE;
            }
        }
        break;
    }

    case Mode::TRANSCRIBING:
        // The thinking overlay is drawn in transcribeAndShow; we just wait
        // for the HTTP call to return (it blocks within transcribeAndShow,
        // so this state is transient).
        break;

    case Mode::SHOWING: {
        // Redraw the transcript on first entry; user pages with A click,
        // dismisses with B.
        static bool s_drawn = false;
        if (!s_drawn) {
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            ui::drawTranscript(g_lastTranscript);
            s_drawn = true;
        }
        if (M5.BtnA.wasClicked()) {
            s_drawn = false;  // force redraw on next loop
            ui::clearToBlack();
            ui::drawStatus(g_state, g_wifiUp, g_bridgeUp);
            if (ui::pageNext()) {
                ui::drawTranscript(g_lastTranscript);
            } else {
                // pageNext wrapped back; just redraw the first page
                ui::drawTranscript(g_lastTranscript);
            }
        }
        if (M5.BtnB.wasClicked()) {
            s_drawn = false;
            ui::pageReset();
            g_mode = Mode::IDLE;
            // Force a fresh poll on the next iteration
            g_lastPoll = 0;
        }
        // Also allow a fresh long-press of A to start a new recording
        if (M5.BtnA.pressedFor(RECORD_HOLD_MS)) {
            s_drawn = false;
            ui::pageReset();
            audio::startRecording();
            g_mode = Mode::RECORDING;
            chirpOk();
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
        if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked()) {
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
