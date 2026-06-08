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

static uint16_t moodColor(const char* mood) {
    if (!mood) return FG;
    if (strcmp(mood, "happy")  == 0) return MOOD_HAPPY;
    if (strcmp(mood, "hungry") == 0) return MOOD_HUNGRY;
    if (strcmp(mood, "sleepy") == 0) return MOOD_SLEEPY;
    if (strcmp(mood, "sick")   == 0) return MOOD_SICK;
    return FG;
}

// Build a circular sprite and use it as a clip mask so subsequent draws
// to the main canvas are auto-clipped to the disc.
static void applyDiscClip() {
    static LGFX_Sprite* mask = nullptr;
    if (!mask) {
        mask = new LGFX_Sprite(&M5.Display);
        mask->setColorDepth(8);
        mask->createSprite(SCREEN_W, SCREEN_H);
        mask->fillSprite(0);          // transparent everywhere
        mask->fillCircle(SCREEN_CX, SCREEN_CY, SCREEN_R, 1);  // opaque disc
    }
    M5.Display.setClipRect(0, 0, SCREEN_W, SCREEN_H);
    // LGFX doesn't have a built-in mask API; we draw a black-filled
    // background inside the disc every frame (clearToBlack does that).
}

void init() {
    applyDiscClip();
    M5.Display.setTextDatum(TL_DATUM);
}

void clearToBlack() {
    M5.Display.fillScreen(BG);
    // Re-paint the disc area only (already black) so we don't see chassis pixels.
    M5.Display.fillCircle(SCREEN_CX, SCREEN_CY, SCREEN_R, BG);
}

static void centeredText(int y, int size, uint16_t color, const char* s) {
    M5.Display.setTextSize(size);
    M5.Display.setTextColor(color, BG);
    int w = M5.Display.textWidth(s);
    int h = M5.Display.fontHeight();
    M5.Display.setCursor(SCREEN_CX - w / 2, y);
    M5.Display.print(s);
    (void)h;
}

void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(wifiUp ? FG : DIM, BG);

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
    int w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, 20);
    M5.Display.print(line);
}

void drawMood(const PetState& s) {
    // The sprite face itself. Centered, with a thin mood-color border ring
    // for extra vibe.
    pet_sprite::drawCentered(pet_sprite::moodIndex(s.mood),
                             pet_sprite::currentFrame());

    // Mood label as small text just above the sprite
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(moodColor(s.mood), BG);
    const char* lbl = s.mood;
    int lw = M5.Display.textWidth(lbl);
    M5.Display.setCursor(SCREEN_CX - lw / 2, SCREEN_CY - PET_SPRITE_H / 2 - 24);
    M5.Display.print(lbl);

    // Last message as a single subtitle line, truncated with ellipsis.
    if (s.last_msg[0]) {
        char sub[40];
        snprintf(sub, sizeof(sub), "\"%s\"", s.last_msg);
        if ((int)M5.Display.textWidth(sub) > 360) {
            int n = (int)strlen(sub);
            while (n > 6 && (int)M5.Display.textWidth(sub) > 360) {
                sub[--n] = '\0';
                sub[n - 1] = sub[n - 2] = sub[n - 3] = '.';
            }
        }
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(DIM, BG);
        int w = M5.Display.textWidth(sub);
        M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_CY + PET_SPRITE_H / 2 + 12);
        M5.Display.print(sub);
    }
}

void drawOffline(const char* reason) {
    centeredText(SCREEN_CY - 20, 4, DIM, "bridge ?");
    if (reason) {
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(DIM, BG);
        int w = M5.Display.textWidth(reason);
        M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_CY + 20);
        M5.Display.print(reason);
    }
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
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(FG, BG);
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
        M5.Display.setCursor(SCREEN_CX - maxCharsPerLine() * 12 / 2, y);
        M5.Display.print(line);
        pos = end;
        y += lineH;
    }

    // Page indicator at the bottom
    if (s_pageCount > 1) {
        char hint[16];
        snprintf(hint, sizeof(hint), "%d/%d", s_pageIndex + 1, s_pageCount);
        int w = M5.Display.textWidth(hint);
        M5.Display.setTextColor(DIM, BG);
        M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_H - 24);
        M5.Display.print(hint);
    }
}

