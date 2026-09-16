// End-to-end test for /transcribe using a mock Groq server.
// Run with: node bridge/_test_transcribe.mjs
//
// Spawns a mock "Groq" that captures the multipart body and returns a canned
// JSON transcript, then starts the real bridge with GROQ_URL pointed at the
// mock. Sends a synthesized 16 kHz mono WAV through /transcribe and asserts
// the response, isolated state-file side effects, and the multipart envelope.

import { createServer } from "node:http";
import { readFileSync, copyFileSync, mkdtempSync, realpathSync, rmSync } from "node:fs";
import { spawn } from "node:child_process";
import { join, dirname } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import { strict as assert } from "node:assert";

const __dirname = dirname(fileURLToPath(import.meta.url));
const BRIDGE_PORT  = 19000 + Math.floor(Math.random() * 1000);
const MOCK_PORT    = BRIDGE_PORT + 1;
const DEVICE_TOKEN = "test-device-token-at-least-32-chars-" + Date.now();
const TEST_DIR     = mkdtempSync(join(tmpdir(), "tokengochi-transcribe-"));
const STATE_FILE   = join(TEST_DIR, "state.json");
const BRIDGE_SCRIPT = join(TEST_DIR, "tamagotchi-bridge.mjs");

// --- helpers ---------------------------------------------------------------
function makeWav(durationS = 1.0, sampleRate = 16000) {
  const numChannels = 1;
  const bitsPerSample = 16;
  const numSamples = Math.floor(durationS * sampleRate);
  const dataSize = numSamples * numChannels * (bitsPerSample / 8);
  const fileSize = 36 + dataSize;
  const buf = Buffer.alloc(44 + dataSize);
  buf.write("RIFF", 0);
  buf.writeUInt32LE(fileSize, 4);
  buf.write("WAVE", 8);
  buf.write("fmt ", 12);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(numChannels, 22);
  buf.writeUInt32LE(sampleRate, 24);
  buf.writeUInt32LE(sampleRate * numChannels * (bitsPerSample / 8), 28);
  buf.writeUInt16LE(numChannels * (bitsPerSample / 8), 32);
  buf.writeUInt16LE(bitsPerSample, 34);
  buf.write("data", 36);
  buf.writeUInt32LE(dataSize, 40);
  return buf;
}

function waitForBridgeReady(child, ms = 5000) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => finish(new Error("timeout waiting for bridge startup")), ms);
    const onOutput = (data) => {
      if (String(data).includes("listening on")) finish();
    };
    const onExit = (code, signal) => finish(
      new Error(`bridge exited before startup (code=${code}, signal=${signal})`)
    );
    const onError = (error) => finish(error);
    function finish(error) {
      clearTimeout(timeout);
      child.stdout.off("data", onOutput);
      child.off("exit", onExit);
      child.off("error", onError);
      if (error) reject(error);
      else resolve();
    }
    child.stdout.on("data", onOutput);
    child.once("exit", onExit);
    child.once("error", onError);
  });
}

function listen(server, port) {
  return new Promise((resolve, reject) => {
    const onListening = () => finish();
    const onError = (error) => finish(error);
    function finish(error) {
      server.off("listening", onListening);
      server.off("error", onError);
      if (error) reject(error);
      else resolve();
    }
    server.once("listening", onListening);
    server.once("error", onError);
    server.listen(port, "127.0.0.1");
  });
}

// --- mock groq -------------------------------------------------------------
let mockLastRequest = null;
let mockResponse = { status: 200, body: { text: "hello tamagotchi", language: "en" } };

const mockGroq = createServer((req, res) => {
  const chunks = [];
  req.on("data", c => chunks.push(c));
  req.on("end", () => {
    mockLastRequest = {
      method: req.method,
      url: req.url,
      contentType: req.headers["content-type"] ?? "",
      authorization: req.headers["authorization"] ?? "",
      body: Buffer.concat(chunks),
    };
    res.writeHead(mockResponse.status, { "content-type": "application/json" });
    res.end(JSON.stringify(mockResponse.body));
  });
});

let bridge;

