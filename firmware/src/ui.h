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

// Compact round-safe status label. The home screen renders the richer radial
// usage/state treatment in drawMood().
void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp);

// Round watch-face home: usage ring, large pet, and short state labels.
void drawMood(const PetState& s);

// "?" / sad-face fallback when the bridge is unreachable.
void drawOffline(const char* reason = nullptr);

// Recording overlay with a circular countdown around the mic.
void drawRec(uint32_t elapsedS, uint32_t totalS);

// Shown briefly after KEYA press while the mic/codec is being armed.
void drawArming();

// Voice input idle screen. Recording has not started yet.
void drawVoiceReady();

// Recording duration settings. Options are intentionally centered inside the
// round display so no touch target depends on invisible corners.
void drawDurationSettings(uint32_t selectedSeconds);

// Brief confirmation after a touch selection saves the duration.
void drawDurationSaved(uint32_t selectedSeconds);

// "thinking" overlay while the bridge is calling Groq.
void drawThinking();

// Pager: render the transcript text centered, word-wrapped, on the disc.
// `text` is NUL-terminated. Pages advance with pageNext().
void drawTranscript(const char* text, const char* title = nullptr, const char* footer = nullptr);

// Device-only transcript history list. `selectedIndex` is zero-based newest-first.
void drawHistoryList(size_t count, size_t selectedIndex, const char* meta, const char* preview);

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
