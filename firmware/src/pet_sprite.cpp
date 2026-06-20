#include "pet_sprite.h"
#include "config.h"
#include "sprites.h"
#include "ui.h"

#include <M5Unified.h>
#include <string.h>

namespace pet_sprite {

static uint8_t g_frame = 0;
static uint32_t g_lastBlinkMs = 0;
static const uint32_t BLINK_PERIOD_MS = 250;  // 4 fps

uint8_t moodIndex(const char* mood) {
    if (!mood) return PET_HAPPY;
    if (strcmp(mood, "excited") == 0) return PET_HAPPY;
    if (strcmp(mood, "very happy") == 0) return PET_HAPPY;
    if (strcmp(mood, "peckish") == 0) return PET_HUNGRY;
    if (strcmp(mood, "very hungry") == 0) return PET_HUNGRY;
    if (strcmp(mood, "hungry") == 0) return PET_HUNGRY;
    if (strcmp(mood, "sleepy") == 0) return PET_SLEEPY;
    if (strcmp(mood, "grumpy") == 0) return PET_SICK;
    if (strcmp(mood, "very grumpy") == 0) return PET_SICK;
    if (strcmp(mood, "very_grumpy") == 0) return PET_SICK;
    if (strcmp(mood, "sick")   == 0) return PET_SICK;
    return PET_HAPPY;
}

void drawCentered(uint8_t moodIdx, uint8_t frame, uint8_t scale) {
    if (moodIdx >= PET_SPRITE_MOODS) moodIdx = PET_HAPPY;
    frame %= PET_SPRITE_FRAMES;
    if (scale < 1) scale = 1;

    const int drawW = PET_SPRITE_W * scale;
    const int drawH = PET_SPRITE_H * scale;
    const int x = SCREEN_CX - drawW / 2;
    const int y = SCREEN_CY - drawH / 2;

    if (scale == 1) {
        // pushImage copies the 16-bit RGB565 pixels from PROGMEM straight to
        // the framebuffer. `transparent` would chroma-key a color, but we
        // rely on the sprite's BG (0x0000) matching the disc BG instead.
        bool oldSwap = ui::target().getSwapBytes();
        ui::target().setSwapBytes(true);
        ui::target().pushImage(x, y, PET_SPRITE_W, PET_SPRITE_H,
                               pet_sprites[moodIdx * PET_SPRITE_FRAMES + frame]);
        ui::target().setSwapBytes(oldSwap);
        ui::flush();
        return;
    }

    ui::target().fillRect(x, y, drawW, drawH, 0x0000);
    const uint16_t* sprite = pet_sprites[moodIdx * PET_SPRITE_FRAMES + frame];
    for (int py = 0; py < PET_SPRITE_H; ++py) {
        for (int px = 0; px < PET_SPRITE_W; ++px) {
            const uint16_t color = pgm_read_word(&sprite[py * PET_SPRITE_W + px]);
            if (color == 0x0000) continue;
            ui::target().fillRect(x + px * scale,
                                  y + py * scale,
                                  scale,
                                  scale,
                                  color);
        }
    }
    ui::flush();
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
