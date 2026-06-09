#include "ui.h"
#include "pet_sprite.h"
#include "sprites.h"
#include <M5Unified.h>

namespace ui {

static constexpr uint16_t BG        = 0x0000;  // black
static constexpr uint16_t FG        = 0xFFFF;  // white
static constexpr uint16_t DIM       = 0x7BEF;  // grey
static constexpr uint16_t MOOD_HAPPY  = 0x07E0;  // green
static constexpr uint16_t MOOD_HUNGRY = 0xFD20;  // amber
static constexpr uint16_t MOOD_SLEEPY = 0x001F;  // blue
static constexpr uint16_t MOOD_SICK   = 0xF800;  // red

static M5Canvas g_canvas(&M5.Display);
static bool g_canvasReady = false;

lgfx::LGFXBase& target() {
    return g_canvasReady ? static_cast<lgfx::LGFXBase&>(g_canvas)
                         : static_cast<lgfx::LGFXBase&>(M5.Display);
}

void flush() {
    if (g_canvasReady) {
        g_canvas.pushSprite(0, 0);
    }
}

static uint16_t moodColor(const char* mood) {
    if (!mood) return FG;
    if (strcmp(mood, "happy")  == 0) return MOOD_HAPPY;
    if (strcmp(mood, "hungry") == 0) return MOOD_HUNGRY;
    if (strcmp(mood, "sleepy") == 0) return MOOD_SLEEPY;
    if (strcmp(mood, "sick")   == 0) return MOOD_SICK;
    return FG;
}

static void applyDiscClip() {
    target().setClipRect(0, 0, SCREEN_W, SCREEN_H);
}

void init() {
    g_canvas.setColorDepth(16);
    g_canvasReady = g_canvas.createSprite(SCREEN_W, SCREEN_H) != nullptr;
    if (!g_canvasReady) {
        Serial.println("[screen] PSRAM mirror allocation failed; TGSHOT unavailable");
    }
    applyDiscClip();
    target().setClipRect(0, 0, SCREEN_W, SCREEN_H);
    target().setTextDatum(TL_DATUM);
}

bool writeScreenshot(Stream& out) {
    if (!g_canvasReady || g_canvas.getBuffer() == nullptr) {
        out.print("TGSHOT ERR no-canvas\n");
        out.flush();
        return false;
    }

    const size_t byteCount = (size_t)SCREEN_W * (size_t)SCREEN_H * 2;
    out.printf("TGSHOT BEGIN %d %d RGB565BE %u\n",
               SCREEN_W, SCREEN_H, (unsigned)byteCount);
    out.flush();

    const uint8_t* data = static_cast<const uint8_t*>(g_canvas.getBuffer());
    size_t sent = 0;
    while (sent < byteCount) {
        const size_t chunk = min((size_t)1024, byteCount - sent);
        out.write(data + sent, chunk);
        sent += chunk;
        out.flush();
        delay(1);
    }

    out.print("\nTGSHOT END\n");
    out.flush();
    return true;
}

void clearToBlack() {
    target().fillScreen(BG);
    // Re-paint the disc area only (already black) so we don't see chassis pixels.
    target().fillCircle(SCREEN_CX, SCREEN_CY, SCREEN_R, BG);
    flush();
}

static void centeredText(int y, int size, uint16_t color, const char* s) {
    target().setTextSize(size);
    target().setTextColor(color, BG);
    int w = target().textWidth(s);
    int h = target().fontHeight();
    target().setCursor(SCREEN_CX - w / 2, y);
    target().print(s);
    (void)h;
    flush();
}

void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp) {
    target().setTextSize(2);
    target().setTextColor(wifiUp ? FG : DIM, BG);

    char line[64];
    if (!wifiUp) {
        snprintf(line, sizeof(line), "wifi: ?");
    } else if (!bridgeUp) {
        snprintf(line, sizeof(line), "bridge: ?");
    } else {
        snprintf(line, sizeof(line), "%ldk tk  %dd %dh",
                 (long)(s.food_today / 1000),
                 (int)(s.age_s / 86400),
                 (int)((s.age_s % 86400) / 3600));
    }
    int w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, 20);
    target().print(line);
    flush();
}

