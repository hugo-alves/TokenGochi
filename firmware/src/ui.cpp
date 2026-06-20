#include "ui.h"
#include "pet_sprite.h"
#include "sprites.h"
#include "battery_status.h"
#include "device_settings.h"
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
static constexpr uint16_t RING_DIM    = 0x3186;  // dark grey
static constexpr uint16_t CHIP_BG     = 0x0841;  // near-black blue/grey

static constexpr uint8_t HOME_PET_SCALE = 2;
static constexpr int HOME_RING_R = 218;
static constexpr int HOME_RING_THICKNESS = 7;
static constexpr int VOICE_BUTTON_R = 92;
static constexpr float DEG_TO_RAD_F = 0.01745329252f;

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
    if (strcmp(mood, "grumpy") == 0) return MOOD_SICK;
    if (strcmp(mood, "very grumpy") == 0) return MOOD_SICK;
    if (strcmp(mood, "very_grumpy") == 0) return MOOD_SICK;
    if (strcmp(mood, "sick")   == 0) return MOOD_SICK;
    return FG;
}

static void applyDiscClip() {
    target().setClipRect(0, 0, SCREEN_W, SCREEN_H);
}

static int chordWidthAtYRaw(int y) {
    const float dy = (float)y - (float)SCREEN_CY;
    const float r = (float)SCREEN_R;
    if (fabsf(dy) >= r) return 0;
    return (int)(sqrtf(r * r - dy * dy) * 2.0f);
}

static int safeWidthAtY(int y, int pad) {
    int width = chordWidthAtYRaw(y) - pad * 2;
    if (width < 0) width = 0;
    if (width > SCREEN_W - pad * 2) width = SCREEN_W - pad * 2;
    return width;
}

static void truncateToWidth(char* text, size_t textSize, int maxWidth) {
    if (!text || textSize == 0) return;
    if (maxWidth <= 0) {
        text[0] = '\0';
        return;
    }
    if ((int)target().textWidth(text) <= maxWidth) return;

    size_t len = strlen(text);
    while (len > 4 && (int)target().textWidth(text) > maxWidth) {
        text[--len] = '\0';
        if (len > 3) {
            text[len - 1] = '.';
            text[len - 2] = '.';
            text[len - 3] = '.';
        }
    }
}

