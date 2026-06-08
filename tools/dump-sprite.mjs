#!/usr/bin/env node
// tools/dump-sprite.mjs — render one of the pet sprites to a PPM image so
// you can sanity-check the art without flashing the device. No deps.
//
//   node tools/dump-sprite.mjs happy 0     # writes /tmp/pet-happy_0.ppm
//   node tools/dump-sprite.mjs sick  3
//
// View with:  open /tmp/pet-happy_0.ppm  (Preview.app on macOS)
//
// Reads the .h file directly and decodes the RGB565 array.

import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error("usage: node tools/dump-sprite.mjs <mood> <frame>");
  console.error("  moods: happy | hungry | sleepy | sick");
  console.error("  frame: 0..3");
  process.exit(1);
}
const MOOD = args[0];
const FRAME = parseInt(args[1], 10);
if (!["happy", "hungry", "sleepy", "sick"].includes(MOOD)) {
  console.error("unknown mood:", MOOD); process.exit(1);
}

const hPath = join(__dirname, "..", "firmware", "src", "sprites.h");
const src = readFileSync(hPath, "utf8");

// Pull PET_SPRITE_W / PET_SPRITE_H out of the header.
const W = parseInt(src.match(/PET_SPRITE_W\s+(\d+)/)[1], 10);
const H = parseInt(src.match(/PET_SPRITE_H\s+(\d+)/)[1], 10);
const FRAMES = parseInt(src.match(/PET_SPRITE_FRAMES\s+(\d+)/)[1], 10);

const moodIdx = ["happy", "hungry", "sleepy", "sick"].indexOf(MOOD);
const blockIdx = moodIdx * FRAMES + FRAME;

// Find the Nth `{` block-start comment that matches our frame.
// We rely on the comment "// happy frame 0" appearing just before the block.
const re = new RegExp(`\\/\\/\\s+${MOOD}\\s+frame\\s+${FRAME}\\s*\\n\\s*\\{([\\s\\S]*?)\\n\\s*\\},`);
const m = src.match(re);
if (!m) { console.error("could not find sprite block in", hPath); process.exit(2); }

const hex = m[1].match(/0x[0-9a-fA-F]{4}/g) || [];
if (hex.length !== W * H) {
  console.error(`pixel count mismatch: got ${hex.length}, want ${W * H}`);
  process.exit(2);
}

// Convert RGB565 -> RGB888 and emit PPM (P6).
const pixels = Buffer.alloc(W * H * 3);
for (let i = 0; i < hex.length; i++) {
  const v = parseInt(hex[i], 16);
  const r = ((v >> 11) & 0x1F) << 3;
  const g = ((v >> 5)  & 0x3F) << 2;
  const b = ( v        & 0x1F) << 3;
  pixels[i * 3 + 0] = r;
  pixels[i * 3 + 1] = g;
  pixels[i * 3 + 2] = b;
}

const outPath = `/tmp/pet-${MOOD}_${FRAME}.ppm`;
const header = Buffer.from(`P6\n${W} ${H}\n255\n`, "utf8");
writeFileSync(outPath, Buffer.concat([header, pixels]));

// Also write a 4x scaled BMP-style preview as a 2nd file using PPM again but
// doubled — skip, just print the path.
console.log(`wrote ${outPath}  (${W}x${H} RGB888, ${(pixels.length/1024).toFixed(1)} KB)`);
console.log(`view:    open ${outPath}`);
