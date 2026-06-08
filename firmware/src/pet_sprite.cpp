#include "pet_sprite.h"
#include "config.h"
#include "sprites.h"

#include <M5Unified.h>
#include <string.h>

namespace pet_sprite {

static uint8_t g_frame = 0;
static uint32_t g_lastBlinkMs = 0;
static const uint32_t BLINK_PERIOD_MS = 250;  // 4 fps

uint8_t moodIndex(const char* mood) {
    if (!mood) return PET_HAPPY;
    if (strcmp(mood, "hungry") == 0) return PET_HUNGRY;
    if (strcmp(mood, "sleepy") == 0) return PET_SLEEPY;
    if (strcmp(mood, "sick")   == 0) return PET_SICK;
    return PET_HAPPY;
}

void drawCentered(uint8_t moodIdx, uint8_t frame) {
    if (moodIdx >= PET_SPRITE_MOODS) moodIdx = PET_HAPPY;
    frame %= PET_SPRITE_FRAMES;

    const int x = SCREEN_CX - PET_SPRITE_W / 2;
    const int y = SCREEN_CY - PET_SPRITE_H / 2;

    // pushImage copies the 16-bit RGB565 pixels from PROGMEM straight to
    // the framebuffer. `transparent` would chroma-key a color, but we
    // rely on the sprite's BG (0x0000) matching the disc BG instead.
    M5.Display.pushImage(x, y, PET_SPRITE_W, PET_SPRITE_H,
                         pet_sprites[moodIdx * PET_SPRITE_FRAMES + frame]);
}

bool tickBlink() {
    uint32_t now = millis();
    if (now - g_lastBlinkMs >= BLINK_PERIOD_MS) {
        g_lastBlinkMs = now;
        g_frame = (g_frame + 1) % PET_SPRITE_FRAMES;
        return true;
    }
    return false;
}

uint8_t currentFrame() { return g_frame; }

}  // namespace pet_sprite