static void drawCenteredSafeText(int y,
                                 int size,
                                 uint16_t color,
                                 const char* text,
                                 int pad = 24,
                                 uint16_t bg = BG) {
    if (!text) return;
    char line[80];
    strncpy(line, text, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    target().setTextSize(size);
    target().setTextColor(color, bg);
    truncateToWidth(line, sizeof(line), safeWidthAtY(y + target().fontHeight() / 2, pad));
    const int w = target().textWidth(line);
    target().setCursor(SCREEN_CX - w / 2, y);
    target().print(line);
}

static void drawCenteredBoxText(int cx,
                                int y,
                                int size,
                                uint16_t color,
                                const char* text,
                                int maxWidth,
                                uint16_t bg = BG) {
    if (!text) return;
    char line[80];
    strncpy(line, text, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    target().setTextSize(size);
    target().setTextColor(color, bg);
    truncateToWidth(line, sizeof(line), maxWidth);
    const int w = target().textWidth(line);
    target().setCursor(cx - w / 2, y);
    target().print(line);
}

static void drawArcDots(int radius,
                        int startDeg,
                        int endDeg,
                        uint16_t color,
                        int thickness,
                        int stepDeg = 2) {
    if (stepDeg < 1) stepDeg = 1;
    if (endDeg < startDeg) {
        int tmp = startDeg;
        startDeg = endDeg;
        endDeg = tmp;
    }
    const int dotR = max(1, thickness / 2);
    for (int deg = startDeg; deg <= endDeg; deg += stepDeg) {
        const float rad = (float)deg * DEG_TO_RAD_F;
        const int x = SCREEN_CX + (int)roundf(cosf(rad) * radius);
        const int y = SCREEN_CY + (int)roundf(sinf(rad) * radius);
        target().fillCircle(x, y, dotR, color);
    }
}

static void drawCircularProgress(int radius,
                                 int progressX1000,
                                 uint16_t color,
                                 int thickness,
                                 bool background = true) {
    if (progressX1000 < 0) progressX1000 = 0;
    if (progressX1000 > 1000) progressX1000 = 1000;
    if (background) {
        drawArcDots(radius, -90, 270, RING_DIM, thickness, 4);
    }
    const int endDeg = -90 + (360 * progressX1000) / 1000;
    drawArcDots(radius, -90, endDeg, color, thickness, 2);
}

static void drawCircularMarker(int radius, int percentX10, uint16_t color, int markerR) {
    if (percentX10 < 0) return;
    if (percentX10 > 1000) percentX10 = 1000;
    const int deg = -90 + (360 * percentX10) / 1000;
    const float rad = (float)deg * DEG_TO_RAD_F;
    const int x = SCREEN_CX + (int)roundf(cosf(rad) * radius);
    const int y = SCREEN_CY + (int)roundf(sinf(rad) * radius);
    target().fillCircle(x, y, markerR, BG);
    target().drawCircle(x, y, markerR, color);
    target().fillCircle(x, y, max(1, markerR - 3), color);
}

static void drawCircularChip(int cx, int cy, int r, const char* label, uint16_t accent) {
    target().fillCircle(cx, cy, r, CHIP_BG);
    target().drawCircle(cx, cy, r, accent);
    target().drawCircle(cx, cy, r - 1, RING_DIM);

    target().setTextSize(1);
    target().setTextColor(FG, CHIP_BG);
    char line[24];
    strncpy(line, label ? label : "", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    truncateToWidth(line, sizeof(line), r * 2 - 10);
    const int w = target().textWidth(line);
    target().setCursor(cx - w / 2, cy - target().fontHeight() / 2);
    target().print(line);
}

static void drawMicIcon(int cx, int cy, uint16_t color, uint16_t fill) {
    target().fillCircle(cx, cy, VOICE_BUTTON_R, fill);
    target().drawCircle(cx, cy, VOICE_BUTTON_R, color);
    target().drawCircle(cx, cy, VOICE_BUTTON_R - 5, RING_DIM);

    target().fillRoundRect(cx - 18, cy - 42, 36, 58, 18, color);
    target().fillRoundRect(cx - 10, cy - 34, 20, 42, 10, BG);
    target().drawLine(cx - 34, cy - 10, cx - 34, cy + 10, color);
    target().drawLine(cx + 34, cy - 10, cx + 34, cy + 10, color);
    target().drawLine(cx - 34, cy + 10, cx - 22, cy + 28, color);
    target().drawLine(cx + 34, cy + 10, cx + 22, cy + 28, color);
    target().drawLine(cx - 22, cy + 28, cx + 22, cy + 28, color);
    target().drawLine(cx, cy + 42, cx, cy + 62, color);
    target().drawLine(cx - 24, cy + 62, cx + 24, cy + 62, color);
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
    drawCenteredSafeText(y, size, color, s);
    flush();
}

static bool hasCodexAccountUsage(const PetState& s) {
    return s.codex_usage_percent_x10 >= 0;
}

static bool hasCodexPace(const PetState& s) {
    return s.codex_pace_label[0] ||
           s.codex_pace_kind[0] ||
           s.codex_expected_percent_x10 >= 0 ||
           s.codex_pace_delta_x10 != INT16_MIN;
}

static bool hasActivity(const PetState& s) {
    return s.activity_idle_seconds >= 0 ||
           (s.activity_stage[0] && strcmp(s.activity_stage, "unknown") != 0);
}

static bool activityIsGrumpy(const PetState& s) {
    return strcmp(s.activity_stage, "grumpy") == 0 ||
           strcmp(s.activity_stage, "very_grumpy") == 0 ||
           strcmp(s.activity_stage, "very grumpy") == 0;
}

static uint16_t activityColor(const PetState& s) {
    if (strcmp(s.activity_stage, "awake") == 0) return MOOD_HAPPY;
    if (strcmp(s.activity_stage, "restless") == 0) return MOOD_PECKISH;
    if (activityIsGrumpy(s)) return MOOD_SICK;
    return DIM;
}

static int activityProgressX1000(const PetState& s) {
    if (strcmp(s.activity_stage, "awake") == 0) return 250;
    if (strcmp(s.activity_stage, "restless") == 0) return 500;
    if (strcmp(s.activity_stage, "grumpy") == 0) return 750;
    if (strcmp(s.activity_stage, "very_grumpy") == 0 ||
        strcmp(s.activity_stage, "very grumpy") == 0) {
        return 1000;
    }
    return -1;
}

static void formatPercentX10(char* out, size_t outSize, int x10) {
    if (x10 < 0) x10 = 0;
    if (x10 > 1000) x10 = 1000;
    int whole = x10 / 10;
    int frac = x10 % 10;
    if (frac == 0) {
        snprintf(out, outSize, "%d%%", whole);
    } else {
        snprintf(out, outSize, "%d.%d%%", whole, frac);
    }
}

static void formatUsagePercent(char* out, size_t outSize, const PetState& s) {
    formatPercentX10(out, outSize, s.codex_usage_percent_x10);
}

static void formatPaceLabel(char* out, size_t outSize, const PetState& s) {
    if (s.codex_pace_label[0]) {
        snprintf(out, outSize, "%s", s.codex_pace_label);
        return;
    }
    if (strcmp(s.codex_pace_kind, "on_pace") == 0) {
        snprintf(out, outSize, "on pace");
        return;
    }
    if (s.codex_pace_kind[0]) {
        int pct = s.codex_balance_percent_x10 >= 0
            ? (s.codex_balance_percent_x10 + 5) / 10
            : 0;
        snprintf(out, outSize, "%d%% %s", pct, s.codex_pace_kind);
        return;
    }
    snprintf(out, outSize, "weekly pace");
}

static void formatHomeFoodLabel(char* out, size_t outSize, const PetState& s) {
    if (hasCodexPace(s)) {
        formatPaceLabel(out, outSize, s);
        return;
    }
    if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(out, outSize, "%s weekly", pct);
        return;
    }
    snprintf(out, outSize, "%ldk today", (long)(s.food_today / 1000));
}

static void formatActivityStage(char* out, size_t outSize, const PetState& s) {
    if (strcmp(s.activity_stage, "very_grumpy") == 0) {
        snprintf(out, outSize, "very grumpy");
    } else if (s.activity_stage[0]) {
        snprintf(out, outSize, "%s", s.activity_stage);
    } else {
        snprintf(out, outSize, "unknown");
    }
}

static void formatIdleLabel(char* out, size_t outSize, const PetState& s) {
    if (s.activity_idle_seconds < 0) {
        snprintf(out, outSize, "idle ?");
        return;
    }
    int32_t sec = s.activity_idle_seconds;
    if (sec < 60) {
        snprintf(out, outSize, "active now");
    } else if (sec < 3600) {
        snprintf(out, outSize, "idle %ldm", (long)(sec / 60));
    } else {
        long h = sec / 3600;
        long m = (sec % 3600) / 60;
        snprintf(out, outSize, "idle %ldh%02ldm", h, m);
    }
}

static void drawActivityArc(const PetState& s) {
    drawArcDots(166, 45, 135, RING_DIM, 5, 6);
    const int progress = activityProgressX1000(s);
    if (progress < 0) return;
    const int endDeg = 45 + (90 * progress) / 1000;
    drawArcDots(166, 45, endDeg, activityColor(s), 5, 3);
}

static void drawHomeFoodHeader(const PetState& s, uint16_t accent) {
    target().fillRect(58, 12, 350, 112, BG);

    char food[32];
    formatHomeFoodLabel(food, sizeof(food), s);
    drawCenteredSafeText(54, 3, FG, food, 54);

    target().setTextSize(1);
    target().setTextColor(DIM, BG);
    int dotW = target().textWidth("...");
    target().setCursor(SCREEN_CX - 92, 100);
    target().print("...");
    target().setCursor(SCREEN_CX + 92 - dotW, 100);
    target().print("...");

    drawCenteredSafeText(92, 3, accent, "FOOD", 68);
}

static void drawAwakePanel(const PetState& s) {
    const uint16_t awake = activityColor(s);
    const int x = 120;
    const int y = 326;
    const int w = 226;
    const int h = 104;
    const int r = 34;

    target().fillRoundRect(x, y, w, h, r, BG);
    target().drawRoundRect(x, y, w, h, r, RING_DIM);
    target().drawRoundRect(x + 5, y + 5, w - 10, h - 10, r - 5, 0x2104);

    if (activityProgressX1000(s) >= 0) {
        target().drawLine(x + 22, y + 30, x + 66, y + 30, activityIsGrumpy(s) ? RING_DIM : awake);
        target().drawLine(x + w - 66, y + 30, x + w - 22, y + 30, awake);
        target().drawLine(x + 78, y + h - 14, x + 112, y + h - 14, RING_DIM);
        target().drawLine(x + w - 112, y + h - 14, x + w - 78, y + h - 14, awake);
    }

    char idle[24];
    char stage[18];
    formatIdleLabel(idle, sizeof(idle), s);
    formatActivityStage(stage, sizeof(stage), s);

    drawCenteredBoxText(SCREEN_CX, y + 20, 2, FG, "AWAKE", w - 48);
    drawCenteredBoxText(SCREEN_CX, y + 48, 3, FG, idle, w - 42);
    drawCenteredBoxText(SCREEN_CX, y + 82, 2, awake, stage, w - 48);
}

void drawStatus(const PetState& s, bool wifiUp, bool bridgeUp) {
    char line[64];
    bool showFoodLabel = false;
    if (!wifiUp) {
        snprintf(line, sizeof(line), "wifi ?");
    } else if (!bridgeUp) {
        snprintf(line, sizeof(line), "api ?");
    } else if (hasCodexPace(s)) {
        if (strcmp(s.codex_pace_kind, "reserve") == 0 ||
            strcmp(s.codex_pace_kind, "deficit") == 0) {
            const int pct = s.codex_balance_percent_x10 >= 0
                ? (s.codex_balance_percent_x10 + 5) / 10
                : 0;
            snprintf(line, sizeof(line), "%d%% %s",
                     pct,
                     strcmp(s.codex_pace_kind, "reserve") == 0 ? "rsv" : "def");
        } else {
            formatPaceLabel(line, sizeof(line), s);
        }
        showFoodLabel = true;
    } else if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(line, sizeof(line), "%s weekly", pct);
        showFoodLabel = true;
    } else {
        snprintf(line, sizeof(line), "%ldk tk", (long)(s.food_today / 1000));
        showFoodLabel = true;
    }
    if (showFoodLabel) {
        drawCenteredSafeText(18, 1, DIM, "FOOD", 66);
        drawCenteredSafeText(32, 2, wifiUp && bridgeUp ? FG : DIM, line, 66);
    } else {
        drawCenteredSafeText(28, 2, wifiUp && bridgeUp ? FG : DIM, line, 56);
    }
    flush();
}

void drawMood(const PetState& s) {
    const uint16_t accent = moodColor(s.mood);
    int progressX1000 = -1;
    if (hasCodexAccountUsage(s)) {
        progressX1000 = s.codex_usage_percent_x10;
        if (progressX1000 < 0) progressX1000 = 0;
        if (progressX1000 > 1000) progressX1000 = 1000;
    }

    if (progressX1000 >= 0) {
        drawCircularProgress(HOME_RING_R, progressX1000, accent, HOME_RING_THICKNESS);
        drawCircularMarker(HOME_RING_R, s.codex_expected_percent_x10, FG, 7);
    } else {
        drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 12);
        drawArcDots(HOME_RING_R, -90, 30, accent, HOME_RING_THICKNESS, 6);
    }

    drawHomeFoodHeader(s, accent);

    // The pet face is the primary object on the watch face.
    pet_sprite::drawCentered(pet_sprite::moodIndex(s.mood),
                             pet_sprite::currentFrame(),
                             HOME_PET_SCALE);

    if (hasActivity(s)) {
        drawAwakePanel(s);
    } else if (s.last_msg[0]) {
        char sub[40];
        snprintf(sub, sizeof(sub), "\"%s\"", s.last_msg);
        drawCenteredSafeText(374, 2, DIM, sub, 76);
    }
    flush();
}

