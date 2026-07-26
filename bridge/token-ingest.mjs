#!/usr/bin/env node
// Token Tamagotchi outbound-only ingest client.
//
// Reuses the local bridge parser by running:
//   node tamagotchi-bridge.mjs --once
// and forwards { tokens_today, breakdown, ts } to the Worker ingest API.

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
    // .env is optional when the service manager provides every required value.
  }
}
loadEnv();

const BRIDGE_CMD = [process.execPath, join(__dirname, "tamagotchi-bridge.mjs"), "--once"];
const WORKER_URL = (process.env.CLOUDFLARE_WORKER_URL || "").trim();
const INGEST_TOKEN = (process.env.INGEST_TOKEN || "").trim();
const INTERVAL_MS = Math.max(
  15_000,
  Number.parseInt(process.env.INGEST_INTERVAL_MS || "60000", 10)
);
const DEV = process.argv.includes("--once");

if (!WORKER_URL || !INGEST_TOKEN) {
  console.error("missing CLOUDFLARE_WORKER_URL or INGEST_TOKEN in environment");
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
      claude: Math.max(0, Math.floor((tokens.breakdown?.claude ?? 0))),
      codex: Math.max(0, Math.floor((tokens.breakdown?.codex ?? 0))),
    },
    usage: tokens.usage && typeof tokens.usage === "object" ? tokens.usage : undefined,
    ts: Math.max(0, Math.floor(tokens.ts || Date.now() / 1000)),
  };
}

async function publish(snapshot) {
  const res = await fetch(`${WORKER_URL.replace(/\/+$/, "")}/ingest/tokens`, {
    method: "POST",
    headers: {
      "authorization": `Bearer ${INGEST_TOKEN}`,
      "content-type": "application/json",
    },
    body: JSON.stringify(snapshot),
  });
  if (!res.ok) {
    await res.arrayBuffer();
    throw new Error(`POST ${WORKER_URL}/ingest/tokens -> ${res.status}`);
  }
}

async function tick() {
  const snapshot = readSnapshot();
  await publish(snapshot);
}

async function main() {
  if (DEV) {
    await tick();
    return;
  }

  while (true) {
    try {
      await tick();
      console.log("published tokens");
    } catch (err) {
      console.error(err instanceof Error ? err.message : String(err));
    }
    await new Promise(r => setTimeout(r, INTERVAL_MS));
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
