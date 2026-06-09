// Quick logic check for the bridge's pure functions.
// Run with: node bridge/_test_pure.mjs
import { strict as assert } from "node:assert";
import { readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { codexCumulative } from "./tamagotchi-bridge.mjs";

// --- mood (re-implemented here; the live one is module-private) ------------
function computeMood(foodToday, now = new Date()) {
  const h = now.getHours();
  if (h < 7 || h >= 23) return "sleepy";
  if (foodToday === 0 && h >= 22) return "sick";
  if (foodToday < 5_000) return "hungry";
  return "happy";
}

assert.equal(computeMood(0,   new Date("2026-06-08T03:00:00")), "sleepy", "03:00 -> sleepy");
assert.equal(computeMood(0,   new Date("2026-06-08T07:00:00")), "hungry", "07:00 -> hungry (not sleepy)");
assert.equal(computeMood(100, new Date("2026-06-08T07:00:00")), "hungry", "low food, day");
assert.equal(computeMood(0,   new Date("2026-06-08T22:00:00")), "sick",   "22:00 + 0 food -> sick");
assert.equal(computeMood(0,   new Date("2026-06-08T23:30:00")), "sleepy", "23:30 -> sleepy wins over sick");
assert.equal(computeMood(8000,new Date("2026-06-08T15:00:00")), "happy",  "fed, afternoon -> happy");
assert.equal(computeMood(50000,new Date("2026-06-08T15:00:00")),"happy",  "well-fed -> happy");
console.log("mood: 7/7 ok");

// --- bumpTotals logic, simulated against a temp state.json ------------------
const tmp = mkdtempSync(join(tmpdir(), "tg-test-"));
const STATE = join(tmp, "state.json");
function defaultState() {
  return {
    pet_birth_ts: Math.floor(Date.now() / 1000),
    last_msg: "", last_msg_ts: 0,
    total_tokens_ever: 0,
    peak_today_today: 0, peak_today_date: "2026-06-08",
    audio_runs_today: 0, audio_runs_today_date: "2026-06-08",
  };
}
function load() { return { ...defaultState(), ...JSON.parse(readFileSync(STATE, "utf8")) }; }
function save(s) { writeFileSync(STATE, JSON.stringify(s, null, 2)); }
function bumpTotals(tt) {
  const s = load();
  const today = "2026-06-08";
  if (s.peak_today_date !== today) { s.peak_today_date = today; s.peak_today_today = 0; }
  if (tt > s.peak_today_today) { s.total_tokens_ever += tt - s.peak_today_today; s.peak_today_today = tt; save(s); }
}
function reset() { save(defaultState()); }

// 1) first call, 30k tokens -> adds 30k
reset();
bumpTotals(30_000);
assert.equal(load().total_tokens_ever, 30_000, "first call: 30k added");
assert.equal(load().peak_today_today, 30_000);

// 2) same tokens, second call -> no add (no double count)
bumpTotals(30_000);
assert.equal(load().total_tokens_ever, 30_000, "stable tokens: no double count");
assert.equal(load().peak_today_today, 30_000);

// 3) growth -> adds delta
bumpTotals(45_000);
assert.equal(load().total_tokens_ever, 45_000, "growth 30k->45k adds 15k");
assert.equal(load().peak_today_today, 45_000);

// 4) tokens decrease (mid-day reset) -> no change to total
bumpTotals(10_000);
assert.equal(load().total_tokens_ever, 45_000, "decrease ignored");
assert.equal(load().peak_today_today, 45_000);

console.log("bumpTotals: 4/4 ok");

// --- Codex token_count schema ----------------------------------------------
assert.equal(
  codexCumulative({
    total_token_usage: {
      input_tokens: 60_226,
      cached_input_tokens: 29_440,
      output_tokens: 869,
      reasoning_output_tokens: 214,
      total_tokens: 61_095,
    },
    last_token_usage: {
      input_tokens: 34_189,
      cached_input_tokens: 25_984,
      output_tokens: 462,
      reasoning_output_tokens: 65,
      total_tokens: 34_651,
    },
  }),
  61_095,
  "Codex event_msg token_count info.total_token_usage is cumulative"
);
console.log("codexCumulative: 1/1 ok");

console.log("\nAll logic tests passed.");