void drawOffline(const char* reason) {
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 12);
    drawArcDots(HOME_RING_R, -70, -20, MOOD_SICK, HOME_RING_THICKNESS, 4);
    drawArcDots(HOME_RING_R, 110, 160, MOOD_SICK, HOME_RING_THICKNESS, 4);
    centeredText(SCREEN_CY - 34, 4, DIM, "api ?");
    if (reason) {
        drawCenteredSafeText(SCREEN_CY + 20, 2, DIM, reason, 70);
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

    if (s_pageCount > 1) {
        drawCircularProgress(HOME_RING_R,
                             ((s_pageIndex + 1) * 1000) / s_pageCount,
                             0x87F0,
                             4);
    }

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
    const int topY = transcriptTopY();
    size_t lineStarts[12];
    size_t lineEnds[12];
    int lineCount = 0;
    int layoutY = topY;
    while (pos < pageEnd &&
           layoutY <= TRANSCRIPT_BOTTOM_Y &&
           lineCount < (int)(sizeof(lineStarts) / sizeof(lineStarts[0]))) {
        size_t end = wrappedLineEnd(pos, maxCharsForY(layoutY));
        if (end > pageEnd) end = pageEnd;
        lineStarts[lineCount] = pos;
        lineEnds[lineCount] = end;
        lineCount++;
        pos = end > pos ? end : pos + 1;
        layoutY += TRANSCRIPT_LINE_H;
    }

    const int availableH = TRANSCRIPT_BOTTOM_Y - topY + TRANSCRIPT_LINE_H;
    const int textH = lineCount * TRANSCRIPT_LINE_H;
    int y = topY;
    if (textH > 0 && textH < availableH) {
        y = topY + (availableH - textH) / 2;
    }

    for (int i = 0; i < lineCount; ++i) {
        char line[80] = {0};
        copyLine(lineStarts[i], lineEnds[i], line, sizeof(line));
        if (line[0]) {
            const int w = target().textWidth(line);
            target().setCursor(SCREEN_CX - w / 2, y);
            target().print(line);
        }
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
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 12);
    drawCenteredSafeText(52, 3, 0x87F0, "HISTORY", 78);

    if (count == 0) {
        drawCenteredSafeText(SCREEN_CY - 12, 2, DIM, "no transcripts", 76);
        drawHintLine("A: pet");
        flush();
        return;
    }

    char pos[24];
    snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(selectedIndex + 1), (unsigned)count);
    drawCenteredSafeText(100, 2, FG, pos, 80);

    if (meta && *meta) {
        drawCenteredSafeText(130, 1, DIM, meta, 80);
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
        int w = target().textWidth(line);
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
    drawArcDots(HOME_RING_R, -60, 240, RING_DIM, HOME_RING_THICKNESS, 8);
    drawCenteredSafeText(54, 3, 0x87F0, "VOICE", 80);
    drawMicIcon(SCREEN_CX, SCREEN_CY + 18, 0x87F0, 0x0208);
    drawCenteredSafeText(SCREEN_CY + 132, 1, DIM, "tap mic or B", 64);
    drawHintLine("A: pet");
    flush();
}

