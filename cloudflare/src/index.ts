import { isAuthorized } from "./auth.ts";
import type { TokenSnapshot } from "./pet.ts";
import { getPetState, getTokensToday, upsertTokenSnapshot, resetPet, recordTranscription } from "./storage.ts";
import { transcribeAudio, wavDurationMs } from "./groq.ts";

export interface Env {
  DB: D1Database;
  DEVICE_TOKEN: string;
  INGEST_TOKEN: string;
  TOKEN_SOURCE_URL: string;
  TOKEN_SOURCE_TOKEN: string;
  TOKEN_SOURCE_MAX_AGE_SECONDS: string;
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

function normalizeSnapshot(payload: { tokens_today?: unknown; breakdown?: unknown; usage?: unknown; ts?: unknown } | null): TokenSnapshot | null {
  if (!payload) return null;
  const tokensToday = typeof payload.tokens_today === "number" ? Math.max(0, Math.floor(payload.tokens_today)) : NaN;
  const breakdown = (payload.breakdown && typeof payload.breakdown === "object") ? payload.breakdown as Record<string, unknown> : null;
  const claude = breakdown && typeof breakdown.claude === "number" ? Math.max(0, Math.floor(breakdown.claude)) : NaN;
  const codex = breakdown && typeof breakdown.codex === "number" ? Math.max(0, Math.floor(breakdown.codex)) : NaN;
  const usage = payload.usage && typeof payload.usage === "object" ? payload.usage as TokenSnapshot["usage"] : null;
  const ts = typeof payload.ts === "number" ? Math.max(0, Math.floor(payload.ts)) : Math.floor(Date.now() / 1000);

  if (!Number.isFinite(tokensToday) || !Number.isFinite(claude) || !Number.isFinite(codex)) return null;
  return { tokens_today: tokensToday, breakdown: { claude, codex }, usage, ts };
}

function sourceMaxAgeSeconds(env: Env): number {
  const parsed = Number.parseInt(env.TOKEN_SOURCE_MAX_AGE_SECONDS || "75", 10);
  return Number.isFinite(parsed) ? Math.max(15, parsed) : 75;
}

function tokenSourceConfigured(env: Env): boolean {
  return Boolean(env.TOKEN_SOURCE_URL && env.TOKEN_SOURCE_TOKEN);
}

function previewText(value: string, maxLength = 160): string {
  const singleLine = value.replace(/\s+/g, " ").trim();
  return singleLine.length > maxLength ? `${singleLine.slice(0, maxLength)}...` : singleLine;
}

async function fetchTokenSource(env: Env): Promise<TokenSnapshot> {
  if (!tokenSourceConfigured(env)) throw new Error("token source not configured");

  const configured = new URL(env.TOKEN_SOURCE_URL);
  if (configured.pathname === "/" || configured.pathname === "") {
    configured.pathname = "/tokens_today";
  }
  const res = await fetch(configured.toString(), {
    method: "GET",
    headers: {
      "authorization": `Bearer ${env.TOKEN_SOURCE_TOKEN}`,
      "accept": "application/json",
    },
  });
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`token source ${res.status}: ${text.slice(0, 200)}`);
  }

  let payload: unknown;
  try {
    payload = JSON.parse(text);
  } catch {
    throw new Error("token source returned invalid JSON");
  }

  const candidate = (payload && typeof payload === "object" && "tokens" in payload)
    ? (payload as { tokens?: unknown }).tokens
    : payload;
  const snapshot = normalizeSnapshot(candidate as { tokens_today?: unknown; breakdown?: unknown; usage?: unknown; ts?: unknown } | null);
  if (!snapshot) throw new Error("token source returned invalid snapshot");
  return snapshot;
}

async function refreshFromTokenSource(env: Env): Promise<TokenSnapshot> {
  const snapshot = await fetchTokenSource(env);
  return upsertTokenSnapshot(env.DB, snapshot);
}

async function maybeRefreshFromTokenSource(env: Env): Promise<void> {
  if (!tokenSourceConfigured(env)) return;
  const current = await getTokensToday(env.DB);
  const age = Math.floor(Date.now() / 1000) - current.ts;
  if (age < sourceMaxAgeSeconds(env)) return;
  try {
    await refreshFromTokenSource(env);
  } catch (err) {
    console.error(`token source refresh failed: ${String((err as Error).message || err)}`);
  }
}

