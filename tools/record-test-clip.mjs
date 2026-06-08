#!/usr/bin/env node
// ============================================================================
//  record-test-clip.mjs — capture audio, POST it to bridge /transcribe
//
//  Useful for smoke-testing the Whisper/Groq path end-to-end from your Mac
//  before the device-side audio (step 7) is in. Three input modes:
//
//    --seconds N         record N seconds from the default mic
//                        (uses sox `rec`, or ffmpeg, whichever is on PATH)
//    --in FILE           use an existing WAV file
//    --say "TEXT"        macOS-only: synthesize speech via `say` + afconvert
//
//  Default mode is --say (no mic permissions, no extra tools needed on macOS).
//
//  Examples:
//    node tools/record-test-clip.mjs --say "hello tamagotchi"
//    node tools/record-test-clip.mjs --in /path/to/clip.wav
//    node tools/record-test-clip.mjs --seconds 3
//    node tools/record-test-clip.mjs --say "hi" --save /tmp/clip.wav
//                                                  ^-- save audio before posting
//    node tools/record-test-clip.mjs --url http://localhost:8787 --token ...
// ============================================================================

import { readFileSync, writeFileSync, statSync, mkdtempSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename } from "node:path";
import { spawn, spawnSync } from "node:child_process";

const args = parseArgs(process.argv.slice(2));

const URL    = args.url    ?? "http://localhost:8787";
const TOKEN  = args.token  ?? "the-same-long-random-string-as-the-firmware";
const SEC    = parseInt(args.seconds ?? "0", 10);
const IN     = args.in     ?? null;
const SAY    = args.say    ?? null;
const SAVE   = args.save   ?? null;

// --- input selection --------------------------------------------------------
async function captureAudio() {
  if (IN)  return { wav: readFileSync(IN),          source: `file:${IN}` };
  if (SAY) return { wav: await sayToWav(SAY),       source: `say:${SAY}` };
  if (SEC) return { wav: await recordMic(SEC),      source: `mic:${SEC}s` };
  // No mode specified — default to a short TTS sample.
  return { wav: await sayToWav("hello tamagotchi"), source: `say:"hello tamagotchi" (default)` };
}

async function sayToWav(text) {
  const dir = mkdtempSync(join(tmpdir(), "tg-clip-"));
  const aiff = join(dir, "clip.aiff");
  const wav  = join(dir, "clip.wav");

  runOrDie("say", ["-o", aiff, text]);
  // LEI16 = little-endian signed 16-bit integer; 16kHz mono matches firmware
  runOrDie("afconvert", ["-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, wav]);
  const buf = readFileSync(wav);
  if (SAVE) writeFileSync(SAVE, buf);
  return buf;
}

async function recordMic(seconds) {
  const dir = mkdtempSync(join(tmpdir(), "tg-clip-"));
  const wav = join(dir, "clip.wav");

  if (which("rec")) {
    runOrDie("rec", [
      "-r", "16000", "-c", "1", "-e", "signed-integer", "-b", "16",
      wav, "trim", "0", String(seconds),
    ]);
  } else if (which("ffmpeg")) {
    runOrDie("ffmpeg", [
      "-y", "-f", "avfoundation", "-i", ":0",
      "-ar", "16000", "-ac", "1", "-acodec", "pcm_s16le",
      "-t", String(seconds), wav,
    ]);
  } else {
    throw new Error("no audio tool found. Install sox (`brew install sox`) or ffmpeg, or use --in/--say instead.");
  }
  const buf = readFileSync(wav);
  if (SAVE) writeFileSync(SAVE, buf);
  return buf;
}

// --- POST /transcribe ------------------------------------------------------
async function postWav(wav) {
  const url = URL.replace(/\/$/, "") + "/transcribe";
  const r = await fetch(url, {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${TOKEN}`,
      "Content-Type":  "audio/wav",
    },
    body: wav,
  });
  const text = await r.text();
  let body;
  try { body = JSON.parse(text); } catch { body = { raw: text }; }
  return { status: r.status, body };
}

// --- main -------------------------------------------------------------------
async function main() {
  console.log(`[record-test-clip] url=${URL} token=${TOKEN.slice(0, 8)}…`);
  const { wav, source } = await captureAudio();
  const sizeKb = (wav.length / 1024).toFixed(1);
  console.log(`[record-test-clip] source=${source} size=${sizeKb}KB`);

  if (SAVE) console.log(`[record-test-clip] saved -> ${SAVE}`);

  const t0 = Date.now();
  const { status, body } = await postWav(wav);
  const ms = Date.now() - t0;
  console.log(`[record-test-clip] <- ${status} in ${ms}ms`);
  console.log(JSON.stringify(body, null, 2));
  process.exit(status === 200 ? 0 : 1);
}

main().catch(e => {
  console.error("[record-test-clip] error:", e.message ?? e);
  process.exit(2);
});

// --- helpers ----------------------------------------------------------------
function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith("--")) {
      const k = a.slice(2);
      const v = argv[i + 1] && !argv[i + 1].startsWith("--") ? argv[++i] : "true";
      out[k] = v;
    }
  }
  return out;
}

function which(cmd) {
  const r = spawnSync("which", [cmd], { encoding: "utf8" });
  return r.status === 0 && r.stdout.trim().length > 0;
}

function runOrDie(cmd, args) {
  const r = spawnSync(cmd, args, { stdio: ["ignore", "inherit", "inherit"] });
  if (r.status !== 0) {
    throw new Error(`${cmd} exited with status ${r.status}`);
  }
}
