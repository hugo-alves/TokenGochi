#!/usr/bin/env node
// ============================================================================
//  synth-pet-state.mjs — synthetic bridge for firmware dev
//
//  Same endpoints as the real bridge (/health, /pet/state, /tokens_today,
//  /transcribe) but the values come from CLI flags. No JSONL walks, no
//  Groq, no state.json — just a stable, known response you can iterate
//  the firmware UI against without touching the real bridge.
//
//  Usage:
//    node tools/synth-pet-state.mjs [options]
//
//  Options:
//    --host 127.0.0.1    bind address (default loopback)
//    --port 8788          port to listen on (default 8788, avoid clash w/ real)
//    --token ...          bearer token (default is local-development only)
//    --mood happy         initial mood; overridden by --cycle
//    --food 10000         food_today
//    --age 3600           age in seconds since pet "birth"
//    --msg "hello"        last_msg
//    --total 0            total_tokens_ever
//    --audio 0            audio_runs_today
//    --cycle 5000         auto-rotate mood every N ms (happy→hungry→sleepy→sick)
//    --transcribe-text T  canned text to return from /transcribe
//
//  All flags optional. Drop the --cycle to keep mood fixed.
// ============================================================================

import { createServer } from "node:http";

const args = parseArgs(process.argv.slice(2));
const HOST          = args.host ?? "127.0.0.1";
const PORT          = parseInt(args.port ?? "8788", 10);
const TOKEN         = args.token ?? "local-dev-device-token-not-for-production-0001";
const FIXED_MOOD    = args.mood ?? "happy";
const FOOD          = parseInt(args.food ?? "10000", 10);
const AGE_S         = parseInt(args.age ?? "3600", 10);
const MSG           = args.msg ?? "";
const TOTAL         = parseInt(args.total ?? "0", 10);
const AUDIO         = parseInt(args.audio ?? "0", 10);
const CYCLE_MS      = parseInt(args.cycle ?? "0", 10);
const TRANSCRIBE_T  = args["transcribe-text"] ?? `synthetic transcript (mood=${FIXED_MOOD})`;

const MOODS = ["happy", "hungry", "sleepy", "sick"];
const PET_BIRTH_TS = Math.floor(Date.now() / 1000) - AGE_S;

// Mutable across requests (e.g. /transcribe can update last_msg)
let liveMsg = MSG;
let liveMsgTs = MSG ? Math.floor(Date.now() / 1000) : 0;
let liveAudio = AUDIO;

function currentMood() {
  if (!CYCLE_MS) return FIXED_MOOD;
  const i = Math.floor(Date.now() / CYCLE_MS) % MOODS.length;
  return MOODS[i];
}

function authOk(req) {
  return (req.headers.authorization ?? "") === `Bearer ${TOKEN}`;
}

function send(res, status, body) {
  res.writeHead(status, { "content-type": "application/json" });
  res.end(JSON.stringify(body));
}

function petState() {
  return {
    mood:              currentMood(),
    age_s:             Math.max(0, Math.floor(Date.now() / 1000) - PET_BIRTH_TS),
    food_today:        FOOD,
    last_msg:          liveMsg,
    last_msg_ts:       liveMsgTs,
    total_tokens_ever: TOTAL,
    audio_runs_today:  liveAudio,
    breakdown: {
      claude: Math.floor(FOOD * 0.7),
      codex:  Math.floor(FOOD * 0.3),
    },
    ts: Math.floor(Date.now() / 1000),
  };
}

function tokensToday() {
  return {
    tokens_today: FOOD,
    breakdown: {
      claude: Math.floor(FOOD * 0.7),
      codex:  Math.floor(FOOD * 0.3),
    },
    ts: Math.floor(Date.now() / 1000),
  };
}

createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    return send(res, 200, { ok: true, synthetic: true, version: "0.1.0" });
  }
  if (!authOk(req)) {
    res.writeHead(401, { "content-type": "text/plain" });
    return res.end("nope");
  }

  if (req.method === "GET" && req.url === "/pet/state")   return send(res, 200, petState());
  if (req.method === "GET" && req.url === "/tokens_today") return send(res, 200, tokensToday());

  if (req.method === "POST" && req.url === "/transcribe") {
    // drain body, return canned transcript, update state
    req.on("data", () => {});
    req.on("end", () => {
      liveMsg   = TRANSCRIBE_T;
      liveMsgTs = Math.floor(Date.now() / 1000);
      liveAudio += 1;
      send(res, 200, { text: TRANSCRIBE_T, duration_s: 1.0, lang: "en", ms_groq: 0 });
    });
    return;
  }

  send(res, 404, { error: "not found" });
}).listen(PORT, HOST, () => {
  const moodDesc = CYCLE_MS ? `cycling every ${CYCLE_MS}ms` : `fixed=${FIXED_MOOD}`;
  console.log(`synthetic bridge on ${HOST}:${PORT} — ${moodDesc}, food=${FOOD}, age=${AGE_S}s`);
  console.log(`  try: curl -H 'Authorization: Bearer <token>' http://${HOST}:${PORT}/pet/state`);
});

// --- arg parser (no deps) --------------------------------------------------
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