void drawMood(const PetState& s) {
    // The sprite face itself. Centered, with a thin mood-color border ring
    // for extra vibe.
    pet_sprite::drawCentered(pet_sprite::moodIndex(s.mood),
                             pet_sprite::currentFrame());

    // Mood label as small text just above the sprite
    target().setTextSize(2);
    target().setTextColor(moodColor(s.mood), BG);
    const char* lbl = s.mood;
    int lw = target().textWidth(lbl);
    target().setCursor(SCREEN_CX - lw / 2, SCREEN_CY - PET_SPRITE_H / 2 - 24);
    target().print(lbl);

    // Last message as a single subtitle line, truncated with ellipsis.
    if (s.last_msg[0]) {
        char sub[40];
        snprintf(sub, sizeof(sub), "\"%s\"", s.last_msg);
        if ((int)target().textWidth(sub) > 360) {
            int n = (int)strlen(sub);
            while (n > 6 && (int)target().textWidth(sub) > 360) {
                sub[--n] = '\0';
                sub[n - 1] = sub[n - 2] = sub[n - 3] = '.';
            }
        }
        target().setTextSize(2);
        target().setTextColor(DIM, BG);
        int w = target().textWidth(sub);
        target().setCursor(SCREEN_CX - w / 2, SCREEN_CY + PET_SPRITE_H / 2 + 12);
        target().print(sub);
    }
    flush();
}

void drawOffline(const char* reason) {
    centeredText(SCREEN_CY - 20, 4, DIM, "bridge ?");
    if (reason) {
        target().setTextSize(2);
        target().setTextColor(DIM, BG);
        int w = target().textWidth(reason);
        target().setCursor(SCREEN_CX - w / 2, SCREEN_CY + 20);
        target().print(reason);
    }
    flush();
}

// ---------------------------------------------------------------- pager ------
static const char*  s_pageText = nullptr;
static size_t       s_pageStart = 0;
static size_t       s_pageLen = 0;
static bool         s_showingTranscript = false;
static char         s_ownedText[512];   // copy of the current transcript
static int          s_pageIndex = 0;
static int          s_pageCount = 0;

// Compute line widths for the current font/size and word-wrap.
static int maxCharsPerLine() { return 18; }   // empirical for size 2
static int maxLinesOnScreen() { return 10; }  // empirical
static int charsPerPage() { return maxCharsPerLine() * maxLinesOnScreen(); }

void pageReset() {
    s_pageIndex = 0;
    s_pageStart = 0;
    s_pageLen = 0;
    s_pageText = nullptr;
    s_showingTranscript = false;
    s_ownedText[0] = '\0';
    s_pageCount = 0;
}

bool pageNext() {
    if (!s_showingTranscript) return false;
    s_pageIndex++;
    if (s_pageIndex >= s_pageCount) {
        // wrap back to the start
        s_pageIndex = 0;
    }
    s_pageStart = s_pageIndex * charsPerPage();
    return true;
}

bool showingTranscript() { return s_showingTranscript; }

void drawTranscript(const char* text) {
    pageReset();
    if (!text || !*text) return;

    strncpy(s_ownedText, text, sizeof(s_ownedText) - 1);
    s_ownedText[sizeof(s_ownedText) - 1] = '\0';
    s_pageText = s_ownedText;
    s_pageLen = strlen(s_ownedText);
    s_pageCount = (s_pageLen + charsPerPage() - 1) / charsPerPage();
    s_pageIndex = 0;
    s_pageStart = 0;
    s_showingTranscript = true;

    // Render this page
    target().setTextSize(2);
    target().setTextColor(FG, BG);
    const int lineH = 22;  // approx line height for size 2
    const int topY  = 36;

    int cx = SCREEN_CX, cy = SCREEN_CY;
    // Word-wrap: for each line, take up to maxCharsPerLine chars ending at
    // a space, otherwise break mid-word.
    size_t pos = s_pageStart;
    int y = topY;
    while (pos < s_pageLen && y < SCREEN_H - 20) {
        size_t end = pos;
        int charsThisLine = 0;
        size_t lastSpace = pos;
        while (end < s_pageLen && charsThisLine < maxCharsPerLine()) {
            char c = s_ownedText[end];
            end++;
            charsThisLine++;
            if (c == ' ') lastSpace = end;
        }
        if (end < s_pageLen && s_ownedText[end] != ' ' && lastSpace > pos) {
            end = lastSpace;
        }
        char line[40] = {0};
        size_t n = end - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, s_ownedText + pos, n);
        line[n] = '\0';
        // crude centering-ish: just left-aligned
        target().setCursor(SCREEN_CX - maxCharsPerLine() * 12 / 2, y);
        target().print(line);
        pos = end;
        y += lineH;
    }

    // Page indicator at the bottom
    if (s_pageCount > 1) {
        char hint[16];
        snprintf(hint, sizeof(hint), "%d/%d", s_pageIndex + 1, s_pageCount);
        int w = target().textWidth(hint);
        target().setTextColor(DIM, BG);
        target().setCursor(SCREEN_CX - w / 2, SCREEN_H - 24);
        target().print(hint);
    }
    flush();
}