static void drawSettingsChrome(const char* title, const char* subtitle = nullptr) {
    drawArcDots(214, -150, -30, RING_DIM, 4, 5);
    drawArcDots(214, 210, 330, RING_DIM, 4, 5);
    drawCenteredSafeText(38, 3, 0x87F0, title, 58);
    if (subtitle && *subtitle) {
        drawCenteredSafeText(80, 1, DIM, subtitle, 54);
    }
}

static void drawRoundButton(int cx,
                            int cy,
                            int r,
                            const char* label,
                            uint16_t border,
                            uint16_t fill,
                            uint16_t text = FG,
                            int textSize = 2) {
    target().fillCircle(cx, cy, r, fill);
    target().drawCircle(cx, cy, r, border);
    target().drawCircle(cx, cy, r - 5, border == DIM ? RING_DIM : border);
    drawCenteredBoxText(cx, cy - (textSize == 1 ? 8 : 12), textSize, text, label, r * 2 - 18, fill);
}

static void drawMenuOption(int cx,
                           int cy,
                           const char* label,
                           const char* value,
                           bool selected,
                           uint16_t accent) {
    const int r = 54;
    const uint16_t fill = selected ? 0x0340 : BG;
    const uint16_t border = selected ? accent : RING_DIM;
    target().fillCircle(cx, cy, r, fill);
    target().drawCircle(cx, cy, r, border);
    target().drawCircle(cx, cy, r - 5, selected ? border : DIM);
    drawCenteredBoxText(cx, cy - 22, 2, selected ? 0x07E0 : FG, label, r * 2 - 16, fill);
    drawCenteredBoxText(cx, cy + 12, 1, selected ? FG : DIM, value, r * 2 - 18, fill);
}

