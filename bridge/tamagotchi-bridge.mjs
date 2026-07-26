#!/usr/bin/env node
// ============================================================================
//  Token Tamagotchi — self-contained bridge.
//  Reads local JSONL transcripts that Claude Code and Codex CLI write to disk.
//  An unsupported, read-only Codex account usage mode is available only with
//  an explicit experimental opt-in. The bridge serves:
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
const PLACEHOLDER_DEVICE_TOKEN = "replace-with-a-random-device-token";

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
const HOST               = (process.env.HOST || "127.0.0.1").trim();
const PORT               = parseInt(process.env.PORT || "8787", 10);
const DEVICE_TOKEN       = (process.env.DEVICE_TOKEN || "").trim();
const CACHE_MS           = 30_000;  // re-scan logs at most every 30s
const INCLUDE_CACHE      = true;    // count cache read/creation tokens as "food" too
const GROQ_API_KEY       = process.env.GROQ_API_KEY || "";
const GROQ_WHISPER_MODEL = process.env.GROQ_WHISPER_MODEL || "whisper-large-v3";
const GROQ_URL           = process.env.GROQ_URL || "https://api.groq.com/openai/v1/audio/transcriptions";
const TOKEN_USAGE_SOURCE = (process.env.TOKEN_USAGE_SOURCE || "local").trim().toLowerCase();
const EXPERIMENTAL_CODEX_ACCOUNT_USAGE =
  process.env.EXPERIMENTAL_CODEX_ACCOUNT_USAGE === "1";