// ---------------------------------------------------------------- recording --
void drawRec(uint32_t elapsedS) {
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(0xF800, BG);  // red
    const char* label = "REC";
    int w = M5.Display.textWidth(label);
    M5.Display.setCursor(SCREEN_CX - w / 2, 30);
    M5.Display.print(label);

    char t[16];
    snprintf(t, sizeof(t), "%lus", (unsigned long)elapsedS);
    M5.Display.setTextSize(2);
    w = M5.Display.textWidth(t);
    M5.Display.setCursor(SCREEN_CX - w / 2, 80);
    M5.Display.print(t);

    // "bar" that pulses — width based on millis
    int wBar = 100 + (millis() / 8) % 200;
    M5.Display.fillRoundRect(SCREEN_CX - wBar / 2, 360, wBar, 8, 4, 0xF800);
}

// ---------------------------------------------------------------- thinking ---
void drawThinking() {
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(DIM, BG);
    const char* label = "...";
    int w = M5.Display.textWidth(label);
    M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_CY);
    M5.Display.print(label);
}

// ---------------------------------------------------------------- stats ------
void drawStats(const PetState& s, int rssi, const char* proxyUrl) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(FG, BG);

    char line[40];
    int y = 30;
    int lineH = 28;

    // Header
    M5.Display.setTextColor(moodColor(s.mood), BG);
    snprintf(line, sizeof(line), "stats: %s", s.mood);
    int w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;
    M5.Display.setTextColor(FG, BG);

    snprintf(line, sizeof(line), "today:  %ldk", (long)(s.food_today / 1000));
    w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;

    snprintf(line, sizeof(line), "total:  %ldk", (long)(s.total_tokens_ever / 1000));
    w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;

    snprintf(line, sizeof(line), "chats:  %d", s.audio_runs_today);
    w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;

    snprintf(line, sizeof(line), "rssi:   %d dBm", rssi);
    w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;

    // Truncate URL visually by skipping the scheme
    const char* host = strstr(proxyUrl, "://");
    host = host ? host + 3 : proxyUrl;
    snprintf(line, sizeof(line), "bridge: %s", host);
    w = M5.Display.textWidth(line);
    M5.Display.setCursor(SCREEN_CX - w / 2, y); M5.Display.print(line);
    y += lineH;

    drawHintLine("hold B to reset  |  A: home");
}

void drawHintLine(const char* s) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(DIM, BG);
    int w = M5.Display.textWidth(s);
    M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_H - 18);
    M5.Display.print(s);
}

void drawGreeting(const char* lastMsg) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(DIM, BG);
    M5.Display.setCursor(SCREEN_CX - M5.Display.textWidth("last heard:") / 2, 60);
    M5.Display.print("last heard:");

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(FG, BG);
    int y = 100;
    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\"", lastMsg);
    int w = M5.Display.textWidth(buf);
    if (w > 360) {
        // crude truncation with ellipsis
        int n = (int)strlen(buf);
        while (n > 6 && M5.Display.textWidth(buf) > 360) {
            buf[--n] = '\0';
            buf[n - 1] = buf[n - 2] = buf[n - 3] = '.';
        }
        w = M5.Display.textWidth(buf);
    }
    M5.Display.setCursor(SCREEN_CX - w / 2, y);
    M5.Display.print(buf);
}

void drawConfirmReset() {
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(0xFD20, BG);   // amber
    const char* q = "reset pet?";
    int w = M5.Display.textWidth(q);
    M5.Display.setCursor(SCREEN_CX - w / 2, SCREEN_CY - 30);
    M5.Display.print(q);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0x07E0, BG);   // green
    const char* yes = "A: yes";
    w = M5.Display.textWidth(yes);
    M5.Display.setCursor(SCREEN_CX - 60, SCREEN_CY + 20);
    M5.Display.print(yes);

    M5.Display.setTextColor(0xF800, BG);   // red
    const char* no = "B: no";
    w = M5.Display.textWidth(no);
    M5.Display.setCursor(SCREEN_CX + 20, SCREEN_CY + 20);
    M5.Display.print(no);
}

}  // namespace ui