// ---------------------------------------------------------------- recording --
void drawArming() {
    target().setTextSize(2);
    target().setTextColor(DIM, BG);
    const char* label = "getting ready";
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 12);
    target().print(label);
    flush();
}

void drawRec(uint32_t elapsedS) {
    target().setTextSize(3);
    target().setTextColor(0xF800, BG);  // red
    const char* label = "REC";
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, 30);
    target().print(label);

    char t[16];
    snprintf(t, sizeof(t), "%lus", (unsigned long)elapsedS);
    target().setTextSize(2);
    w = target().textWidth(t);
    target().setCursor(SCREEN_CX - w / 2, 80);
    target().print(t);

    // "bar" that pulses — width based on millis
    int wBar = 100 + (millis() / 8) % 200;
    target().fillRoundRect(SCREEN_CX - wBar / 2, 360, wBar, 8, 4, 0xF800);
    flush();
}

// ---------------------------------------------------------------- thinking ---
void drawThinking() {
    target().setTextSize(2);
    target().setTextColor(DIM, BG);
    const char* label = "sending...";
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 12);
    target().print(label);
    flush();
}

// ---------------------------------------------------------------- stats ------
void drawStats(const PetState& s, int rssi, const char* proxyUrl) {
    target().setTextSize(2);
    target().setTextColor(FG, BG);

    char line[40];
    int y = 30;
    int lineH = 28;

    // Header
    target().setTextColor(moodColor(s.mood), BG);
    snprintf(line, sizeof(line), "stats: %s", s.mood);
    int w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;
    target().setTextColor(FG, BG);

    snprintf(line, sizeof(line), "today:  %ldk", (long)(s.food_today / 1000));
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    snprintf(line, sizeof(line), "total:  %ldk", (long)(s.total_tokens_ever / 1000));
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    snprintf(line, sizeof(line), "chats:  %d", s.audio_runs_today);
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    snprintf(line, sizeof(line), "rssi:   %d dBm", rssi);
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    // Truncate URL visually by skipping the scheme
    const char* host = strstr(proxyUrl, "://");
    host = host ? host + 3 : proxyUrl;
    snprintf(line, sizeof(line), "bridge: %s", host);
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    drawHintLine("hold B to reset  |  A: home");
    flush();
}

void drawHintLine(const char* s) {
    target().setTextSize(1);
    target().setTextColor(DIM, BG);
    int w = target().textWidth(s);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_H - 18);
    target().print(s);
    flush();
}

void drawGreeting(const char* lastMsg) {
    target().setTextSize(2);
    target().setTextColor(DIM, BG);
    target().setCursor(SCREEN_CX - target().textWidth("last heard:") / 2, 60);
    target().print("last heard:");

    target().setTextSize(2);
    target().setTextColor(FG, BG);
    int y = 100;
    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\"", lastMsg);
    int w = target().textWidth(buf);
    if (w > 360) {
        // crude truncation with ellipsis
        int n = (int)strlen(buf);
        while (n > 6 && target().textWidth(buf) > 360) {
            buf[--n] = '\0';
            buf[n - 1] = buf[n - 2] = buf[n - 3] = '.';
        }
        w = target().textWidth(buf);
    }
    target().setCursor(SCREEN_CX - w / 2, y);
    target().print(buf);
    flush();
}

void drawConfirmReset() {
    target().setTextSize(3);
    target().setTextColor(0xFD20, BG);   // amber
    const char* q = "reset pet?";
    int w = target().textWidth(q);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 30);
    target().print(q);

    target().setTextSize(2);
    target().setTextColor(0x07E0, BG);   // green
    const char* yes = "A: yes";
    w = target().textWidth(yes);
    target().setCursor(SCREEN_CX - 60, SCREEN_CY + 20);
    target().print(yes);

    target().setTextColor(0xF800, BG);   // red
    const char* no = "B: no";
    w = target().textWidth(no);
    target().setCursor(SCREEN_CX + 20, SCREEN_CY + 20);
    target().print(no);
    flush();
}

}  // namespace ui
