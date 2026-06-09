#!/usr/bin/env node
// ============================================================================
//  Token Tamagotchi — self-contained bridge (no CodexBar dependency)
//  Reads the local JSONL transcripts that Claude Code and Codex CLI write to
//  disk, sums today's tokens, and serves:
//    GET  /tokens_today   — { tokens_today, breakdown, ts }
//    GET  /pet/state      — derived pet stats (mood, age, food, last_msg)
//    GET  /health         — service status, no auth
//  Plus a JSONL-watching cache and a tiny state.json for pet persistence.
// ----------------------------------------------------------------------------
//  Run:
//    node bridge/tamagotchi-bridge.mjs            # start the LAN server
//    node bridge/tamagotchi-bridge.mjs --once     # print tokens + state once
//
//  Configuration: drop a .env in this directory (see .env.example), or set
//    the same vars in the environment. process.env always wins.
//
//  Wire-up:
//    ipconfig getifaddr en0    # this Mac's LAN IP
//    -> set firmware PROXY_URL to http://<that-ip>:8787/
//    -> keep DEVICE_TOKEN identical on both sides.
//
//  Always-on: see com.tokengochi.bridge.plist in this directory.
//
//  Requires: Node 18+. No npm install.
// ============================================================================

import { createServer } from "node:http";
import { createReadStream, readFileSync, writeFileSync } from "node:fs";
import { readdir, stat } from "node:fs/promises";
import { createInterface } from "node:readline";
import { homedir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------- env loader --
// Tiny .env reader. process.env always wins. No dotenv dep, no surprises.
function loadEnv() {
  const path = join(__dirname, ".env");
  let text;
  try {
    text = readFileSync(path, "utf8");
  } catch {
    return;
  }
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const m = line.match(/^([A-Z0-9_]+)\s*=\s*(.*?)\s*$/i);
    if (!m) continue;
    const [, key, value] = m;
    if (process.env[key] !== undefined) continue;
    process.env[key] = value.replace(/^["']|["']$/g, "").trim();
  }
}
loadEnv();

// ---------------------------------------------------------------- config -----
const PORT               = parseInt(process.env.PORT || "8787", 10);
const DEVICE_TOKEN       = process.env.DEVICE_TOKEN || "the-same-long-random-string-as-the-firmware";
const CACHE_MS           = 30_000;  // re-scan logs at most every 30s
const INCLUDE_CACHE      = true;    // count cache read/creation tokens as "food" too
const GROQ_API_KEY       = process.env.GROQ_API_KEY || "";
const GROQ_WHISPER_MODEL = process.env.GROQ_WHISPER_MODEL || "whisper-large-v3-turbo";
const GROQ_URL           = process.env.GROQ_URL || "https://api.groq.com/openai/v1/audio/transcriptions";
const VERSION            = "0.2.0";

// ---------------------------------------------------------------- dates ------
const pad = (n) => String(n).padStart(2, "0");
const ymd = (d) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;

function isToday(ts) {
  if (!ts) return false;
  const d = new Date(ts);
  return !isNaN(d) && ymd(d) === ymd(new Date());
}
function startOfTodayMs() {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

// ---------------------------------------------------------------- state ------
const STATE_PATH = join(__dirname, "state.json");
const defaultState = () => ({
  pet_birth_ts: Math.floor(Date.now() / 1000),
  last_msg: "",
  last_msg_ts: 0,
  total_tokens_ever: 0,
  peak_today_today: 0,
  peak_today_date: ymd(new Date()),
  audio_runs_today: 0,
  audio_runs_today_date: ymd(new Date()),
});

let stateCache = null;
function loadState() {
  if (stateCache) return stateCache;
  try {
    const raw = readFileSync(STATE_PATH, "utf8");
    stateCache = { ...defaultState(), ...JSON.parse(raw) };
  } catch {
    stateCache = defaultState();
    saveState();
  }
  return stateCache;
}
function saveState() {
  if (!stateCache) return;
  try {
    writeFileSync(STATE_PATH, JSON.stringify(stateCache, null, 2));
  } catch (e) {
    console.error("state.json write failed:", e);
  }
}

// Track monotonic token total across day rollovers.
// Only counts growth above the peak seen this calendar day, so a bridge
// restart mid-day never double-counts, and a fresh day starts the peak at 0
// (capturing any work that happened before the bridge noticed the new day).
function bumpTotals(tokensToday) {
  const s = loadState();
  const today = ymd(new Date());
  if (s.peak_today_date !== today) {
    s.peak_today_date = today;
    s.peak_today_today = 0;
  }
  if (tokensToday > s.peak_today_today) {
    s.total_tokens_ever += tokensToday - s.peak_today_today;
    s.peak_today_today = tokensToday;
    saveState();
  }
}

// Stamp the state with the most recent transcript and bump the daily counter.
function recordTranscription(text) {
  const s = loadState();
  const today = ymd(new Date());
  if (s.audio_runs_today_date !== today) {
    s.audio_runs_today_date = today;
    s.audio_runs_today = 0;
  }
  s.last_msg = String(text).slice(0, 500);
  s.last_msg_ts = Math.floor(Date.now() / 1000);
  s.audio_runs_today += 1;
  saveState();
}

// ---------------------------------------------------------------- mood -------
// Rule-based. See PLAN.md §4.2.
function computeMood(foodToday, now = new Date()) {
  const h = now.getHours();
  if (h < 7 || h >= 23) return "sleepy";
  if (foodToday === 0 && h >= 22) return "sick";
  if (foodToday < 5_000) return "hungry";
  return "happy";
}

// ---------------------------------------------------------------- audio -----
// Read the full request body into a Buffer, capped at maxBytes. Rejects
// (and aborts the socket) the moment a chunk pushes us over the cap.
function readBody(req, maxBytes) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on("data", (chunk) => {
      total += chunk.length;
      if (total > maxBytes) {
        req.destroy();
        reject(new Error("body too large"));
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

// Build a multipart/form-data body as a single Buffer. Zero deps.
//   fields: [{ name, value, filename?, contentType? }]
//   value may be a string (utf8) or Buffer.
function buildMultipart(boundary, fields) {
  const parts = [];
  for (const f of fields) {
    let header = `Content-Disposition: form-data; name="${f.name}"`;
    if (f.filename) header += `; filename="${f.filename}"`;
    header += "\r\n";
    if (f.contentType) header += `Content-Type: ${f.contentType}\r\n`;
    header += "\r\n";
    parts.push(Buffer.from(header, "utf8"));
    parts.push(Buffer.isBuffer(f.value) ? f.value : Buffer.from(String(f.value), "utf8"));
    parts.push(Buffer.from("\r\n", "utf8"));
  }
  const close = Buffer.from(`--${boundary}--\r\n`, "utf8");
  return Buffer.concat([...parts, close]);
}

// Best-effort: pull duration_ms from a 16-bit-PCM WAV buffer. Returns null
// if the header is malformed; the firmware is expected to send canonical WAV.
function wavDurationMs(buf) {
  if (buf.length < 44) return null;
  if (buf.toString("ascii", 0, 4) !== "RIFF") return null;
  if (buf.toString("ascii", 8, 12) !== "WAVE") return null;
  let off = 12;
  while (off + 8 <= buf.length) {
    const id = buf.toString("ascii", off, off + 4);
    const size = buf.readUInt32LE(off + 4);
    if (id === "fmt ") {
      const sampleRate  = buf.readUInt32LE(off + 8 + 4);
      const numChannels = buf.readUInt16LE(off + 8 + 2);
      const bps         = buf.readUInt16LE(off + 8 + 14);
      let dataOff = off + 8 + size;
      while (dataOff + 8 <= buf.length) {
        const dId = buf.toString("ascii", dataOff, dataOff + 4);
        const dSize = buf.readUInt32LE(dataOff + 4);
        if (dId === "data") {
          const bytesPerSec = sampleRate * numChannels * (bps / 8);
          return Math.round((dSize / bytesPerSec) * 1000);
        }
        dataOff += 8 + dSize;
      }
      return null;
    }
    off += 8 + size;
  }
  return null;
}

// ---------------------------------------------------------------- io ---------
async function* streamLines(file) {
  const rl = createInterface({
    input: createReadStream(file, { encoding: "utf8" }),
    crlfDelay: Infinity,
  });
  for await (const line of rl) if (line.trim()) yield line;
}

async function* walk(dir) {
  let entries;
  try {
    entries = await readdir(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else if (e.isFile() && e.name.endsWith(".jsonl")) yield p;
  }
}

// ------------------------------------------------------------ Claude Code ----
// ~/.claude/projects/<encoded-project>/<session>.jsonl  (one file per session).
// Assistant-message lines carry message.usage with the token counts.
function claudeRoots() {
  const roots = [];
  if (process.env.CLAUDE_CONFIG_DIR) roots.push(process.env.CLAUDE_CONFIG_DIR);
  roots.push(join(homedir(), ".claude"));
  roots.push(join(homedir(), ".config", "claude"));
  return roots.map((r) => join(r, "projects"));
}

async function computeClaude(seen) {
  const since = startOfTodayMs();
  let total = 0;
  for (const root of claudeRoots()) {
    for await (const file of walk(root)) {
      try {
        if ((await stat(file)).mtimeMs < since) continue;
      } catch {
        continue;
      }
      for await (const line of streamLines(file)) {
        let o;
        try {
          o = JSON.parse(line);
        } catch {
          continue;
        }
        const msg = o.message;
        const u = msg?.usage;
        if (!u) continue;

        // dedup: the same message reappears across files on resume/branch
        const key = `${msg.id ?? o.uuid ?? ""}:${o.requestId ?? o.request_id ?? ""}`;
        if (key !== ":" && seen.has(key)) continue;
        if (key !== ":") seen.add(key);

        if (!isToday(o.timestamp)) continue;

        total += (u.input_tokens || 0) + (u.output_tokens || 0);
        if (INCLUDE_CACHE) {
          total += (u.cache_creation_input_tokens || 0) + (u.cache_read_input_tokens || 0);
        }
      }
    }
  }
  return total;
}

// --------------------------------------------------------------- Codex CLI ---
// ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl  (CODEX_HOME may override / list).
// token_count events report *cumulative* totals -> diff within a file for deltas.
function codexHomes() {
  const raw = process.env.CODEX_HOME || join(homedir(), ".codex");
  return raw.split(",").map((s) => s.trim()).filter(Boolean);
}

export function codexCumulative(p) {
  if (!p || typeof p !== "object") return null;
  if (p.total_token_usage && typeof p.total_token_usage === "object") {
    return codexCumulative(p.total_token_usage);
  }
  if (typeof p.total_tokens === "number") return p.total_tokens;
  const keys = [
    "input_tokens", "cached_input_tokens", "output_tokens",
    "reasoning_output_tokens", "cache_read_input_tokens",
  ];
  let s = 0, found = false;
  for (const k of keys) if (typeof p[k] === "number") { s += p[k]; found = true; }
  return found ? s : null;
}

async function* codexFiles() {
  const now = new Date();
  const yest = new Date(Date.now() - 86_400_000); // catch sessions crossing midnight
  for (const home of codexHomes()) {
    for (const [d, isTodayFolder] of [[now, true], [yest, false]]) {
      const dir = join(home, "sessions", String(d.getFullYear()), pad(d.getMonth() + 1), pad(d.getDate()));
      let entries;
      try {
        entries = await readdir(dir, { withFileTypes: true });
      } catch {
        continue;
      }
      for (const e of entries) {
        if (e.isFile() && e.name.startsWith("rollout-") && e.name.endsWith(".jsonl")) {
          yield { file: join(dir, e.name), folderIsToday: isTodayFolder };
        }
      }
    }
  }
}

async function computeCodex() {
  let total = 0;
  for await (const { file, folderIsToday } of codexFiles()) {
    let prev = 0; // cumulative is per-session, so reset per file
    for await (const line of streamLines(file)) {
      let o;
      try {
        o = JSON.parse(line);
      } catch {
        continue;
      }
      const payload = o.payload ?? o;
      const isTokenCount = payload?.type === "token_count" || o.type === "token_count";
      if (!isTokenCount) continue;

      const cur =
        codexCumulative(payload) ??
        codexCumulative(payload.info) ??
        codexCumulative(payload.usage);
      if (cur == null) continue;

      const delta = Math.max(0, cur - prev);
      prev = cur;

      const ts = o.timestamp ?? payload.timestamp;
      if (ts ? isToday(ts) : folderIsToday) total += delta;
    }
  }
  return total;
}

// ------------------------------------------------------------- aggregate -----
let cache = { at: 0, payload: null };

async function tokensToday() {
  if (cache.payload && Date.now() - cache.at < CACHE_MS) return cache.payload;

  const seen = new Set();
  const [claude, codex] = await Promise.all([computeClaude(seen), computeCodex()]);
  const payload = {
    tokens_today: claude + codex,
    breakdown: { claude, codex },
    ts: Math.floor(Date.now() / 1000),
  };
  cache = { at: Date.now(), payload };
  return payload;
}

async function petState() {
  const t = await tokensToday();
  bumpTotals(t.tokens_today);
  const s = loadState();
  return {
    mood: computeMood(t.tokens_today),
    age_s: Math.max(0, Math.floor(Date.now() / 1000) - s.pet_birth_ts),
    food_today: t.tokens_today,
    last_msg: s.last_msg,
    last_msg_ts: s.last_msg_ts,
    total_tokens_ever: s.total_tokens_ever,
    audio_runs_today: s.audio_runs_today,
    breakdown: t.breakdown,
    ts: t.ts,
  };
}

// ---------------------------------------------------------------- http --------
function authOk(req) {
  return (req.headers.authorization ?? "") === `Bearer ${DEVICE_TOKEN}`;
}
function send(res, status, body, contentType = "application/json") {
  res.writeHead(status, { "content-type": contentType });
  res.end(contentType === "application/json" ? JSON.stringify(body) : body);
}

async function main() {
  if (process.argv.includes("--once")) {
    const t = await tokensToday();
    const s = loadState();
    console.log(JSON.stringify({
      tokens: t,
      pet: { ...s, mood: computeMood(t.tokens_today) },
    }, null, 2));
    return;
  }

  createServer(async (req, res) => {
    const t0 = Date.now();
    const log = (status) => console.log(
      `${new Date().toISOString()} ${req.method} ${req.url} -> ${status} (${Date.now() - t0}ms)`
    );

    if (req.method === "GET" && req.url === "/health") {
      log(200);
      return send(res, 200, {
        ok: true,
        version: VERSION,
        groq_configured: !!GROQ_API_KEY,
        whisper_model: GROQ_API_KEY ? GROQ_WHISPER_MODEL : null,
      });
    }

    if (!authOk(req)) {
      log(401);
      return send(res, 401, "nope", "text/plain");
    }

    try {
      if (req.method === "GET" && req.url === "/tokens_today") {
        const p = await tokensToday();
        log(200);
        return send(res, 200, p);
      }
      if (req.method === "GET" && req.url === "/pet/state") {
        const p = await petState();
        log(200);
        return send(res, 200, p);
      }
      if (req.method === "POST" && req.url === "/pet/reset") {
        // Soft-reset the pet: new birth_ts, clear last_msg, zero today's
        // audio_runs_today. Does not touch the JSONL-derived token totals.
        const s = loadState();
        s.pet_birth_ts        = Math.floor(Date.now() / 1000);
        s.last_msg            = "";
        s.last_msg_ts         = 0;
        s.audio_runs_today    = 0;
        s.audio_runs_today_date = ymd(new Date());
        saveState();
        cache = { at: 0, payload: null };
        log(200);
        return send(res, 200, await petState());
      }
      if (req.method === "POST" && req.url === "/transcribe") {
        if (!GROQ_API_KEY) {
          log(503);
          return send(res, 503, { error: "groq not configured" });
        }
        const ct = (req.headers["content-type"] ?? "").toLowerCase();
        if (!ct.startsWith("audio/wav") && !ct.startsWith("application/octet-stream")) {
          log(400);
          return send(res, 400, { error: "expected Content-Type audio/wav or application/octet-stream" });
        }
        let wav;
        try {
          wav = await readBody(req, 1024 * 1024); // 1MB cap (30s @ 16kHz mono = 960KB)
        } catch (e) {
          log(413);
          return send(res, 413, { error: String(e.message ?? e) });
        }
        if (wav.length < 44) {
          log(400);
          return send(res, 400, { error: "audio body too short for WAV header" });
        }

        const tGroq = Date.now();
        const boundary = "----TG" + Math.random().toString(36).slice(2);
        const body = buildMultipart(boundary, [
          { name: "file", filename: "audio.wav", contentType: "audio/wav", value: wav },
          { name: "model", value: GROQ_WHISPER_MODEL },
          { name: "response_format", value: "json" },
        ]);
        const groqRes = await fetch(GROQ_URL, {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${GROQ_API_KEY}`,
            "Content-Type": `multipart/form-data; boundary=${boundary}`,
          },
          body,
        });
        if (!groqRes.ok) {
          const errText = await groqRes.text();
          console.error(`groq ${groqRes.status}: ${errText.slice(0, 200)}`);
          log(502);
          return send(res, 502, { error: `groq ${groqRes.status}: ${errText.slice(0, 200)}` });
        }
        const groqJson = await groqRes.json();
        const text = groqJson.text ?? "";
        recordTranscription(text);
        log(200);
        return send(res, 200, {
          text,
          duration_s: (wavDurationMs(wav) ?? 0) / 1000,
          lang: groqJson.language ?? null,
          ms_groq: Date.now() - tGroq,
        });
      }
      log(404);
      return send(res, 404, { error: "not found" });
    } catch (e) {
      console.error(e);
      log(502);
      return send(res, 502, { error: String(e) });
    }
  }).listen(PORT, () => {
    loadState(); // make sure state.json exists at boot
    console.log(`token-tamagotchi bridge v${VERSION} listening on :${PORT} (token ${DEVICE_TOKEN.slice(0, 6)}…)`);
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main();
}

// ----------------------------------------------------------------------------
//  Accuracy note: Claude's on-disk JSONL token counts are known to run low vs
//  what Claude Code tracks internally. For a "more = happier" pet that's fine.
//  If you ever want faithful numbers, feed from Claude Code's statusline payload
//  (cumulative totals piped to your statusline script) instead of the JSONL.
//  The firmware contract ({ tokens_today, ts }) stays the same either way.
// ----------------------------------------------------------------------------