static void drawLinearGauge(int y, uint8_t percent, uint16_t accent) {
    percent = device_settings::clampPercent(percent);
    const int x = SCREEN_CX - 120;
    const int w = 240;
    const int h = 18;
    const int filled = (w * percent) / 100;
    target().fillRoundRect(x, y, w, h, h / 2, CHIP_BG);
    if (filled > 0) {
        target().fillRoundRect(x, y, filled, h, h / 2, accent);
    }
    target().drawRoundRect(x, y, w, h, h / 2, DIM);
}

static void drawChoicePill(int cx, int cy, const char* label, bool active) {
    const int w = 70;
    const int h = 38;
    const int x = cx - w / 2;
    const int y = cy - h / 2;
    const uint16_t fill = active ? 0x0340 : BG;
    const uint16_t border = active ? 0x07E0 : RING_DIM;
    target().fillRoundRect(x, y, w, h, 12, fill);
    target().drawRoundRect(x, y, w, h, 12, border);
    drawCenteredBoxText(cx, y + 11, 1, active ? 0x07E0 : FG, label, w - 12, fill);
}

static void drawInfoPill(int cx, int cy, const char* label, const char* value, uint16_t accent) {
    const int w = 132;
    const int h = 56;
    const int x = cx - w / 2;
    const int y = cy - h / 2;
    target().fillRoundRect(x, y, w, h, 16, BG);
    target().drawRoundRect(x, y, w, h, 16, RING_DIM);
    drawCenteredBoxText(cx, y + 9, 1, DIM, label, w - 16, BG);
    drawCenteredBoxText(cx, y + 29, 1, accent, value, w - 16, BG);
}

static void formatBatteryPercent(char* out, size_t outSize, const battery_status::Snapshot& battery) {
    if (battery.percentKnown && battery.percent >= 0) {
        snprintf(out, outSize, "%d%%", (int)battery.percent);
    } else {
        snprintf(out, outSize, "--%%");
    }
}

static const char* feedbackValue(const device_settings::Settings& settings) {
    if (settings.buttonSound && settings.vibration) return "both";
    if (settings.buttonSound) return "sound";
    if (settings.vibration) return "vibe";
    return "off";
}

void drawDurationSettings(uint32_t selectedSeconds, bool autoMode) {
    drawSettingsChrome("VOICE", "auto trims silence");

    static constexpr uint32_t OPTIONS[] = {0, 10, 20, 30};
    static constexpr int OPTION_X[] = {233, 112, 233, 354};
    static constexpr int OPTION_Y[] = {176, 272, 272, 272};

    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); ++i) {
        const bool selected = OPTIONS[i] == 0 ? autoMode : (!autoMode && selectedSeconds == OPTIONS[i]);
        const uint16_t fill = selected ? 0x0340 : BG;
        const uint16_t border = selected ? 0x07E0 : RING_DIM;
        target().fillCircle(OPTION_X[i], OPTION_Y[i], 56, fill);
        target().drawCircle(OPTION_X[i], OPTION_Y[i], 56, border);
        target().drawCircle(OPTION_X[i], OPTION_Y[i], 50, selected ? 0x07E0 : DIM);

        char label[16];
        if (OPTIONS[i] == 0) {
            snprintf(label, sizeof(label), "AUTO");
        } else {
            snprintf(label, sizeof(label), "%lus", (unsigned long)OPTIONS[i]);
        }
        drawCenteredBoxText(OPTION_X[i], OPTION_Y[i] - 17, OPTIONS[i] == 0 ? 2 : 3,
                            selected ? 0x07E0 : FG, label, 88, fill);
    }

    drawCenteredSafeText(340, 1, DIM, "auto stops after your voice", 54);
    drawHintLine("tap option | B next");
    flush();
}

