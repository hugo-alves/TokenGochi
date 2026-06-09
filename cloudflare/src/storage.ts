import { computeMood } from "./pet.ts";
import type { PetStatePayload, TokenSnapshot } from "./pet.ts";

export interface RawTokenInput {
  tokens_today: number;
  breakdown: {
    claude: number;
    codex: number;
  };
  ts?: number;
}

interface AppStateRow {
  pet_birth_ts: number;
  last_msg: string;
  last_msg_ts: number;
  total_tokens_ever: number;
  peak_today_total: number;
  peak_today_date: string;
  audio_runs_today: number;
  audio_runs_today_date: string;
}

interface TokenRow {
  tokens_today: number;
  breakdown_claude: number;
  breakdown_codex: number;
  ts: number;
}

const TOKEN_COUNTER_ID = 1;
const APP_STATE_ID = 1;

function asInt(v: unknown, fallback = 0): number {
  if (typeof v === "number" && Number.isFinite(v)) return Math.max(0, Math.floor(v));
  if (typeof v === "string") {
    const n = Number(v);
    return Number.isFinite(n) ? Math.max(0, Math.floor(n)) : fallback;
  }
  return fallback;
}

function asString(v: unknown, fallback = ""): string {
  if (typeof v === "string") return v;
  return fallback;
}

