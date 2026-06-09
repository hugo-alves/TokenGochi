#include "ui.h"
#include "pet_sprite.h"
#include "sprites.h"
#include <M5Unified.h>
#include <math.h>

namespace ui {

static constexpr uint16_t BG        = 0x0000;  // black
static constexpr uint16_t FG        = 0xFFFF;  // white
static constexpr uint16_t DIM       = 0x7BEF;  // grey
static constexpr uint16_t MOOD_HAPPY  = 0x07E0;  // green
static constexpr uint16_t MOOD_EXCITED = 0x87F0;  // bright cyan-green
static constexpr uint16_t MOOD_HUNGRY = 0xFD20;  // amber
static constexpr uint16_t MOOD_PECKISH = 0xFFE0;  // yellow
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
    if (strcmp(mood, "very happy") == 0) return MOOD_EXCITED;
    if (strcmp(mood, "excited") == 0) return MOOD_EXCITED;
    if (strcmp(mood, "happy")  == 0) return MOOD_HAPPY;
    if (strcmp(mood, "peckish") == 0) return MOOD_PECKISH;
    if (strcmp(mood, "very hungry") == 0) return MOOD_HUNGRY;
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

static bool hasCodexAccountUsage(const PetState& s) {
    return s.codex_usage_percent_x10 >= 0;
}

static void formatUsagePercent(char* out, size_t outSize, const PetState& s) {
    int x10 = s.codex_usage_percent_x10;
    if (x10 < 0) x10 = 0;
    int whole = x10 / 10;
    int frac = x10 % 10;
    if (frac == 0) {
        snprintf(out, outSize, "%d%%", whole);
    } else {
        snprintf(out, outSize, "%d.%d%%", whole, frac);
    }
}

void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp) {
    target().setTextSize(2);
    target().setTextColor(wifiUp ? FG : DIM, BG);

    char line[64];
    if (!wifiUp) {
        snprintf(line, sizeof(line), "wifi: ?");
    } else if (!bridgeUp) {
        snprintf(line, sizeof(line), "api: ?");
    } else if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(line, sizeof(line), "%s use  %dd %dh",
                 pct,
                 (int)(s.age_s / 86400),
                 (int)((s.age_s % 86400) / 3600));
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
    centeredText(SCREEN_CY - 20, 4, DIM, "api ?");
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
static char         s_ownedText[TRANSCRIPT_LOG_TEXT_BYTES];   // copy of the current transcript
static char         s_pageTitle[32];
static char         s_pageFooter[32];
static size_t       s_pageStarts[12];
static int          s_pageIndex = 0;
static int          s_pageCount = 0;

static constexpr int TRANSCRIPT_LINE_H = 24;
static constexpr int TRANSCRIPT_SIDE_PAD = 20;
static constexpr int TRANSCRIPT_BOTTOM_Y = SCREEN_H - 46;

static int transcriptTopY() {
    return s_pageTitle[0] ? 66 : 54;
}

static int chordWidthAtY(int y) {
    const float sampleY = (float)y + (float)TRANSCRIPT_LINE_H * 0.5f;
    const float dy = sampleY - (float)SCREEN_CY;
    const float r = (float)SCREEN_R;
    float half = 0.0f;
    if (fabsf(dy) < r) {
        half = sqrtf(r * r - dy * dy);
    }
    int width = (int)(half * 2.0f) - TRANSCRIPT_SIDE_PAD * 2;
    if (width < 132) width = 132;
    if (width > SCREEN_W - TRANSCRIPT_SIDE_PAD * 2) {
        width = SCREEN_W - TRANSCRIPT_SIDE_PAD * 2;
    }
    return width;
}

static int maxCharsForY(int y) {
    target().setTextSize(2);
    int charW = target().textWidth("M");
    if (charW <= 0) charW = 12;
    int chars = chordWidthAtY(y) / charW;
    if (chars < 10) chars = 10;
    if (chars > 60) chars = 60;
    return chars;
}

static size_t skipLeadingSpaces(size_t pos) {
    while (pos < s_pageLen && (s_ownedText[pos] == ' ' || s_ownedText[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

static size_t wrappedLineEnd(size_t pos, int maxChars) {
    pos = skipLeadingSpaces(pos);
    if (pos >= s_pageLen) return pos;

    size_t end = pos;
    size_t lastBreak = 0;
    int chars = 0;
    while (end < s_pageLen && chars < maxChars) {
        const char c = s_ownedText[end];
        if (c == '\n' || c == '\r') return end + 1;
        ++end;
        ++chars;
        if (c == ' ' || c == '-' || c == ',' || c == '.' || c == ';' || c == ':') {
            lastBreak = end;
        }
    }
    if (end < s_pageLen &&
        s_ownedText[end] != ' ' &&
        s_ownedText[end] != '\n' &&
        s_ownedText[end] != '\r' &&
        lastBreak > pos) {
        return lastBreak;
    }
    return end > pos ? end : pos + 1;
}

static void copyLine(size_t start, size_t end, char* out, size_t outSize) {
    start = skipLeadingSpaces(start);
    while (end > start &&
           (s_ownedText[end - 1] == ' ' ||
            s_ownedText[end - 1] == '\t' ||
            s_ownedText[end - 1] == '\n' ||
            s_ownedText[end - 1] == '\r')) {
        --end;
    }

    size_t n = end > start ? end - start : 0;
    if (n >= outSize) n = outSize - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = s_ownedText[start + i];
        out[i] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    out[n] = '\0';
}

static void layoutTranscriptPages() {
    s_pageCount = 0;
    s_pageStart = 0;
    if (s_pageLen == 0) return;

    size_t pos = 0;
    while (pos < s_pageLen && s_pageCount < (int)(sizeof(s_pageStarts) / sizeof(s_pageStarts[0]))) {
        s_pageStarts[s_pageCount++] = pos;
        int y = transcriptTopY();
        while (pos < s_pageLen && y <= TRANSCRIPT_BOTTOM_Y) {
            size_t next = wrappedLineEnd(pos, maxCharsForY(y));
            if (next <= pos) next = pos + 1;
            pos = next;
            y += TRANSCRIPT_LINE_H;
        }
    }

    if (s_pageCount == 0) {
        s_pageStarts[0] = 0;
        s_pageCount = 1;
    }
}

void pageReset() {
    s_pageIndex = 0;
    s_pageStart = 0;
    s_pageLen = 0;
    s_pageText = nullptr;
    s_showingTranscript = false;
    s_ownedText[0] = '\0';
    s_pageTitle[0] = '\0';
    s_pageFooter[0] = '\0';
    s_pageCount = 0;
}

static void renderTranscriptPage() {
    if (!s_showingTranscript || !s_pageText) return;

    target().setTextSize(2);
    target().setTextColor(FG, BG);

    if (s_pageTitle[0]) {
        target().setTextSize(1);
        target().setTextColor(DIM, BG);
        int w = target().textWidth(s_pageTitle);
        target().setCursor(SCREEN_CX - w / 2, 46);
        target().print(s_pageTitle);
        target().setTextSize(2);
        target().setTextColor(FG, BG);
    }

    const size_t pageEnd = (s_pageIndex + 1 < s_pageCount)
        ? s_pageStarts[s_pageIndex + 1]
        : s_pageLen;
    size_t pos = s_pageStarts[s_pageIndex];
    int y = transcriptTopY();
    while (pos < pageEnd && y <= TRANSCRIPT_BOTTOM_Y) {
        size_t end = wrappedLineEnd(pos, maxCharsForY(y));
        if (end > pageEnd) end = pageEnd;
        char line[80] = {0};
        copyLine(pos, end, line, sizeof(line));
        if (line[0]) {
            const int w = target().textWidth(line);
            target().setCursor(SCREEN_CX - w / 2, y);
            target().print(line);
        }
        pos = end > pos ? end : pos + 1;
        y += TRANSCRIPT_LINE_H;
    }

    target().setTextSize(1);
    target().setTextColor(DIM, BG);
    char hint[40];
    if (s_pageFooter[0]) {
        snprintf(hint, sizeof(hint), "%s", s_pageFooter);
    } else if (s_pageCount > 1) {
        snprintf(hint, sizeof(hint), "%d/%d  A/tap  B", s_pageIndex + 1, s_pageCount);
    } else {
        snprintf(hint, sizeof(hint), "B: done");
    }
    int w = target().textWidth(hint);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_H - 24);
    target().print(hint);
    flush();
}

bool pageNext() {
    if (!s_showingTranscript) return false;
    s_pageIndex++;
    if (s_pageIndex >= s_pageCount) {
        // wrap back to the start
        s_pageIndex = 0;
    }
    s_pageStart = s_pageStarts[s_pageIndex];
    return true;
}

bool showingTranscript() { return s_showingTranscript; }

void drawTranscript(const char* text, const char* title, const char* footer) {
    if (!text || !*text) {
        pageReset();
        return;
    }

    const char* nextTitle = title ? title : "";
    const char* nextFooter = footer ? footer : "";
    if (s_showingTranscript &&
        strncmp(s_ownedText, text, sizeof(s_ownedText)) == 0 &&
        strncmp(s_pageTitle, nextTitle, sizeof(s_pageTitle)) == 0 &&
        strncmp(s_pageFooter, nextFooter, sizeof(s_pageFooter)) == 0) {
        renderTranscriptPage();
        return;
    }

    pageReset();
    strncpy(s_ownedText, text, sizeof(s_ownedText) - 1);
    s_ownedText[sizeof(s_ownedText) - 1] = '\0';
    strncpy(s_pageTitle, nextTitle, sizeof(s_pageTitle) - 1);
    s_pageTitle[sizeof(s_pageTitle) - 1] = '\0';
    strncpy(s_pageFooter, nextFooter, sizeof(s_pageFooter) - 1);
    s_pageFooter[sizeof(s_pageFooter) - 1] = '\0';
    s_pageText = s_ownedText;
    s_pageLen = strlen(s_ownedText);
    layoutTranscriptPages();
    s_pageIndex = 0;
    s_pageStart = s_pageStarts[0];
    s_showingTranscript = true;

    renderTranscriptPage();
}

void drawHistoryList(size_t count, size_t selectedIndex, const char* meta, const char* preview) {
    target().setTextSize(3);
    target().setTextColor(0x87F0, BG);
    const char* title = "HISTORY";
    int w = target().textWidth(title);
    target().setCursor(SCREEN_CX - w / 2, 52);
    target().print(title);

    if (count == 0) {
        target().setTextSize(2);
        target().setTextColor(DIM, BG);
        const char* empty = "no transcripts";
        w = target().textWidth(empty);
        target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 12);
        target().print(empty);
        drawHintLine("A: pet");
        flush();
        return;
    }

    char pos[24];
    snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(selectedIndex + 1), (unsigned)count);
    target().setTextSize(2);
    target().setTextColor(FG, BG);
    w = target().textWidth(pos);
    target().setCursor(SCREEN_CX - w / 2, 104);
    target().print(pos);

    if (meta && *meta) {
        target().setTextSize(1);
        target().setTextColor(DIM, BG);
        w = target().textWidth(meta);
        target().setCursor(SCREEN_CX - w / 2, 132);
        target().print(meta);
    }

    target().setTextSize(2);
    target().setTextColor(FG, BG);
    const char* text = preview ? preview : "";
    size_t len = strlen(text);
    size_t posText = 0;
    int y = 174;
    while (posText < len && y <= 320) {
        const int maxChars = maxCharsForY(y);
        size_t end = posText;
        size_t lastSpace = 0;
        int chars = 0;
        while (end < len && chars < maxChars) {
            char c = text[end];
            if (c == '\n' || c == '\r') break;
            ++end;
            ++chars;
            if (c == ' ') lastSpace = end;
        }
        if (end < len && text[end] != ' ' && lastSpace > posText) {
            end = lastSpace;
        }
        while (posText < end && text[posText] == ' ') ++posText;
        char line[80] = {0};
        size_t n = end > posText ? end - posText : 0;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + posText, n);
        line[n] = '\0';
        w = target().textWidth(line);
        target().setCursor(SCREEN_CX - w / 2, y);
        target().print(line);
        posText = end > posText ? end : posText + 1;
        y += TRANSCRIPT_LINE_H;
    }

    drawHintLine("A open  |  B older");
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

void drawVoiceReady() {
    target().setTextSize(3);
    target().setTextColor(0x87F0, BG);
    const char* label = "VOICE";
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 66);
    target().print(label);

    target().setTextSize(2);
    target().setTextColor(FG, BG);
    const char* ready = "ready";
    w = target().textWidth(ready);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 20);
    target().print(ready);

    target().setTextSize(1);
    target().setTextColor(DIM, BG);
    const char* hint = "B: record  |  A: pet";
    w = target().textWidth(hint);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY + 34);
    target().print(hint);
    flush();
}

