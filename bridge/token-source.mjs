#!/usr/bin/env node
// VPS token source for Cloudflare pull mode.
//
// Exposes the same token snapshot shape used by /ingest/tokens:
//   GET /tokens_today -> { tokens_today, breakdown: { claude, codex }, ts }
//
// Run this on the machine where Codex/Claude are logged in, expose it through
// a public HTTPS tunnel/Funnel, and point the Worker TOKEN_SOURCE_URL at it.

import { createServer } from "node:http";
import { spawnSync } from "node:child_process";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { readFileSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));

function loadEnv() {
  try {
    const text = readFileSync(join(__dirname, ".env"), "utf8");
    for (const raw of text.split(/\r?\n/)) {
      const line = raw.trim();
      if (!line || line.startsWith("#")) continue;
      const m = line.match(/^([A-Z0-9_]+)\s*=\s*(.*?)\s*$/i);
      if (!m || process.env[m[1]] !== undefined) continue;
      process.env[m[1]] = m[2].replace(/^["']|["']$/g, "").trim();
    }
  } catch {
    // .env is optional; service managers can provide env directly.
  }
}
loadEnv();

const PORT = Number.parseInt(process.env.TOKEN_SOURCE_PORT || "8790", 10);
const HOST = (process.env.TOKEN_SOURCE_HOST || "127.0.0.1").trim();
const TOKEN_SOURCE_TOKEN = (process.env.TOKEN_SOURCE_TOKEN || "").trim();
const VERSION = "0.1.0";
const BRIDGE_CMD = [process.execPath, join(__dirname, "tamagotchi-bridge.mjs"), "--once"];

if (!TOKEN_SOURCE_TOKEN) {
  console.error("missing TOKEN_SOURCE_TOKEN in environment");
  process.exit(1);
}

function readSnapshot() {
  const result = spawnSync(BRIDGE_CMD[0], BRIDGE_CMD.slice(1), {
    encoding: "utf8",
    maxBuffer: 4 * 1024 * 1024,
    env: {
      ...process.env,
      PORT: process.env.BRIDGE_PORT || process.env.PORT || "8787",
    },
  });

  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`bridge --once failed with code ${result.status}`);
  }

  let parsed;
  try {
    parsed = JSON.parse(result.stdout.trim());
  } catch (err) {
    throw new Error(`failed parsing bridge --once output: ${(err && err.message) || err}`);
  }

  const tokens = parsed?.tokens;
  if (!tokens || typeof tokens.tokens_today !== "number") {
    throw new Error("unexpected bridge payload");
  }

  return {
    tokens_today: Math.max(0, Math.floor(tokens.tokens_today)),
    breakdown: {
      claude: Math.max(0, Math.floor(tokens.breakdown?.claude ?? 0)),
      codex: Math.max(0, Math.floor(tokens.breakdown?.codex ?? 0)),
    },
    usage: tokens.usage && typeof tokens.usage === "object" ? tokens.usage : undefined,
    ts: Math.max(0, Math.floor(tokens.ts || Date.now() / 1000)),
  };
}

function send(res, status, body, contentType = "application/json") {
  res.writeHead(status, {
    "content-type": contentType,
    "cache-control": "no-store",
  });
  res.end(contentType === "application/json" ? JSON.stringify(body) : body);
}

function authOk(req) {
  return (req.headers.authorization ?? "") === `Bearer ${TOKEN_SOURCE_TOKEN}`;
}

createServer((req, res) => {
  const t0 = Date.now();
  const log = (status) => console.log(
    `${new Date().toISOString()} ${req.method} ${req.url} -> ${status} (${Date.now() - t0}ms)`
  );

  if (req.method === "GET" && req.url === "/health") {
    log(200);
    return send(res, 200, { ok: true, version: VERSION });
  }

  if (req.method !== "GET" || req.url !== "/tokens_today") {
    log(404);
    return send(res, 404, { error: "not found" });
  }

  if (!authOk(req)) {
    log(401);
    return send(res, 401, "nope", "text/plain");
  }

  try {
    const snapshot = readSnapshot();
    log(200);
    return send(res, 200, snapshot);
  } catch (err) {
    console.error(err instanceof Error ? err.message : String(err));
    log(502);
    return send(res, 502, { error: "token snapshot unavailable" });
  }
}).listen(PORT, HOST, () => {
  console.log(`tokengochi token source v${VERSION} listening on ${HOST}:${PORT}`);
});
