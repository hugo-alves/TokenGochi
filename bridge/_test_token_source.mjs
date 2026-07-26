// End-to-end test for token-source.mjs.
// Run with: node bridge/_test_token_source.mjs

import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { strict as assert } from "node:assert";

const __dirname = dirname(fileURLToPath(import.meta.url));
const SOURCE_PORT = 19100 + Math.floor(Math.random() * 1000);
const TOKEN_SOURCE_TOKEN = "source-test-token-" + Date.now();
const tmp = mkdtempSync(join(tmpdir(), "tg-source-test-"));
const codexHome = join(tmp, "codex");

function pad(n) {
  return String(n).padStart(2, "0");
}

function writeCodexSession() {
  const now = new Date();
  const dir = join(
    codexHome,
    "sessions",
    String(now.getFullYear()),
    pad(now.getMonth() + 1),
    pad(now.getDate())
  );
  mkdirSync(dir, { recursive: true });
  const file = join(dir, "rollout-test.jsonl");
  const timestamp = now.toISOString();
  writeFileSync(file, [
    JSON.stringify({
      timestamp,
      payload: {
        type: "token_count",
        total_token_usage: { total_tokens: 100 },
      },
    }),
    JSON.stringify({
      timestamp,
      payload: {
        type: "token_count",
        total_token_usage: { total_tokens: 250 },
      },
    }),
  ].join("\n") + "\n");
}

async function waitForHealth(url, ms = 5000) {
  const start = Date.now();
  while (Date.now() - start < ms) {
    try {
      const r = await fetch(url);
      if (r.ok) return;
    } catch {
      // keep waiting
    }
    await new Promise(r => setTimeout(r, 25));
  }
  throw new Error("timeout waiting for token source");
}

writeCodexSession();

const source = spawn(
  "node",
  [join(__dirname, "token-source.mjs")],
  {
    env: {
      ...process.env,
      CODEX_HOME: codexHome,
      TOKEN_SOURCE_TOKEN,
      TOKEN_SOURCE_PORT: String(SOURCE_PORT),
    },
    stdio: ["ignore", "pipe", "pipe"],
  }
);
source.stdout.on("data", d => process.stdout.write(`[source] ${d}`));
source.stderr.on("data", d => process.stderr.write(`[source!] ${d}`));

try {
  await waitForHealth(`http://127.0.0.1:${SOURCE_PORT}/health`);

  let r = await fetch(`http://127.0.0.1:${SOURCE_PORT}/health`);
  let j = await r.json();
  assert.equal(r.status, 200);
  assert.equal(j.ok, true);
  console.log("test 1 (/health): ok");

  r = await fetch(`http://127.0.0.1:${SOURCE_PORT}/tokens_today`);
  assert.equal(r.status, 401);
  console.log("test 2 (auth required): ok");

  r = await fetch(`http://127.0.0.1:${SOURCE_PORT}/tokens_today`, {
    headers: { authorization: `Bearer ${TOKEN_SOURCE_TOKEN}` },
  });
  j = await r.json();
  assert.equal(r.status, 200);
  assert.equal(j.breakdown.codex, 250);
  assert.equal(j.usage.source, "local_logs");
  assert.equal(j.usage.activity.source, "local_logs");
  assert.equal(j.usage.activity.stage, "awake");
  assert.equal(typeof j.usage.activity.last_active_ts, "number");
  assert.equal(typeof j.usage.activity.idle_seconds, "number");
  assert.equal(typeof j.tokens_today, "number");
  assert.equal(typeof j.ts, "number");
  assert(j.tokens_today >= j.breakdown.codex);
  console.log("test 3 (token snapshot with activity metadata): ok");

  console.log("\nAll token-source tests passed.");
} finally {
  source.kill("SIGTERM");
  rmSync(tmp, { recursive: true, force: true });
}
