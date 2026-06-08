#!/usr/bin/env node
// ============================================================================
//  generate-sprites.mjs — produce 16 pet-face sprites as RGB565 C header.
//
//  Layout:
//    4 moods (happy, hungry, sleepy, sick) x 4 blink frames
//    each 80x80 RGB565, drawn as opaque (BG = 0x0000 matches the disc BG)
//
//  Output:
//    firmware/src/sprites.h — the pet_sprites[] array, PROGMEM-const.
//
//  Regenerate whenever the art changes:
//    node tools/generate-sprites.mjs
//
//  To swap in your own art:
//    1. draw 16 PNGs at 80x80, named happy_0.png .. sick_3.png
//    2. add a PNG->RGB565 loader here (e.g. via the `pngjs` lib) and write
//       the array straight through. This script's structure stays the same.
// ============================================================================

import { writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));

const W = 80, H = 80;
const FRAMES = 4;
const MOODS = ["happy", "hungry", "sleepy", "sick"];

// --- color helpers ----------------------------------------------------------
function rgb(r, g, b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const C = {
  BG:        0x0000,
  HEAD_HAPPY:  rgb(0xFF, 0xC0, 0x00),  // warm yellow
  HEAD_HUNGRY: rgb(0xFD, 0xA0, 0x00),  // amber/orange
  HEAD_SLEEPY: rgb(0x00, 0x7F, 0xFF),  // soft blue
  HEAD_SICK:   rgb(0x07, 0xE0, 0x00),  // bright green
  EYE:        rgb(0x00, 0x00, 0x00),
  EYE_WHITE:  rgb(0xFF, 0xFF, 0xFF),
  EYE_SPARK:  rgb(0xFF, 0xFF, 0xFF),
  MOUTH:      rgb(0x00, 0x00, 0x00),
  CHEEK:      rgb(0xFD, 0xA0, 0xA0),
  ZZZ:        rgb(0xFF, 0xFF, 0xFF),
  SWEAT:      rgb(0x00, 0xFF, 0xFF),
  OUTLINE:    rgb(0x10, 0x10, 0x10),
};

// --- drawing primitives -----------------------------------------------------
function makeCanvas() { return new Uint16Array(W * H); }
function setPx(c, x, y, color) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  c[y * W + x] = color;
}
function fillCircle(c, cx, cy, r, color) {
  for (let y = cy - r; y <= cy + r; y++) {
    for (let x = cx - r; x <= cx + r; x++) {
      if ((x - cx) ** 2 + (y - cy) ** 2 <= r * r) setPx(c, x, y, color);
    }
  }
}
function ring(c, cx, cy, r, color, thickness = 1) {
  const ri = r - thickness;
  for (let y = cy - r; y <= cy + r; y++) {
    for (let x = cx - r; x <= cx + r; x++) {
      const d2 = (x - cx) ** 2 + (y - cy) ** 2;
      if (d2 <= r * r && d2 >= ri * ri) setPx(c, x, y, color);
    }
  }
}
function drawLine(c, x1, y1, x2, y2, color, thickness = 1) {
  const dx = x2 - x1, dy = y2 - y1;
  const steps = Math.max(Math.abs(dx), Math.abs(dy), 1);
  for (let i = 0; i <= steps; i++) {
    const t = i / steps;
    const cx = Math.round(x1 + t * dx);
    const cy = Math.round(y1 + t * dy);
    for (let ox = 0; ox < thickness; ox++) {
      for (let oy = 0; oy < thickness; oy++) {
        setPx(c, cx + ox, cy + oy, color);
        setPx(c, cx - ox, cy + oy, color);
        setPx(c, cx + ox, cy - oy, color);
        setPx(c, cx - ox, cy - oy, color);
      }
    }
  }
}
function drawArc(c, cx, cy, r, startDeg, endDeg, color, thickness = 1) {
  for (let a = startDeg; a <= endDeg; a += 0.6) {
    const rad = (a * Math.PI) / 180;
    const x = Math.round(cx + r * Math.cos(rad));
    const y = Math.round(cy + r * Math.sin(rad));
    for (let ox = 0; ox < thickness; ox++) {
      for (let oy = 0; oy < thickness; oy++) {
        setPx(c, x + ox, y + oy, color);
        setPx(c, x - ox, y + oy, color);
        setPx(c, x + ox, y - oy, color);
        setPx(c, x - ox, y - oy, color);
      }
    }
  }
}

// --- per-mood art -----------------------------------------------------------
const CX = 40, CY = 40;

function head(c, color) {
  fillCircle(c, CX, CY, 30, color);
  ring(c, CX, CY, 30, C.OUTLINE, 1);
}

function drawHappy(c, blink) {
  head(c, C.HEAD_HAPPY);
  // Cheeks
  fillCircle(c, 22, 46, 3, C.CHEEK);
  fillCircle(c, 58, 46, 3, C.CHEEK);
  // Eyes
  const yE = 34, xL = 28, xR = 52;
  if (blink === 0 || blink === 3) {
    fillCircle(c, xL, yE, 5, C.EYE_WHITE);
    fillCircle(c, xR, yE, 5, C.EYE_WHITE);
    fillCircle(c, xL + 1, yE + 1, 3, C.EYE);
    fillCircle(c, xR + 1, yE + 1, 3, C.EYE);
    setPx(c, xL - 1, yE - 1, C.EYE_SPARK);
    setPx(c, xR - 1, yE - 1, C.EYE_SPARK);
  } else if (blink === 1) {
    fillCircle(c, xL, yE, 4, C.EYE_WHITE);
    fillCircle(c, xR, yE, 4, C.EYE_WHITE);
    drawLine(c, xL - 4, yE, xL + 4, yE, C.EYE, 2);
    drawLine(c, xR - 4, yE, xR + 4, yE, C.EYE, 2);
  } else {
    drawLine(c, xL - 4, yE, xL + 4, yE, C.EYE, 2);
    drawLine(c, xR - 4, yE, xR + 4, yE, C.EYE, 2);
  }
  // Smile
  drawArc(c, CX, 44, 8, 20, 160, C.MOUTH, 2);
}