async function queueTokenSourceRefresh(env: Env, ctx: ExecutionContext): Promise<void> {
  if (!tokenSourceConfigured(env)) return;
  const current = await getTokensToday(env.DB);
  const age = Math.floor(Date.now() / 1000) - current.ts;
  if (age < sourceMaxAgeSeconds(env)) return;
  ctx.waitUntil(refreshFromTokenSource(env).catch((err) => {
    console.error(`background token source refresh failed: ${String((err as Error).message || err)}`);
  }));
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

async function onWatchGetState(env: Env, ctx: ExecutionContext): Promise<Response> {
  await queueTokenSourceRefresh(env, ctx);
  const state = await getPetState(env.DB);
  return jsonResponse(state);
}

async function onWatchGetTokens(env: Env, ctx: ExecutionContext): Promise<Response> {
  await queueTokenSourceRefresh(env, ctx);
  const tokens = await getTokensToday(env.DB);
  return jsonResponse(tokens);
}

async function onWatchReset(env: Env): Promise<Response> {
  const state = await resetPet(env.DB);
  return jsonResponse(state);
}

async function onWatchTranscribe(req: Request, env: Env): Promise<Response> {
  const traceId = crypto.randomUUID();
  const contentType = req.headers.get("content-type") ?? "";

  if (!env.GROQ_API_KEY) {
    console.warn(`[transcribe:${traceId}] reject reason=groq_not_configured`);
    return jsonResponse({ error: "groq not configured" }, 503);
  }

  const wav = await readAudio(req);
  if (!wav.ok) {
    const code = wav.tooLarge ? 413 : 400;
    console.warn(`[transcribe:${traceId}] reject status=${code} content_type=${JSON.stringify(contentType)}`);
    return jsonResponse({ error: "expected Content-Type audio/wav and body ≤1MB" }, code);
  }

  const bytes = wav.body!.byteLength;
  const durationMs = wavDurationMs(wav.body!) ?? 0;
  const model = env.GROQ_WHISPER_MODEL || "whisper-large-v3";
  console.log(`[transcribe:${traceId}] recv content_type=${JSON.stringify(contentType)} bytes=${bytes} duration_ms=${durationMs}`);
  console.log(`[transcribe:${traceId}] send groq model=${JSON.stringify(model)} bytes=${bytes}`);

  let transcription;
  try {
    transcription = await transcribeAudio(
      env.GROQ_URL || "https://api.groq.com/openai/v1/audio/transcriptions",
      env.GROQ_API_KEY,
      model,
      wav.body!
    );
  } catch (err) {
    console.error(`[transcribe:${traceId}] recv groq error=${JSON.stringify(String((err as Error).message || err))}`);
    return jsonResponse({ error: String((err as Error).message || "groq failure") }, 502);
  }

  console.log(
    `[transcribe:${traceId}] recv groq ok text_len=${transcription.text.length}` +
    ` lang=${JSON.stringify(transcription.lang)} duration_ms=${transcription.durationMs}` +
    ` ms_groq=${transcription.msGroq} text_preview=${JSON.stringify(previewText(transcription.text))}`
  );

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
  const snapshot = normalizeSnapshot(payload as { tokens_today?: unknown; breakdown?: unknown; usage?: unknown; ts?: unknown });
  if (!snapshot) return jsonResponse({ error: "invalid payload" }, 400);

  const stored = await upsertTokenSnapshot(env.DB, snapshot);
  return jsonResponse({ ok: true, tokens: stored, state: await getPetState(env.DB) });
}

async function onPullIngest(env: Env): Promise<Response> {
  const stored = await refreshFromTokenSource(env);
  return jsonResponse({ ok: true, tokens: stored, state: await getPetState(env.DB) });
}

export default {
  async scheduled(_event: ScheduledEvent, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil((async () => {
      if (!tokenSourceConfigured(env)) return;
      try {
        await refreshFromTokenSource(env);
      } catch (err) {
        console.error(`scheduled token source refresh failed: ${String((err as Error).message || err)}`);
      }
    })());
  },

  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);
    const path = normalizePath(url.pathname);
    const method = request.method.toUpperCase();

    if (path === "/health" && method === "GET") {
      return jsonResponse({
        ok: true,
        version: WORKER_VERSION,
        groq_configured: Boolean(env.GROQ_API_KEY),
        whisper_model: env.GROQ_WHISPER_MODEL || "whisper-large-v3",
        token_source_configured: tokenSourceConfigured(env),
      });
    }

    if (path === "/ingest/tokens" && method === "POST") {
      if (!isAuthorized(request, env.INGEST_TOKEN)) return textResponse("nope", 401);
      return onIngest(request, env);
    }

    if (path === "/ingest/pull" && method === "POST") {
      if (!isAuthorized(request, env.INGEST_TOKEN)) return textResponse("nope", 401);
      return onPullIngest(env);
    }

    if (!isAuthorized(request, env.DEVICE_TOKEN)) return textResponse("nope", 401);

    switch (path) {
      case "/tokens_today":
        if (method !== "GET") return textResponse("method not allowed", 405);
        return onWatchGetTokens(env, ctx);
      case "/pet/state":
        if (method !== "GET") return textResponse("method not allowed", 405);
        return onWatchGetState(env, ctx);
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