void drawDurationSettings(uint32_t selectedSeconds) {
    target().setTextSize(3);
    target().setTextColor(0x87F0, BG);
    const char* title = "VOICE";
    int w = target().textWidth(title);
    target().setCursor(SCREEN_CX - w / 2, 46);
    target().print(title);

    target().setTextSize(2);
    target().setTextColor(FG, BG);
    const char* subtitle = "recording length";
    w = target().textWidth(subtitle);
    target().setCursor(SCREEN_CX - w / 2, 90);
    target().print(subtitle);

    static constexpr uint32_t OPTIONS[] = {10, 20, 30};
    static constexpr int BOX_W = 220;
    static constexpr int BOX_H = 54;
    static constexpr int BOX_X = SCREEN_CX - BOX_W / 2;
    static constexpr int BOX_Y[] = {142, 214, 286};

    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); ++i) {
        const bool selected = selectedSeconds == OPTIONS[i];
        const uint16_t border = selected ? 0x07E0 : DIM;
        const uint16_t fill = selected ? 0x0340 : BG;
        target().fillRoundRect(BOX_X, BOX_Y[i], BOX_W, BOX_H, 8, fill);
        target().drawRoundRect(BOX_X, BOX_Y[i], BOX_W, BOX_H, 8, border);

        char label[16];
        snprintf(label, sizeof(label), "%lu sec", (unsigned long)OPTIONS[i]);
        target().setTextSize(2);
        target().setTextColor(selected ? 0x07E0 : FG, fill);
        w = target().textWidth(label);
        target().setCursor(SCREEN_CX - w / 2, BOX_Y[i] + 17);
        target().print(label);
    }

    drawHintLine("tap option  |  B cycle  |  A pet");
    flush();
}