function drawHungry(c, blink) {
  head(c, C.HEAD_HUNGRY);
  // Droopy eyes
  const yE = 36, xL = 28, xR = 52;
  if (blink === 0 || blink === 3) {
    fillCircle(c, xL, yE, 4, C.EYE_WHITE);
    fillCircle(c, xR, yE, 4, C.EYE_WHITE);
    fillCircle(c, xL, yE + 1, 2, C.EYE);
    fillCircle(c, xR, yE + 1, 2, C.EYE);
  } else {
    drawLine(c, xL - 3, yE, xL + 3, yE + 1, C.EYE, 1);
    drawLine(c, xR - 3, yE, xR + 3, yE + 1, C.EYE, 1);
  }
  // Frown
  drawArc(c, CX, 56, 8, 200, 340, C.MOUTH, 2);
  // Little tummy rumble mark
  if (blink === 0 || blink === 2) {
    drawLine(c, 36, 68, 44, 68, C.OUTLINE, 1);
  }
}

function drawSleepy(c, blink) {
  head(c, C.HEAD_SLEEPY);
  // Closed eyes (curved lines)
  const yE = 34;
  drawLine(c, 22, yE, 30, yE + 3, C.EYE, 2);
  drawLine(c, 30, yE + 3, 34, yE - 2, C.EYE, 2);
  drawLine(c, 46, yE - 2, 50, yE + 3, C.EYE, 2);
  drawLine(c, 50, yE + 3, 58, yE, C.EYE, 2);
  // Small relaxed mouth
  drawArc(c, CX, 48, 5, 30, 150, C.MOUTH, 2);
  // Zzz (faded on mid-blink frames)
  const zzzAlpha = blink === 1 ? 0 : (blink === 2 ? 0.5 : 1);
  for (const [x, y, w] of [[66, 18, 1], [70, 12, 1], [74, 6, 1]]) {
    if (zzzAlpha >= 1) {
      setPx(c, x, y, C.ZZZ);
      setPx(c, x + w, y, C.ZZZ);
    }
  }
}

function drawSick(c, blink) {
  head(c, C.HEAD_SICK);
  // X eyes
  const yE = 32;
  for (const cx of [27, 53]) {
    drawLine(c, cx - 4, yE - 4, cx + 4, yE + 4, C.EYE, 2);
    drawLine(c, cx + 4, yE - 4, cx - 4, yE + 4, C.EYE, 2);
  }
  // Frown
  drawArc(c, CX, 56, 8, 200, 340, C.MOUTH, 2);
  // Sweat drop (top right)
  if (blink !== 1) {
    fillCircle(c, 60, 22, 3, C.SWEAT);
    setPx(c, 60, 26, C.SWEAT);
    setPx(c, 59, 27, C.SWEAT);
    setPx(c, 61, 27, C.SWEAT);
  }
}

// --- generate ---------------------------------------------------------------
const DRAWS = { happy: drawHappy, hungry: drawHungry, sleepy: drawSleepy, sick: drawSick };
const sprites = MOODS.map((mood) =>
  Array.from({ length: FRAMES }, (_, fi) => {
    const c = makeCanvas();
    DRAWS[mood](c, fi);
    return c;
  })
);

// --- emit C header ----------------------------------------------------------
let out = `// Auto-generated by tools/generate-sprites.mjs — do not edit by hand.
// 16 sprites: 4 moods (happy, hungry, sleepy, sick) x ${FRAMES} blink frames.
// ${W}x${H} RGB565 each. Index = mood * ${FRAMES} + frame.
// Pixels outside the head circle are 0x0000 (matches the disc BG).
// Total size: ${(16 * W * H * 2 / 1024).toFixed(1)} KB, lives in flash (PROGMEM).

#pragma once
#include <stdint.h>
#include <pgmspace.h>

#define PET_SPRITE_W ${W}
#define PET_SPRITE_H ${H}
#define PET_SPRITE_FRAMES ${FRAMES}
#define PET_SPRITE_MOODS 4

enum PetSpriteMood : uint8_t {
  PET_HAPPY  = 0,
  PET_HUNGRY = 1,
  PET_SLEEPY = 2,
  PET_SICK   = 3,
};

const uint16_t pet_sprites[PET_SPRITE_MOODS * PET_SPRITE_FRAMES][PET_SPRITE_W * PET_SPRITE_H] PROGMEM = {
`;

for (let mi = 0; mi < MOODS.length; mi++) {
  for (let fi = 0; fi < FRAMES; fi++) {
    const s = sprites[mi][fi];
    out += `  // ${MOODS[mi]} frame ${fi}\n  {`;
    for (let i = 0; i < s.length; i++) {
      if (i % 16 === 0) out += "\n    ";
      out += `0x${s[i].toString(16).padStart(4, "0")},`;
    }
    out += "\n  },\n";
  }
}
out += "};\n";

const outPath = join(__dirname, "..", "firmware", "src", "sprites.h");
mkdirSync(dirname(outPath), { recursive: true });
writeFileSync(outPath, out);
console.log(`wrote ${outPath}  (${(out.length / 1024).toFixed(1)} KB, ${MOODS.length * FRAMES} sprites)`);
