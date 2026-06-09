import { isAuthorized } from "./auth.ts";
import type { TokenSnapshot } from "./pet.ts";
import { getPetState, getTokensToday, upsertTokenSnapshot, resetPet, recordTranscription } from "./storage.ts";
import { transcribeAudio } from "./groq.ts";

export interface Env {
  DB: D1Database;
  DEVICE_TOKEN: string;
  INGEST_TOKEN: string;
  GROQ_API_KEY: string;
  GROQ_URL: string;
  GROQ_WHISPER_MODEL: string;
}

const WORKER_VERSION = "0.1.0";
const MAX_AUDIO_BYTES = 1 * 1024 * 1024;

function jsonResponse(payload: unknown, status = 200): Response {
  return new Response(JSON.stringify(payload), {
    status,
    headers: {
      "content-type": "application/json",
      "cache-control": "no-store",
    },
  });
}

function textResponse(text: string, status = 401): Response {
  return new Response(text, { status, headers: { "content-type": "text/plain" }});
}

function normalizePath(pathname: string): string {
  if (!pathname || pathname === "/") return "/";
  return pathname.endsWith("/") ? pathname.slice(0, -1) : pathname;
}

async function readJsonBody<T>(req: Request): Promise<T | null> {
  try {
    return (await req.json()) as T;
  } catch {
    return null;
  }
}

function normalizeSnapshot(payload: { tokens_today?: unknown; breakdown?: unknown; ts?: unknown } | null): TokenSnapshot | null {
  if (!payload) return null;
  const tokensToday = typeof payload.tokens_today === "number" ? Math.max(0, Math.floor(payload.tokens_today)) : NaN;
  const breakdown = (payload.breakdown && typeof payload.breakdown === "object") ? payload.breakdown as Record<string, unknown> : null;
  const claude = breakdown && typeof breakdown.claude === "number" ? Math.max(0, Math.floor(breakdown.claude)) : NaN;
  const codex = breakdown && typeof breakdown.codex === "number" ? Math.max(0, Math.floor(breakdown.codex)) : NaN;
  const ts = typeof payload.ts === "number" ? Math.max(0, Math.floor(payload.ts)) : Math.floor(Date.now() / 1000);

  if (!Number.isFinite(tokensToday) || !Number.isFinite(claude) || !Number.isFinite(codex)) return null;
  return { tokens_today: tokensToday, breakdown: { claude, codex }, ts };
}

interface AudioReadResult {
  ok: boolean;
  body?: ArrayBuffer;
  tooLarge?: boolean;
}

async function readAudio(req: Request): Promise<AudioReadResult> {
  const ct = (req.headers.get("content-type") ?? "").toLowerCase();
  if (!ct.startsWith("audio/wav") && !ct.startsWith("application/octet-stream")) {
    return { ok: false };
  }
  const body = await req.arrayBuffer();
  if (!body || body.byteLength === 0) return { ok: false };
  if (body.byteLength > MAX_AUDIO_BYTES) return { ok: false, tooLarge: true };
  return { ok: true, body };
}

async function onWatchGetState(env: Env): Promise<Response> {
  const state = await getPetState(env.DB);
  return jsonResponse(state);
}

async function onWatchGetTokens(env: Env): Promise<Response> {
  const tokens = await getTokensToday(env.DB);
  return jsonResponse(tokens);
}

async function onWatchReset(env: Env): Promise<Response> {
  const state = await resetPet(env.DB);
  return jsonResponse(state);
}

async function onWatchTranscribe(req: Request, env: Env): Promise<Response> {
  if (!env.GROQ_API_KEY) return jsonResponse({ error: "groq not configured" }, 503);
  const wav = await readAudio(req);
  if (!wav.ok) {
    const code = wav.tooLarge ? 413 : 400;
    return jsonResponse({ error: "expected Content-Type audio/wav and body ≤1MB" }, code);
  }

  let transcription;
  try {
    transcription = await transcribeAudio(
      env.GROQ_URL || "https://api.groq.com/openai/v1/audio/transcriptions",
      env.GROQ_API_KEY,
      env.GROQ_WHISPER_MODEL || "whisper-large-v3-turbo",
      wav.body!
    );
  } catch (err) {
    return jsonResponse({ error: String((err as Error).message || "groq failure") }, 502);
  }

  await recordTranscription(
    env.DB,
    transcription.text,
    transcription.durationMs,
    transcription.lang,
    transcription.msGroq
  );
  return jsonResponse({
    text: transcription.text,
    duration_s: transcription.durationMs / 1000,
    lang: transcription.lang,
    ms_groq: transcription.msGroq,
  });
}

async function onIngest(req: Request, env: Env): Promise<Response> {
  const payload = await readJsonBody(req);
  const snapshot = normalizeSnapshot(payload as { tokens_today?: unknown; breakdown?: unknown; ts?: unknown });
  if (!snapshot) return jsonResponse({ error: "invalid payload" }, 400);

  const stored = await upsertTokenSnapshot(env.DB, snapshot);
  return jsonResponse({ ok: true, tokens: stored, state: await getPetState(env.DB) });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = normalizePath(url.pathname);
    const method = request.method.toUpperCase();

    if (path === "/health" && method === "GET") {
      return jsonResponse({
        ok: true,
        version: WORKER_VERSION,
        groq_configured: Boolean(env.GROQ_API_KEY),
        whisper_model: env.GROQ_WHISPER_MODEL || "whisper-large-v3-turbo",
      });
    }

    if (path === "/ingest/tokens" && method === "POST") {
      if (!isAuthorized(request, env.INGEST_TOKEN)) return textResponse("nope", 401);
      return onIngest(request, env);
    }

    if (!isAuthorized(request, env.DEVICE_TOKEN)) return textResponse("nope", 401);

    switch (path) {
      case "/tokens_today":
        if (method !== "GET") return textResponse("method not allowed", 405);
        return onWatchGetTokens(env);
      case "/pet/state":
        if (method !== "GET") return textResponse("method not allowed", 405);
        return onWatchGetState(env);
      case "/pet/reset":
        if (method !== "POST") return textResponse("method not allowed", 405);
        return onWatchReset(env);
      case "/transcribe":
        if (method !== "POST") return textResponse("method not allowed", 405);
        return onWatchTranscribe(request, env);
      default:
        return jsonResponse({ error: "not found" }, 404);
    }
  },
};
