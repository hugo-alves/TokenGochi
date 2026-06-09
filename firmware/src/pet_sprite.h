#pragma once
// Sprite renderer for the pet face. Sprites live in PROGMEM (sprites.h),
// one uint16_t per pixel, RGB565. The 0x0000 pixels around the head
// match the disc BG so the sprite blends into the round display.

#include <stdint.h>
#include "pet_state.h"

namespace pet_sprite {

// Look up a mood index from the bridge's string. Defaults to "happy".
uint8_t moodIndex(const char* mood);

// Draw sprite(mood, frame) centered on the disc. Frame is 0..FRAMES-1;
// invalid values are wrapped. Scale is nearest-neighbor so the generated
// sprite data can stay compact in flash.
void drawCentered(uint8_t moodIdx, uint8_t frame, uint8_t scale = 1);

// Advance the blink animation. Returns true iff the frame index changed,
// so the caller knows when to redraw the sprite.
bool tickBlink();

// Last frame index set by tickBlink().
uint8_t currentFrame();

}  // namespace pet_sprite