void drawDurationSaved(uint32_t selectedSeconds) {
    drawSettingsChrome("SAVED", "voice duration");

    target().setTextSize(5);
    target().setTextColor(0x07E0, BG);
    char label[16];
    snprintf(label, sizeof(label), "%lus", (unsigned long)selectedSeconds);
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, SCREEN_CY - 28);
    target().print(label);

    drawCenteredSafeText(SCREEN_CY + 58, 2, FG, "ready", 76);
    flush();
}

void drawSettingsMenu(const device_settings::Settings& settings,
                      const battery_status::Snapshot& battery,
                      battery_status::WarningState warning,
                      uint8_t selectedIndex) {
    drawSettingsChrome("SETTINGS", "tap a control");

    char voice[10];
    char bright[10];
    char volume[10];
    char batt[10];
    snprintf(voice, sizeof(voice), "%s", device_settings::recordModeLabel(settings));
    snprintf(bright, sizeof(bright), "%u%%", (unsigned)settings.brightnessPercent);
    snprintf(volume, sizeof(volume), "%u%%", (unsigned)settings.volumePercent);
    formatBatteryPercent(batt, sizeof(batt), battery);

    static constexpr const char* LABELS[] = {
        "Voice", "Screen", "Volume", "Feel", "Dim", "Battery"
    };
    static constexpr int X[] = {142, 324, 142, 324, 142, 324};
    static constexpr int Y[] = {154, 154, 248, 248, 342, 342};
    static constexpr uint8_t COUNT = sizeof(LABELS) / sizeof(LABELS[0]);

    for (uint8_t i = 0; i < COUNT; ++i) {
        const char* value = "";
        switch (i) {
            case 0: value = voice; break;
            case 1: value = bright; break;
            case 2: value = volume; break;
            case 3: value = feedbackValue(settings); break;
            case 4: value = device_settings::autoDimLabel(settings); break;
            case 5: value = batt; break;
            default: break;
        }
        uint16_t accent = 0x07E0;
        if (i == 5 && battery_status::isWarning(warning)) {
            accent = warning == battery_status::WarningState::Critical ? MOOD_SICK : MOOD_HUNGRY;
        }
        drawMenuOption(X[i], Y[i], LABELS[i], value, selectedIndex == i, accent);
    }
    drawHintLine("tap open | B next");
    flush();
}

void drawPercentSetting(const char* title, uint8_t percent, const char* hint) {
    const bool brightness = strcmp(title, "BRIGHT") == 0;
    drawSettingsChrome(brightness ? "SCREEN" : title,
                       brightness ? "display brightness" : "speaker volume");

    char value[12];
    snprintf(value, sizeof(value), "%u%%", (unsigned)percent);
    target().setTextSize(5);
    target().setTextColor(FG, BG);
    int w = target().textWidth(value);
    target().setCursor(SCREEN_CX - w / 2, 132);
    target().print(value);

    drawLinearGauge(218, percent, 0x07E0);
    drawRoundButton(SCREEN_CX - 86, SCREEN_CY + 84, 58, "-", DIM, BG, FG, 4);
    drawRoundButton(SCREEN_CX + 86, SCREEN_CY + 84, 58, "+", 0x07E0, 0x0340, 0x07E0, 4);
    drawHintLine(hint ? hint : "tap -/+ | B +10");
    flush();
}

void drawFeedbackSettings(const device_settings::Settings& settings) {
    drawSettingsChrome("FEEDBACK", "alerts and haptics");

    const int y = SCREEN_CY + 10;
    const int r = 68;
    const int left = SCREEN_CX - 76;
    const int right = SCREEN_CX + 76;
    const uint16_t soundFill = settings.buttonSound ? 0x0340 : BG;
    const uint16_t vibeFill = settings.vibration ? 0x0340 : BG;

    target().fillCircle(left, y, r, soundFill);
    target().drawCircle(left, y, r, settings.buttonSound ? 0x07E0 : RING_DIM);
    target().drawCircle(left, y, r - 6, settings.buttonSound ? 0x07E0 : DIM);
    drawCenteredBoxText(left, y - 34, 2, FG, "SOUND", r * 2 - 24, soundFill);
    drawCenteredBoxText(left, y + 4, 3, settings.buttonSound ? 0x07E0 : DIM,
                        settings.buttonSound ? "ON" : "OFF", r * 2 - 24, soundFill);

    target().fillCircle(right, y, r, vibeFill);
    target().drawCircle(right, y, r, settings.vibration ? 0x07E0 : RING_DIM);
    target().drawCircle(right, y, r - 6, settings.vibration ? 0x07E0 : DIM);
    drawCenteredBoxText(right, y - 34, 2, FG, "VIBE", r * 2 - 24, vibeFill);
    drawCenteredBoxText(right, y + 4, 3, settings.vibration ? 0x07E0 : DIM,
                        settings.vibration ? "ON" : "OFF", r * 2 - 24, vibeFill);

    drawHintLine("tap toggle | B");
    flush();
}