function dayId(tsSec: number): string {
  const d = new Date(tsSec * 1000);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

function toPayload(state: AppStateRow, tokens: TokenRow): PetStatePayload {
  const now = Math.floor(Date.now() / 1000);
  return {
    mood: computeMood(tokens.tokens_today, new Date(now * 1000)),
    age_s: Math.max(0, now - state.pet_birth_ts),
    food_today: tokens.tokens_today,
    last_msg: state.last_msg,
    last_msg_ts: state.last_msg_ts,
    total_tokens_ever: state.total_tokens_ever,
    audio_runs_today: state.audio_runs_today,
    breakdown: {
      claude: tokens.breakdown_claude,
      codex: tokens.breakdown_codex,
    },
    ts: tokens.ts,
  };
}

export async function ensureDefaults(db: D1Database, nowSec = Math.floor(Date.now() / 1000)): Promise<void> {
  const stateRow = await db.prepare(
    "SELECT 1 FROM app_state WHERE id = ?"
  ).bind(APP_STATE_ID).first();
  if (!stateRow) {
    const today = dayId(nowSec);
    await db.prepare(`
      INSERT INTO app_state
      (id, pet_birth_ts, last_msg, last_msg_ts, total_tokens_ever, peak_today_total, peak_today_date, audio_runs_today, audio_runs_today_date, updated_at)
      VALUES (?, ?, '', 0, 0, 0, ?, 0, ?, ?)
    `).bind(APP_STATE_ID, nowSec, today, today, nowSec).run();
  }

  const tokenRow = await db.prepare(
    "SELECT 1 FROM token_counters WHERE id = ?"
  ).bind(TOKEN_COUNTER_ID).first();
  if (!tokenRow) {
    await db.prepare(`
      INSERT INTO token_counters
      (id, tokens_today, breakdown_claude, breakdown_codex, ts, updated_at)
      VALUES (?, 0, 0, 0, ?, ?)
    `).bind(TOKEN_COUNTER_ID, nowSec, nowSec).run();
  }
}

async function getStateRow(db: D1Database): Promise<AppStateRow> {
  const row = await db.prepare(`
    SELECT
      pet_birth_ts,
      last_msg,
      last_msg_ts,
      total_tokens_ever,
      peak_today_total,
      peak_today_date,
      audio_runs_today,
      audio_runs_today_date
    FROM app_state
    WHERE id = ?
  `).bind(APP_STATE_ID).first();

  if (!row) throw new Error("app_state not initialized");
  return {
    pet_birth_ts: asInt(row["pet_birth_ts"], Math.floor(Date.now() / 1000)),
    last_msg: asString(row["last_msg"], ""),
    last_msg_ts: asInt(row["last_msg_ts"], 0),
    total_tokens_ever: asInt(row["total_tokens_ever"], 0),
    peak_today_total: asInt(row["peak_today_total"], 0),
    peak_today_date: asString(row["peak_today_date"], dayId(Math.floor(Date.now() / 1000))),
    audio_runs_today: asInt(row["audio_runs_today"], 0),
    audio_runs_today_date: asString(row["audio_runs_today_date"], dayId(Math.floor(Date.now() / 1000))),
  };
}

async function getTokenRow(db: D1Database): Promise<TokenRow> {
  const row = await db.prepare(`
    SELECT tokens_today, breakdown_claude, breakdown_codex, ts
    FROM token_counters
    WHERE id = ?
  `).bind(TOKEN_COUNTER_ID).first();
  if (!row) throw new Error("token_counters not initialized");
  return {
    tokens_today: asInt(row["tokens_today"], 0),
    breakdown_claude: asInt(row["breakdown_claude"], 0),
    breakdown_codex: asInt(row["breakdown_codex"], 0),
    ts: asInt(row["ts"], Math.floor(Date.now() / 1000)),
  };
}

export async function getTokensToday(db: D1Database): Promise<TokenSnapshot> {
  await ensureDefaults(db);
  const row = await getTokenRow(db);
  return {
    tokens_today: row.tokens_today,
    breakdown: {
      claude: row.breakdown_claude,
      codex: row.breakdown_codex,
    },
    ts: row.ts,
  };
}

export async function getPetState(db: D1Database): Promise<PetStatePayload> {
  await ensureDefaults(db);
  const [state, tokens] = await Promise.all([getStateRow(db), getTokenRow(db)]);
  return toPayload(state, tokens);
}

export async function upsertTokenSnapshot(db: D1Database, payload: RawTokenInput): Promise<TokenSnapshot> {
  await ensureDefaults(db);

  const nowSec = Math.floor(Date.now() / 1000);
  const tokensToday = Math.max(0, asInt(payload.tokens_today, 0));
  const breakdownClaude = Math.max(0, asInt(payload.breakdown?.claude, 0));
  const breakdownCodex = Math.max(0, asInt(payload.breakdown?.codex, 0));
  const nowY = dayId(nowSec);
  const ts = payload.ts ? asInt(payload.ts, nowSec) : nowSec;

  const state = await getStateRow(db);
  let peakDate = state.peak_today_date;
  let peak = state.peak_today_total;
  let totalTokensEver = state.total_tokens_ever;
  if (peakDate !== nowY) {
    peakDate = nowY;
    peak = 0;
  }

  if (tokensToday > peak) {
    totalTokensEver += tokensToday - peak;
    peak = tokensToday;
  }

  await db.batch([
    db.prepare(`
      INSERT OR REPLACE INTO token_counters
      (id, tokens_today, breakdown_claude, breakdown_codex, ts, updated_at)
      VALUES (?, ?, ?, ?, ?, ?)
    `).bind(TOKEN_COUNTER_ID, tokensToday, breakdownClaude, breakdownCodex, ts, nowSec),
    db.prepare(`
      UPDATE app_state
      SET total_tokens_ever = ?, peak_today_total = ?, peak_today_date = ?, updated_at = ?
      WHERE id = ?
    `).bind(totalTokensEver, peak, peakDate, nowSec, APP_STATE_ID),
  ]);

  return getTokensToday(db);
}

export async function recordTranscription(
  db: D1Database,
  text: string,
  durationMs: number,
  lang: string | null,
  msGroq: number
): Promise<PetStatePayload> {
  await ensureDefaults(db);

  const nowSec = Math.floor(Date.now() / 1000);
  const nowY = dayId(nowSec);
  const state = await getStateRow(db);
  const nextAudioRuns = (state.audio_runs_today_date === nowY ? state.audio_runs_today : 0) + 1;

  await db.batch([
    db.prepare(`
      INSERT INTO transcriptions
      (text, language, duration_ms, ms_groq, created_at)
      VALUES (?, ?, ?, ?, ?)
    `).bind(text.slice(0, 500), lang, durationMs, msGroq, nowSec),
    db.prepare(`
      UPDATE app_state
      SET
        last_msg = ?,
        last_msg_ts = ?,
        audio_runs_today = ?,
        audio_runs_today_date = ?,
        updated_at = ?
      WHERE id = ?
    `).bind(text.slice(0, 500), nowSec, nextAudioRuns, nowY, nowSec, APP_STATE_ID),
  ]);
  return getPetState(db);
}

export async function resetPet(db: D1Database): Promise<PetStatePayload> {
  await ensureDefaults(db);
  const nowSec = Math.floor(Date.now() / 1000);
  const today = dayId(nowSec);
  await db.prepare(`
    UPDATE app_state
    SET pet_birth_ts = ?, last_msg = '', last_msg_ts = 0, audio_runs_today = 0, audio_runs_today_date = ?, updated_at = ?
    WHERE id = ?
  `).bind(nowSec, today, nowSec, APP_STATE_ID).run();
  return getPetState(db);
}
