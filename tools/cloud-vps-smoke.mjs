#!/usr/bin/env node
// Smoke-test the VPS pull architecture without printing secrets.

import process from "node:process";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, "..");

function loadEnvFile(path) {
  if (!existsSync(path)) return;
  const text = readFileSync(path, "utf8");
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const m = line.match(/^([A-Z0-9_]+)\s*=\s*(.*?)\s*$/i);
    if (!m || process.env[m[1]] !== undefined) continue;
    process.env[m[1]] = m[2].replace(/^["']|["']$/g, "").trim();
  }
}

loadEnvFile(join(root, "bridge", ".env"));

const WORKER_URL = (process.env.CLOUDFLARE_WORKER_URL || "").replace(/\/+$/, "");
const DEVICE_TOKEN = (process.env.DEVICE_TOKEN || "").trim();
const INGEST_TOKEN = (process.env.INGEST_TOKEN || "").trim();
const TOKEN_SOURCE_URL = (process.env.TOKEN_SOURCE_URL || "").replace(/\/+$/, "");
const TOKEN_SOURCE_TOKEN = (process.env.TOKEN_SOURCE_TOKEN || "").trim();

function tokenSourceUrls(raw) {
  const url = new URL(raw);
  const parts = url.pathname.split("/").filter(Boolean);
  const isExactTokenPath = parts[parts.length - 1] === "tokens_today";
  if (isExactTokenPath) {
    const tokenUrl = url.toString();
    parts.pop();
    url.pathname = parts.length ? `/${parts.join("/")}/health` : "/health";
    return { health: url.toString(), tokens: tokenUrl };
  }

  const base = url.pathname === "/" ? "" : url.pathname.replace(/\/+$/, "");
  const health = new URL(url.toString());
  health.pathname = `${base}/health`;
  const tokens = new URL(url.toString());
  tokens.pathname = `${base}/tokens_today`;
  return { health: health.toString(), tokens: tokens.toString() };
}

function requireEnv(name, value) {
  if (value) return true;
  console.error(`missing ${name}`);
  process.exitCode = 1;
  return false;
}

async function getJson(url, token) {
  const res = await fetch(url, {
    headers: token ? { authorization: `Bearer ${token}` } : {},
  });
  const text = await res.text();
  if (!res.ok) throw new Error(`${url} -> ${res.status}: ${text.slice(0, 200)}`);
  return JSON.parse(text);
}

async function postJson(url, token) {
  const res = await fetch(url, {
    method: "POST",
    headers: token ? { authorization: `Bearer ${token}` } : {},
  });
  const text = await res.text();
  if (!res.ok) throw new Error(`${url} -> ${res.status}: ${text.slice(0, 200)}`);
  return JSON.parse(text);
}

async function main() {
  if (![
    requireEnv("CLOUDFLARE_WORKER_URL", WORKER_URL),
    requireEnv("DEVICE_TOKEN", DEVICE_TOKEN),
    requireEnv("INGEST_TOKEN", INGEST_TOKEN),
    requireEnv("TOKEN_SOURCE_URL", TOKEN_SOURCE_URL),
    requireEnv("TOKEN_SOURCE_TOKEN", TOKEN_SOURCE_TOKEN),
  ].every(Boolean)) return;

  const source = tokenSourceUrls(TOKEN_SOURCE_URL);
  const sourceHealth = await getJson(source.health);
  console.log(`source health: ok=${sourceHealth.ok === true}`);

  const sourceTokens = await getJson(source.tokens, TOKEN_SOURCE_TOKEN);
  console.log(`source tokens: total=${sourceTokens.tokens_today} codex=${sourceTokens.breakdown?.codex ?? 0} ts=${sourceTokens.ts}`);

  const workerHealth = await getJson(`${WORKER_URL}/health`);
  console.log(`worker health: ok=${workerHealth.ok === true} token_source_configured=${workerHealth.token_source_configured === true}`);

  const pulled = await postJson(`${WORKER_URL}/ingest/pull`, INGEST_TOKEN);
  console.log(`worker pull: total=${pulled.tokens?.tokens_today} codex=${pulled.tokens?.breakdown?.codex ?? 0} ts=${pulled.tokens?.ts}`);

  const state = await getJson(`${WORKER_URL}/pet/state`, DEVICE_TOKEN);
  console.log(`pet state: food_today=${state.food_today} codex=${state.breakdown?.codex ?? 0} ts=${state.ts}`);

  if (state.food_today !== sourceTokens.tokens_today || state.breakdown?.codex !== sourceTokens.breakdown?.codex) {
    throw new Error("Worker pet state does not match VPS token source after pull");
  }
}

main().catch((err) => {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
});