void drawAutoDimSettings(const device_settings::Settings& settings) {
    drawSettingsChrome("AUTO DIM", "screen rests when idle");

    const char* label = device_settings::autoDimLabel(settings);
    target().setTextSize(5);
    target().setTextColor(settings.autoDimEnabled ? 0x07E0 : DIM, BG);
    int w = target().textWidth(label);
    target().setCursor(SCREEN_CX - w / 2, 128);
    target().print(label);

    drawChoicePill(92, 252, "off", !settings.autoDimEnabled);
    drawChoicePill(186, 252, "15s", settings.autoDimEnabled && settings.autoDimTimeoutMs <= 15000);
    drawChoicePill(280, 252, "30s", settings.autoDimEnabled &&
                               settings.autoDimTimeoutMs > 15000 &&
                               settings.autoDimTimeoutMs <= 30000);
    drawChoicePill(374, 252, "60s", settings.autoDimEnabled && settings.autoDimTimeoutMs > 30000);

    char dim[24];
    snprintf(dim, sizeof(dim), "dim to %u%%", (unsigned)settings.dimBrightnessPercent);
    drawCenteredSafeText(318, 2, FG, dim, 66);
    drawHintLine("tap/B cycle | A");
    flush();
}

void drawBatterySettings(const battery_status::Snapshot& battery,
                         battery_status::WarningState warning,
                         bool lowBatteryWarningEnabled) {
    drawSettingsChrome("BATTERY", battery_status::warningLabel(warning));

    char pct[10];
    formatBatteryPercent(pct, sizeof(pct), battery);
    target().setTextSize(5);
    target().setTextColor(battery_status::isWarning(warning) ? MOOD_HUNGRY : FG, BG);
    int w = target().textWidth(pct);
    target().setCursor(SCREEN_CX - w / 2, 118);
    target().print(pct);

    if (battery.percentKnown && battery.percent >= 0) {
        drawLinearGauge(206, (uint8_t)battery.percent, battery_status::isWarning(warning) ? MOOD_HUNGRY : 0x07E0);
    }

    char line[48];
    drawInfoPill(SCREEN_CX - 76, 268, "state", battery_status::chargeLabel(battery.charge), FG);
    drawInfoPill(SCREEN_CX + 76, 268, "warn", battery_status::warningLabel(warning),
                 battery_status::isWarning(warning) ? MOOD_HUNGRY : 0x07E0);

    if (battery.voltageKnown) {
        snprintf(line, sizeof(line), "%dmV", (int)battery.voltageMv);
    } else {
        snprintf(line, sizeof(line), "voltage unknown");
    }
    drawCenteredSafeText(330, 2, FG, line, 78);

    if (battery.currentKnown) {
        snprintf(line, sizeof(line), "%ldmA", (long)battery.currentMa);
        drawCenteredSafeText(360, 1, DIM, line, 64);
    }

    snprintf(line, sizeof(line), "warnings %s", lowBatteryWarningEnabled ? "on" : "off");
    drawCenteredSafeText(386, 1, lowBatteryWarningEnabled ? 0x07E0 : DIM, line, 68);
    drawHintLine("B warn | refresh");
    flush();
}

void drawSettingsSaved(const char* label) {
    drawSettingsChrome("SAVED", label ? label : "settings");
    drawCenteredSafeText(SCREEN_CY - 12, 3, 0x07E0, "done", 76);
    flush();
}

void drawRec(uint32_t elapsedS, uint32_t totalS, bool autoMode) {
    target().fillRect(0, 50, SCREEN_W, SCREEN_H - 82, BG);
    drawCenteredSafeText(58, 3, 0xF800, "REC", 92);

    char t[16];
    if (totalS == 0) totalS = 1;
    if (elapsedS > totalS) elapsedS = totalS;
    if (autoMode) {
        snprintf(t, sizeof(t), "AUTO");
    } else {
        snprintf(t, sizeof(t), "%lus", (unsigned long)(totalS - elapsedS));
    }
    drawCircularProgress(128, (int)((elapsedS * 1000UL) / totalS), 0xF800, 8);
    drawMicIcon(SCREEN_CX, SCREEN_CY + 10, 0xF800, 0x1800);
    drawCenteredSafeText(SCREEN_CY + 132, 3, FG, t, 80);
    drawHintLine(autoMode ? "pause sends | B" : "tap/B send | A");
    flush();
}

// ---------------------------------------------------------------- thinking ---
void drawThinking() {
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 12);
    const int sweep = 72;
    const int start = -90 + (int)((millis() / 8) % 360);
    drawArcDots(128, start, start + sweep, 0x87F0, 8, 2);
    centeredText(SCREEN_CY - 12, 2, DIM, "sending...");
    flush();
}

