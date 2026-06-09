#!/usr/bin/env node
// Guarded rollout helper for the VPS-backed token source.
//
// Dry-run is the default. Use --apply --yes to copy/install the VPS service and
// enable Tailscale Funnel. Add --deploy-staging to set Worker staging secrets,
// deploy, and run the cloud smoke.

import { randomBytes } from "node:crypto";
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import process from "node:process";
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

const args = new Set(process.argv.slice(2));
const apply = args.has("--apply");
const yes = args.has("--yes");
const deployStaging = args.has("--deploy-staging");
const vps = valueArg("--vps") || process.env.VPS || "deployer@100.78.209.61";
const sourceUrl = (valueArg("--source-url") || process.env.TOKEN_SOURCE_URL || "https://g33k-kid-agent.taild47216.ts.net").replace(/\/+$/, "");
const workerUrl = (valueArg("--worker-url") || process.env.CLOUDFLARE_WORKER_URL || "").replace(/\/+$/, "");
const deviceToken = process.env.DEVICE_TOKEN || "";
const ingestToken = process.env.INGEST_TOKEN || "";
let tokenSourceToken = process.env.TOKEN_SOURCE_TOKEN || "";

function valueArg(name) {
  const prefix = `${name}=`;
  const found = process.argv.slice(2).find((arg) => arg.startsWith(prefix));
  return found ? found.slice(prefix.length) : "";
}

function shQuote(s) {
  return `'${String(s).replace(/'/g, `'\\''`)}'`;
}

function run(cmd, opts = {}) {
  console.log(`$ ${cmd}`);
  if (!apply && !opts.always) return;
  const res = spawnSync(cmd, {
    cwd: opts.cwd || root,
    shell: true,
    stdio: opts.input ? ["pipe", "inherit", "inherit"] : "inherit",
    input: opts.input,
    encoding: "utf8",
    env: { ...process.env, ...(opts.env || {}) },
  });
  if (res.status !== 0) process.exit(res.status ?? 1);
}

function capture(cmd) {
  const res = spawnSync(cmd, {
    cwd: root,
    shell: true,
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  });
  if (res.status !== 0) return "";
  return res.stdout.trim();
}

function requireFile(path) {
  if (!existsSync(path)) {
    console.error(`missing required file: ${path}`);
    process.exit(1);
  }
}

for (const file of [
  "bridge/tamagotchi-bridge.mjs",
  "bridge/token-source.mjs",
  "bridge/tokengochi-token-source.service",
  "bridge/install-token-source-systemd.sh",
  "bridge/uninstall-token-source-systemd.sh",
]) {
  requireFile(join(root, file));
}

if (deployStaging && !apply) {
  console.error("--deploy-staging requires --apply");
  process.exit(1);
}
if (apply && !yes) {
  console.error("--apply changes the VPS/Funnel state and requires --yes");
  process.exit(1);
}
if (deployStaging && (!workerUrl || !deviceToken || !ingestToken)) {
  console.error("--deploy-staging requires CLOUDFLARE_WORKER_URL, DEVICE_TOKEN, and INGEST_TOKEN in the environment");
  process.exit(1);
}

if (!tokenSourceToken && apply) {
  tokenSourceToken = capture(`ssh ${shQuote(vps)} 'sed -n "s/^TOKEN_SOURCE_TOKEN=//p" ~/TokenGochi/bridge/.env 2>/dev/null | head -n 1 | tr -d "\\r"'`);
}
if (!tokenSourceToken) {
  tokenSourceToken = randomBytes(32).toString("hex");
}

console.log(`mode: ${apply ? "apply" : "dry-run"}`);
console.log(`target VPS: ${vps}`);
console.log(`token source URL: ${sourceUrl}`);
console.log(`token source token: ${tokenSourceToken.slice(0, 6)}...${tokenSourceToken.slice(-4)}`);

run(`ssh ${shQuote(vps)} 'mkdir -p ~/TokenGochi/bridge'`);
run([
  "scp",
  "bridge/tamagotchi-bridge.mjs",
  "bridge/token-source.mjs",
  "bridge/tokengochi-token-source.service",
  "bridge/install-token-source-systemd.sh",
  "bridge/uninstall-token-source-systemd.sh",
  `${shQuote(vps)}:~/TokenGochi/bridge/`,
].join(" "));
run(
  `ssh ${shQuote(vps)} 'umask 077; cat > ~/TokenGochi/bridge/.env'`,
  { input: `TOKEN_SOURCE_TOKEN=${tokenSourceToken}\nTOKEN_SOURCE_PORT=8790\n` }
);
run(`ssh ${shQuote(vps)} 'chmod +x ~/TokenGochi/bridge/*.mjs ~/TokenGochi/bridge/*systemd.sh && ~/TokenGochi/bridge/install-token-source-systemd.sh'`);
run(`ssh ${shQuote(vps)} 'curl -fsS http://127.0.0.1:8790/health && printf "\\n"'`);
run(`ssh ${shQuote(vps)} 'code=$(curl -sS -o /tmp/tokengochi-unauth.txt -w "%{http_code}" http://127.0.0.1:8790/tokens_today); test "$code" = 401; echo "$code"'`);
run(`ssh ${shQuote(vps)} 'curl -fsS http://127.0.0.1:8790/tokens_today -H "Authorization: Bearer $(sed -n s/^TOKEN_SOURCE_TOKEN=//p ~/TokenGochi/bridge/.env)" && printf "\\n"'`);
run(`ssh ${shQuote(vps)} 'tailscale funnel --bg --yes 8790'`);
run(`ssh ${shQuote(vps)} 'tailscale funnel status'`);

if (deployStaging) {
  run("npx wrangler secret put TOKEN_SOURCE_URL --env staging", {
    cwd: join(root, "cloudflare"),
    input: sourceUrl,
  });
  run("npx wrangler secret put TOKEN_SOURCE_TOKEN --env staging", {
    cwd: join(root, "cloudflare"),
    input: tokenSourceToken,
  });
  run("npm run deploy:staging", { cwd: join(root, "cloudflare") });
  run("node tools/cloud-vps-smoke.mjs", {
    cwd: root,
    env: {
      CLOUDFLARE_WORKER_URL: workerUrl,
      DEVICE_TOKEN: deviceToken,
      INGEST_TOKEN: ingestToken,
      TOKEN_SOURCE_URL: sourceUrl,
      TOKEN_SOURCE_TOKEN: tokenSourceToken,
    },
  });
}

if (!apply) {
  console.log("\nDry-run only. Re-run with --apply --yes to install the VPS service and enable Funnel.");
  console.log("Add --deploy-staging and set CLOUDFLARE_WORKER_URL, DEVICE_TOKEN, and INGEST_TOKEN to deploy/smoke staging.");
}