const CODEX_USAGE_SCALE  = Math.max(1, parseInt(process.env.CODEX_USAGE_SCALE || "1000", 10));
const VERSION            = "0.2.0";
const ACTIVITY_LOOKBACK_DAYS = 7;
const ACTIVITY_LOOKBACK_MS = ACTIVITY_LOOKBACK_DAYS * 86_400_000;

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
function clampPercent(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return null;
  return Math.max(0, Math.min(100, n));
}
function clamp(v, lower, upper) {
  const n = Number(v);
  if (!Number.isFinite(n)) return lower;
  return Math.max(lower, Math.min(upper, n));
}
function timestampMs(value) {
  if (!value) return null;
  if (typeof value === "number" && Number.isFinite(value)) {
    return value > 10_000_000_000 ? value : value * 1000;
  }
  const d = new Date(value);
  return Number.isNaN(d.getTime()) ? null : d.getTime();
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
export function computeMood(foodToday, now = new Date(), usage = null) {
  const pace = usage?.codex?.pace || null;
  const balanceKind = pace?.balance_kind || paceKindForStage(pace?.stage);
  if (balanceKind === "reserve") return "very hungry";
  if (balanceKind === "deficit") return "very happy";

  const activityStage = usage?.activity?.stage || "unknown";
  if (activityStage === "grumpy" || activityStage === "very_grumpy") return "grumpy";
  if (balanceKind === "on_pace") return "happy";

  if (activityStage && activityStage !== "unknown") {
    if (foodToday < 5_000) return "hungry";
    return "happy";
  }

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

export function activityStage(idleSeconds) {
  const n = Number(idleSeconds);
  if (!Number.isFinite(n) || n < 0) return "unknown";
  if (n < 30 * 60) return "awake";
  if (n < 90 * 60) return "restless";
  if (n < 180 * 60) return "grumpy";
  return "very_grumpy";
}

export function activityMetadata(lastActiveMs, nowMs = Date.now()) {
  const last = Number(lastActiveMs);
  if (!Number.isFinite(last) || last <= 0) {
    return {
      source: "local_logs",
      last_active_ts: null,
      idle_seconds: null,
      stage: "unknown",
    };
  }

  const idleSeconds = Math.max(0, Math.floor((nowMs - last) / 1000));
  return {
    source: "local_logs",
    last_active_ts: Math.floor(last / 1000),
    idle_seconds: idleSeconds,
    stage: activityStage(idleSeconds),
  };
}

export function latestActivityMs(candidates) {
  let latest = null;
  for (const value of candidates || []) {
    const ms = timestampMs(value);
    if (ms == null) continue;
    if (latest == null || ms > latest) latest = ms;
  }
  return latest;
}

function isClaudeUsageRecord(o) {
  return !!o?.message?.usage;
}

function isCodexTokenCountRecord(o) {
  const payload = o?.payload ?? o;
  return payload?.type === "token_count" || o?.type === "token_count";
}

function recordTimestampMs(o, fallbackMs = null) {
  const payload = o?.payload ?? o;
  return timestampMs(o?.timestamp ?? payload?.timestamp) ?? fallbackMs;
}

async function latestClaudeActivityMs(nowMs = Date.now()) {
  const cutoff = nowMs - ACTIVITY_LOOKBACK_MS;
  let latest = null;
  for (const root of claudeRoots()) {
    for await (const file of walk(root)) {
      let fileStat;
      try {
        fileStat = await stat(file);
      } catch {
        continue;
      }
      if (fileStat.mtimeMs < cutoff) continue;

      for await (const line of streamLines(file)) {
        let o;
        try {
          o = JSON.parse(line);
        } catch {
          continue;
        }
        if (!isClaudeUsageRecord(o)) continue;
        const ts = recordTimestampMs(o, fileStat.mtimeMs);
        if (ts == null || ts < cutoff) continue;
        if (latest == null || ts > latest) latest = ts;
      }
    }
  }
  return latest;
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

function primaryCodexHome() {
  return codexHomes()[0] || join(homedir(), ".codex");
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

async function* codexActivityFiles(nowMs = Date.now()) {
  for (const home of codexHomes()) {
    for (let daysBack = 0; daysBack < ACTIVITY_LOOKBACK_DAYS; daysBack += 1) {
      const d = new Date(nowMs - daysBack * 86_400_000);
      const dir = join(home, "sessions", String(d.getFullYear()), pad(d.getMonth() + 1), pad(d.getDate()));
      let entries;
      try {
        entries = await readdir(dir, { withFileTypes: true });
      } catch {
        continue;
      }
      for (const e of entries) {
        if (e.isFile() && e.name.startsWith("rollout-") && e.name.endsWith(".jsonl")) {
          yield join(dir, e.name);
        }
      }
    }
  }
}

async function latestCodexActivityMs(nowMs = Date.now()) {
  const cutoff = nowMs - ACTIVITY_LOOKBACK_MS;
  let latest = null;
  for await (const file of codexActivityFiles(nowMs)) {
    let fileStat;
    try {
      fileStat = await stat(file);
    } catch {
      continue;
    }
    if (fileStat.mtimeMs < cutoff) continue;

    for await (const line of streamLines(file)) {
      let o;
      try {
        o = JSON.parse(line);
      } catch {
        continue;
      }
      if (!isCodexTokenCountRecord(o)) continue;
      const ts = recordTimestampMs(o, fileStat.mtimeMs);
      if (ts == null || ts < cutoff) continue;
      if (latest == null || ts > latest) latest = ts;
    }
  }
  return latest;
}

async function computeActivity(nowMs = Date.now()) {
  const [claude, codex] = await Promise.all([
    latestClaudeActivityMs(nowMs),
    latestCodexActivityMs(nowMs),
  ]);
  return activityMetadata(latestActivityMs([claude, codex]), nowMs);
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

// ----------------------------------------------------------- Codex account ---
// Experimental and unsupported. This mode reads an existing access token from
// ~/.codex/auth.json but never refreshes, rewrites, or exports that file.
// The endpoint is not a public API and may change without notice.
const CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage";

function readCodexAuth() {
  const authPath = join(primaryCodexHome(), "auth.json");
  let json;
  try {
    json = JSON.parse(readFileSync(authPath, "utf8"));
  } catch (err) {
    throw new Error(`Codex auth.json unavailable at ${authPath}: ${err.message || err}`);
  }
  const tokens = json.tokens || {};
  const accessToken = tokens.access_token || tokens.accessToken || json.OPENAI_API_KEY || "";
  const accountId = tokens.account_id || tokens.accountId || "";
  if (!accessToken) throw new Error("Codex auth.json has no access token");
  return { accessToken, accountId };
}

export function paceStage(deltaPercent) {
  const delta = Number(deltaPercent);
  if (!Number.isFinite(delta)) return null;
  const abs = Math.abs(delta);
  if (abs <= 2) return "on_track";
  if (abs <= 6) return delta >= 0 ? "slightly_ahead" : "slightly_behind";
  if (abs <= 12) return delta >= 0 ? "ahead" : "behind";
  return delta >= 0 ? "far_ahead" : "far_behind";
}

function paceKindForStage(stage) {
  if (!stage) return null;
  if (stage === "on_track") return "on_pace";
  if (stage === "slightly_ahead" || stage === "ahead" || stage === "far_ahead") return "deficit";
  if (stage === "slightly_behind" || stage === "behind" || stage === "far_behind") return "reserve";
  return null;
}

export function paceBalance(deltaPercent, stage = paceStage(deltaPercent)) {
  const kind = paceKindForStage(stage);
  if (!kind) return null;
  if (kind === "on_pace") {
    return { kind, percent: 0, label: "on pace" };
  }

  const delta = Number(deltaPercent);
  const percent = Number.isFinite(delta) ? Math.round(Math.abs(delta) * 10) / 10 : 0;
  const displayPercent = Math.round(percent);
  return {
    kind,
    percent,
    label: `${displayPercent}% ${kind}`,
  };
}

export function accountUsagePace(window, nowMs = Date.now()) {
  if (!window || typeof window !== "object") return null;
  const actual = clampPercent(window.used_percent);
  const resetAt = Number(window.reset_at);
  const duration = Number(window.limit_window_seconds);
  if (actual == null || !Number.isFinite(resetAt) || !Number.isFinite(duration) || duration <= 0) return null;

  const nowSec = nowMs / 1000;
  const timeUntilReset = resetAt - nowSec;
  if (timeUntilReset <= 0 || timeUntilReset > duration) return null;

  const elapsed = clamp(duration - timeUntilReset, 0, duration);
  if (elapsed === 0 && actual > 0) return null;
  const expected = clamp((elapsed / duration) * 100, 0, 100);
  const delta = actual - expected;

  let etaSeconds = null;
  let willLastToReset = false;
  if (elapsed > 0 && actual > 0) {
    const rate = actual / elapsed;
    if (rate > 0) {
      const candidate = Math.max(0, 100 - actual) / rate;
      if (candidate >= timeUntilReset) willLastToReset = true;
      else etaSeconds = Math.max(0, Math.round(candidate));
    }
  } else if (elapsed > 0 && actual === 0) {
    willLastToReset = true;
  }

  const stage = paceStage(delta);
  const balance = paceBalance(delta, stage);
  return {
    window: "weekly",
    stage,
    delta_percent: Math.round(delta * 10) / 10,
    expected_used_percent: Math.round(expected * 10) / 10,
    actual_used_percent: Math.round(actual * 10) / 10,
    balance_kind: balance?.kind ?? null,
    balance_percent: balance?.percent ?? null,
    balance_label: balance?.label ?? null,
    eta_seconds: etaSeconds,
    will_last_to_reset: willLastToReset,
  };
}

export function accountUsageMetadata(usage) {
  const primary = usage?.rate_limit?.primary_window || null;
  const secondary = usage?.rate_limit?.secondary_window || null;
  const primaryPercent = clampPercent(primary?.used_percent);
  const secondaryPercent = clampPercent(secondary?.used_percent);
  const metricPercent = secondaryPercent ?? primaryPercent ?? 0;
  const additional = Array.isArray(usage?.additional_rate_limits)
    ? usage.additional_rate_limits.map((item) => ({
        limit_name: item?.limit_name || null,
        metered_feature: item?.metered_feature || null,
        primary_used_percent: clampPercent(item?.rate_limit?.primary_window?.used_percent),
        secondary_used_percent: clampPercent(item?.rate_limit?.secondary_window?.used_percent),
        primary_reset_at: item?.rate_limit?.primary_window?.reset_at || null,
        secondary_reset_at: item?.rate_limit?.secondary_window?.reset_at || null,
      }))
    : [];
  return {
    source: "codex_account",
    plan_type: usage?.plan_type || null,
    primary_used_percent: primaryPercent,
    secondary_used_percent: secondaryPercent,
    metric_used_percent: metricPercent,
    metric_window: secondaryPercent != null ? "weekly" : (primaryPercent != null ? "session" : "none"),
    primary_reset_at: primary?.reset_at || null,
    secondary_reset_at: secondary?.reset_at || null,
    pace: accountUsagePace(secondary),
    synthetic_tokens_per_percent: CODEX_USAGE_SCALE,
    additional_rate_limits: additional,
  };
}

async function computeCodexAccountUsage() {
  if (!EXPERIMENTAL_CODEX_ACCOUNT_USAGE) {
    throw new Error(
      "Codex account usage is experimental; set EXPERIMENTAL_CODEX_ACCOUNT_USAGE=1 to opt in"
    );
  }
  const credentials = readCodexAuth();
  const headers = {
    authorization: `Bearer ${credentials.accessToken}`,
    accept: "application/json",
    "user-agent": "TokenGochi",
  };
  if (credentials.accountId) headers["ChatGPT-Account-Id"] = credentials.accountId;

  const res = await fetch(CODEX_USAGE_URL, { headers });
  const text = await res.text();
  if (!res.ok) throw new Error(`Codex usage API failed ${res.status}`);

  let usage;
  try {
    usage = JSON.parse(text);
  } catch {
    throw new Error("Codex usage API returned invalid JSON");
  }

  const metadata = accountUsageMetadata(usage);
  const codex = Math.round(metadata.metric_used_percent * CODEX_USAGE_SCALE);
  return {
    codex,
    metadata,
  };
}

// ------------------------------------------------------------- aggregate -----
let cache = { at: 0, payload: null };

async function tokensToday() {
  if (cache.payload && Date.now() - cache.at < CACHE_MS) return cache.payload;

  const seen = new Set();
  const claudePromise = computeClaude(seen);
  const activityPromise = computeActivity();
  let codexPromise;
  if (TOKEN_USAGE_SOURCE === "codex-account" || TOKEN_USAGE_SOURCE === "account") {
    codexPromise = computeCodexAccountUsage();
  } else if (TOKEN_USAGE_SOURCE === "local") {
    codexPromise = computeCodex().then((codex) => ({ codex, metadata: { source: "local_logs" } }));
  } else {
    throw new Error(
      `unsupported TOKEN_USAGE_SOURCE=${JSON.stringify(TOKEN_USAGE_SOURCE)}; use "local" or "codex-account"`
    );
  }
  const [claude, codexResult, activity] = await Promise.all([claudePromise, codexPromise, activityPromise]);
  const codex = codexResult.codex;
  const payload = {
    tokens_today: claude + codex,
    breakdown: { claude, codex },
    usage: {
      source: codexResult.metadata?.source || "local_logs",
      codex: codexResult.metadata,
      activity,
    },
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
    mood: computeMood(t.tokens_today, new Date(), t.usage),
    age_s: Math.max(0, Math.floor(Date.now() / 1000) - s.pet_birth_ts),
    food_today: t.tokens_today,
    last_msg: s.last_msg,
    last_msg_ts: s.last_msg_ts,
    total_tokens_ever: s.total_tokens_ever,
    audio_runs_today: s.audio_runs_today,
    breakdown: t.breakdown,
    usage: t.usage,
    ts: t.ts,
  };
}

// ---------------------------------------------------------------- http --------
function authOk(req) {
  return (req.headers.authorization ?? "") === `Bearer ${DEVICE_TOKEN}`;
}
export function isValidDeviceToken(value) {
  const token = String(value ?? "").trim();
  return token.length >= 32 && token !== PLACEHOLDER_DEVICE_TOKEN;
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
      pet: { ...s, mood: computeMood(t.tokens_today, new Date(), t.usage) },
    }, null, 2));
    return;
  }

  if (!isValidDeviceToken(DEVICE_TOKEN)) {
    console.error(
      "DEVICE_TOKEN must be a non-placeholder secret of at least 32 characters; see bridge/.env.example"
    );
    process.exitCode = 1;
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
        const traceId = Math.random().toString(36).slice(2, 10);
        if (!GROQ_API_KEY) {
          console.warn(`[transcribe:${traceId}] reject reason=groq_not_configured`);
          log(503);
          return send(res, 503, { error: "groq not configured" });
        }
        const ct = (req.headers["content-type"] ?? "").toLowerCase();
        if (!ct.startsWith("audio/wav") && !ct.startsWith("application/octet-stream")) {
          console.warn(`[transcribe:${traceId}] reject status=400 content_type=${JSON.stringify(ct)}`);
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
          console.warn(`[transcribe:${traceId}] reject status=400 bytes=${wav.length}`);
          log(400);
          return send(res, 400, { error: "audio body too short for WAV header" });
        }

        const durationMs = wavDurationMs(wav) ?? 0;
        console.log(`[transcribe:${traceId}] recv content_type=${JSON.stringify(ct)} bytes=${wav.length} duration_ms=${durationMs}`);
        const tGroq = Date.now();
        const boundary = "----TG" + Math.random().toString(36).slice(2);
        const body = buildMultipart(boundary, [
          { name: "file", filename: "audio.wav", contentType: "audio/wav", value: wav },
          { name: "model", value: GROQ_WHISPER_MODEL },
          { name: "response_format", value: "json" },
        ]);
        console.log(`[transcribe:${traceId}] send groq model=${JSON.stringify(GROQ_WHISPER_MODEL)} wav_bytes=${wav.length} multipart_bytes=${body.length}`);
        const groqRes = await fetch(GROQ_URL, {
          method: "POST",
          headers: {
            "Authorization": `Bearer ${GROQ_API_KEY}`,
            "Content-Type": `multipart/form-data; boundary=${boundary}`,
          },
          body,
        });
        if (!groqRes.ok) {
          await groqRes.arrayBuffer();
          console.error(`[transcribe:${traceId}] recv groq status=${groqRes.status}`);
          log(502);
          return send(res, 502, { error: `groq ${groqRes.status}` });
        }
        const groqJson = await groqRes.json();
        const text = groqJson.text ?? "";
        const msGroq = Date.now() - tGroq;
        console.log(
          `[transcribe:${traceId}] recv groq ok text_len=${String(text).length}` +
          ` lang=${JSON.stringify(groqJson.language ?? null)} duration_ms=${durationMs}` +
          ` ms_groq=${msGroq}`
        );
        recordTranscription(text);
        log(200);
        return send(res, 200, {
          text,
          duration_s: durationMs / 1000,
          lang: groqJson.language ?? null,
          ms_groq: msGroq,
        });
      }
      log(404);
      return send(res, 404, { error: "not found" });
    } catch (e) {
      console.error(e instanceof Error ? e.message : String(e));
      log(502);
      return send(res, 502, { error: "bridge request failed" });
    }
  }).listen(PORT, HOST, () => {
    loadState(); // make sure state.json exists at boot
    console.log(`token-tamagotchi bridge v${VERSION} listening on ${HOST}:${PORT}`);
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