// ---------------------------------------------------------------- stats ------
void drawStats(const PetState& s, int rssi, const char* proxyUrl) {
    const uint16_t accent = moodColor(s.mood);
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 10);
    if (hasCodexAccountUsage(s)) {
        int progressX1000 = s.codex_usage_percent_x10;
        if (progressX1000 < 0) progressX1000 = 0;
        if (progressX1000 > 1000) progressX1000 = 1000;
        drawCircularProgress(HOME_RING_R, progressX1000, accent, HOME_RING_THICKNESS, false);
        drawCircularMarker(HOME_RING_R, s.codex_expected_percent_x10, FG, 7);
    } else {
        drawArcDots(HOME_RING_R, -90, 50, accent, HOME_RING_THICKNESS, 6);
    }

    char line[40];
    if (hasCodexPace(s)) {
        formatPaceLabel(line, sizeof(line), s);
    } else if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(line, sizeof(line), "%s weekly", pct);
    } else {
        snprintf(line, sizeof(line), "%ldk today", (long)(s.food_today / 1000));
    }
    drawCenteredSafeText(72, 2, accent, line, 64);

    if (hasCodexAccountUsage(s)) {
        char pct[12];
        formatUsagePercent(pct, sizeof(pct), s);
        snprintf(line, sizeof(line), "%s used", pct);
    } else {
        snprintf(line, sizeof(line), "%ldk total", (long)(s.total_tokens_ever / 1000));
    }
    drawCircularChip(112, SCREEN_CY - 18, 45, line, accent);

    if (hasCodexAccountUsage(s) && s.codex_expected_percent_x10 >= 0) {
        char pct[12];
        formatPercentX10(pct, sizeof(pct), s.codex_expected_percent_x10);
        snprintf(line, sizeof(line), "%s exp", pct);
    } else {
        snprintf(line, sizeof(line), "%d chats", s.audio_runs_today);
    }
    drawCircularChip(SCREEN_W - 112, SCREEN_CY - 18, 45, line, 0x87F0);

    if (hasActivity(s)) {
        char idle[24];
        formatIdleLabel(idle, sizeof(idle), s);
        snprintf(line, sizeof(line), "%s", idle);
        drawCircularChip(SCREEN_CX, SCREEN_CY + 84, 43, line, activityColor(s));
    } else if (hasCodexAccountUsage(s) && s.codex_plan[0]) {
        snprintf(line, sizeof(line), "%s", s.codex_plan);
        drawCircularChip(SCREEN_CX, SCREEN_CY + 84, 43, line, rssi > -70 ? 0x07E0 : 0xFD20);
    } else {
        snprintf(line, sizeof(line), "%d dBm", rssi);
        drawCircularChip(SCREEN_CX, SCREEN_CY + 84, 43, line, rssi > -70 ? 0x07E0 : 0xFD20);
    }

    drawCenteredSafeText(SCREEN_CY - 34, 3, accent, s.mood, 78);

    // Truncate URL visually by skipping the scheme
    const char* host = strstr(proxyUrl, "://");
    host = host ? host + 3 : proxyUrl;
    snprintf(line, sizeof(line), "api %s", host);
    drawCenteredSafeText(360, 1, DIM, line, 76);

    drawHintLine("tap/A home  |  B reset?");
    flush();
}

void drawHintLine(const char* s) {
    drawCenteredSafeText(SCREEN_H - 28, 1, DIM, s, 40);
    flush();
}

void drawGreeting(const char* lastMsg) {
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 12);
    drawCenteredSafeText(88, 2, DIM, "last heard:", 80);

    char buf[64];
    snprintf(buf, sizeof(buf), "\"%s\"", lastMsg);
    drawCenteredSafeText(132, 2, FG, buf, 62);
    flush();
}

void drawConfirmReset() {
    drawArcDots(HOME_RING_R, -90, 270, RING_DIM, HOME_RING_THICKNESS, 10);
    drawCenteredSafeText(SCREEN_CY - 64, 3, 0xFD20, "reset pet?", 78);

    const int yesX = SCREEN_CX - 70;
    const int noX = SCREEN_CX + 70;
    const int actionY = SCREEN_CY + 38;

    target().fillCircle(yesX, actionY, 48, 0x0300);
    target().drawCircle(yesX, actionY, 48, 0x07E0);
    target().setTextSize(2);
    target().setTextColor(0x07E0, 0x0300);
    int w = target().textWidth("A yes");
    target().setCursor(yesX - w / 2, actionY - target().fontHeight() / 2);
    target().print("A yes");

    target().fillCircle(noX, actionY, 48, 0x1800);
    target().drawCircle(noX, actionY, 48, 0xF800);
    target().setTextColor(0xF800, 0x1800);
    w = target().textWidth("B no");
    target().setCursor(noX - w / 2, actionY - target().fontHeight() / 2);
    target().print("B no");
    flush();
}

}  // namespace ui