void drawDurationSaved(uint32_t selectedSeconds) {
    target().setTextSize(2);
    target().setTextColor(DIM, BG);
    const char* saved = "recording length";
    int w = target().textWidth(saved);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 70);
    target().print(saved);

    target().setTextSize(4);
    target().setTextColor(0x07E0, BG);
    char label[16];
    snprintf(label, sizeof(label), "%lus", (unsigned long)selectedSeconds);
    w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 22);
    target().print(label);

    target().setTextSize(2);
    target().setTextColor(FG, BG);
    const char* ok = "saved";
    w = target().textWidth(ok);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY + 34);
    target().print(ok);
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

    target().setTextSize(1);
    target().setTextColor(DIM, BG);
    const char* hint = "B: send  |  A: cancel";
    w = target().textWidth(hint);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_H - 18);
    target().print(hint);
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

    if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(line, sizeof(line), "usage:  %s", pct);
    } else {
        snprintf(line, sizeof(line), "today:  %ldk", (long)(s.food_today / 1000));
    }
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    if (hasCodexAccountUsage(s) && s.codex_plan[0]) {
        snprintf(line, sizeof(line), "plan:   %s", s.codex_plan);
    } else {
        snprintf(line, sizeof(line), "total:  %ldk", (long)(s.total_tokens_ever / 1000));
    }
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
    snprintf(line, sizeof(line), "api:    %s", host);
    w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y); target().print(line);
    y += lineH;

    drawHintLine("B: reset?  |  A: home");
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
