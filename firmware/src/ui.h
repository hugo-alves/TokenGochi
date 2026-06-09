#pragma once
// Round-AMOLED helpers. All drawing is clipped to the inscribed circle so
// nothing leaks into the chassis corners. Step 5 is text-only; step 6
// adds sprite faces.

#include "pet_state.h"
#include "config.h"
#include <Arduino.h>
#include <M5Unified.h>

namespace ui {

void init();
lgfx::LGFXBase& target();
void flush();
void clearToBlack();
bool writeScreenshot(Stream& out);

// Top status line: WiFi state + age + tokens.
void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp);

// Big mood text centered in the disc.
void drawMood(const PetState& s);

// "?" / sad-face fallback when the bridge is unreachable.
void drawOffline(const char* reason = nullptr);

// Recording overlay — big "REC" text, elapsed seconds, and a moving bar.
// Pass elapsedS = seconds since recording started.
void drawRec(uint32_t elapsedS);

// Shown briefly after KEYA press while the mic/codec is being armed.
void drawArming();

// "thinking" overlay while the bridge is calling Groq.
void drawThinking();

// Pager: render the transcript text centered, word-wrapped, on the disc.
// `text` is NUL-terminated. Pages advance with pageNext().
void drawTranscript(const char* text);

// Page the transcript forward by one screenful. Returns true if paged.
bool pageNext();

// Reset the pager cursor back to the top of the current transcript.
void pageReset();

// True if the pager is currently showing a transcript.
bool showingTranscript();

// Stats overlay: food/total/age/audio runs/RSSI/proxy URL on a single disc.
void drawStats(const PetState& s, int rssi, const char* proxyUrl);

// Greeting overlay: shows `last_msg` for 2 s after boot (if non-empty).
void drawGreeting(const char* lastMsg);

// Confirm overlay: "reset pet?  A: yes  B: no"
void drawConfirmReset();

// Short single-line status at the bottom of the disc, used by the
// stats view. Caller passes the full string.
void drawHintLine(const char* s);

}  // namespace ui