try {
  await listen(mockGroq, MOCK_PORT);
  console.log(`mock groq listening on :${MOCK_PORT}`);

  // --- bridge process -------------------------------------------------------
  // Copying the standalone bridge into TEST_DIR keeps its relative .env and
  // state.json paths away from developer configuration and runtime state.
  copyFileSync(join(__dirname, "tamagotchi-bridge.mjs"), BRIDGE_SCRIPT);
  bridge = spawn(
    process.execPath,
    [realpathSync(BRIDGE_SCRIPT)],
    {
      env: {
        PORT: String(BRIDGE_PORT),
        DEVICE_TOKEN,
        GROQ_API_KEY: "mock-groq-key",
        GROQ_URL: `http://127.0.0.1:${MOCK_PORT}/`,
        HOME: TEST_DIR,
        CLAUDE_CONFIG_DIR: join(TEST_DIR, "claude"),
        CODEX_HOME: join(TEST_DIR, "codex"),
      },
      stdio: ["ignore", "pipe", "pipe"],
    },
  );
  const bridgeReady = waitForBridgeReady(bridge);
  bridge.stdout.on("data", d => process.stdout.write(`[bridge] ${d}`));
  bridge.stderr.on("data", d => process.stderr.write(`[bridge!] ${d}`));

  await bridgeReady;
  console.log(`bridge listening on :${BRIDGE_PORT}`);

  if (process.env.TOKENGOCHI_TEST_FORCE_FAILURE === "1") {
    throw new Error("forced failure after bridge startup");
  }

// --- test 1: 1s wav, happy path --------------------------------------------
mockLastRequest = null;
mockResponse = { status: 200, body: { text: "hello tamagotchi", language: "en" } };

let r = await fetch(`http://127.0.0.1:${BRIDGE_PORT}/transcribe`, {
  method: "POST",
  headers: {
    "Authorization": `Bearer ${DEVICE_TOKEN}`,
    "Content-Type": "audio/wav",
  },
  body: makeWav(1.0),
});
let j = await r.json();
assert.equal(r.status, 200, `status: ${r.status} body: ${JSON.stringify(j)}`);
assert.equal(j.text, "hello tamagotchi");
assert.equal(j.duration_s, 1.0);
assert.equal(j.lang, "en");
assert(j.ms_groq >= 0);
console.log("test 1 (1s wav happy path): ok");

// mock received a proper multipart envelope
assert.equal(mockLastRequest.method, "POST");
assert(mockLastRequest.contentType.startsWith("multipart/form-data; boundary="), `ct: ${mockLastRequest.contentType}`);
assert.equal(mockLastRequest.authorization, "Bearer mock-groq-key");
assert(mockLastRequest.body.includes("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\""), "file part header");
assert(mockLastRequest.body.includes("Content-Disposition: form-data; name=\"model\""), "model part header");
assert(mockLastRequest.body.includes("whisper-large-v3"), "model value");
assert(mockLastRequest.body.includes(Buffer.from("RIFF")), "wav header inside body");
assert(mockLastRequest.body.includes(makeWav(1.0)), "wav bytes inside body");
console.log("test 2 (multipart envelope to groq): ok");

// state.json got updated
const state1 = JSON.parse(readFileSync(STATE_FILE, "utf8"));
assert.equal(state1.last_msg, "hello tamagotchi");
assert.equal(state1.audio_runs_today, 1);
assert(state1.last_msg_ts > 0);
console.log("test 3 (state.json side effects): ok");

// --- test 4: 0.5s wav, different duration ----------------------------------
mockLastRequest = null;
mockResponse = { status: 200, body: { text: "second transcript", language: "pt" } };

r = await fetch(`http://127.0.0.1:${BRIDGE_PORT}/transcribe`, {
  method: "POST",
  headers: {
    "Authorization": `Bearer ${DEVICE_TOKEN}`,
    "Content-Type": "audio/wav",
  },
  body: makeWav(0.5),
});
j = await r.json();
assert.equal(j.text, "second transcript");
assert.equal(j.duration_s, 0.5);
assert.equal(j.lang, "pt");
const state2 = JSON.parse(readFileSync(STATE_FILE, "utf8"));
assert.equal(state2.last_msg, "second transcript");
assert.equal(state2.audio_runs_today, 2);
console.log("test 4 (0.5s wav, counter increments): ok");

// --- test 5: groq returns 4xx -> 502 from bridge ---------------------------
mockResponse = { status: 401, body: { error: { message: "Invalid API key" } } };
r = await fetch(`http://127.0.0.1:${BRIDGE_PORT}/transcribe`, {
  method: "POST",
  headers: {
    "Authorization": `Bearer ${DEVICE_TOKEN}`,
    "Content-Type": "audio/wav",
  },
  body: makeWav(0.3),
});
j = await r.json();
assert.equal(r.status, 502, `expected 502, got ${r.status}`);
assert.match(j.error, /groq 401/);
console.log("test 5 (groq 4xx -> 502): ok");

// --- test 6: POST /pet/reset soft-resets -------------------------------------
// Set a known state by recording two transcribes first, then call reset.
mockResponse = { status: 200, body: { text: "reset-me-1", language: "en" } };
r = await fetch(`http://127.0.0.1:${BRIDGE_PORT}/transcribe`, {
  method: "POST",
  headers: { "Authorization": `Bearer ${DEVICE_TOKEN}`, "Content-Type": "audio/wav" },
  body: Buffer.alloc(100),
});
j = await r.json();
assert.equal(j.text, "reset-me-1");

r = await fetch(`http://127.0.0.1:${BRIDGE_PORT}/pet/reset`, {
  method: "POST",
  headers: { "Authorization": `Bearer ${DEVICE_TOKEN}` },
});
assert.equal(r.status, 200, "reset status");
const after = await r.json();
assert.equal(after.last_msg, "",        "last_msg cleared");
assert.equal(after.audio_runs_today, 0, "audio_runs_today reset");
assert(after.age_s < 5,                 "pet was just born");
assert(after.mood,                     "mood still computed");
console.log("test 6 (/pet/reset soft-reset): ok");

  console.log("\nAll /transcribe tests passed.");
} finally {
  if (bridge?.pid && bridge.exitCode === null && bridge.signalCode === null) {
    bridge.kill("SIGTERM");
    await new Promise(resolve => bridge.once("exit", resolve));
  }
  if (mockGroq.listening) {
    await new Promise(resolve => mockGroq.close(resolve));
  }
  rmSync(TEST_DIR, { recursive: true, force: true });
}
