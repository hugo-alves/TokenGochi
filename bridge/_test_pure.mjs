// Quick logic check for the bridge's pure functions.
// Run with: node bridge/_test_pure.mjs
import { strict as assert } from "node:assert";
import { readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  accountUsageMetadata,
  accountUsagePace,
  activityMetadata,
  activityStage,
  codexCumulative,
  computeMood as bridgeComputeMood,
  isValidDeviceToken,
  latestActivityMs,
  paceBalance,
  paceStage,
} from "./tamagotchi-bridge.mjs";

// --- server secret validation ----------------------------------------------
assert.equal(isValidDeviceToken(""), false);
assert.equal(isValidDeviceToken("short-token"), false);
assert.equal(isValidDeviceToken("replace-with-a-random-device-token"), false);
assert.equal(
  isValidDeviceToken("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
  true
);
console.log("device token validation: 4/4 ok");

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

// --- Codex account usage schema -------------------------------------------
const accountNowSec = Math.floor(Date.now() / 1000);
const accountUsage = accountUsageMetadata({
  plan_type: "pro",
  rate_limit: {
    primary_window: { used_percent: 26, reset_at: accountNowSec + 9000, limit_window_seconds: 18000 },
    secondary_window: { used_percent: 28, reset_at: accountNowSec + 500, limit_window_seconds: 1000 },
  },
  additional_rate_limits: [{
    limit_name: "GPT-5.3-Codex-Spark",
    metered_feature: "codex_bengalfox",
    rate_limit: {
      primary_window: { used_percent: 0, reset_at: 1781012036 },
      secondary_window: { used_percent: 2, reset_at: 1781546377 },
    },
  }],
});
assert.equal(accountUsage.source, "codex_account");
assert.equal(accountUsage.plan_type, "pro");
assert.equal(accountUsage.primary_used_percent, 26);
assert.equal(accountUsage.secondary_used_percent, 28);
assert.equal(accountUsage.metric_used_percent, 28);
assert.equal(accountUsage.metric_window, "weekly");
assert.equal(accountUsage.additional_rate_limits[0].secondary_used_percent, 2);
assert.equal(accountUsage.pace.stage, "far_behind");
assert.equal(accountUsage.pace.balance_kind, "reserve");
assert.equal(accountUsage.pace.balance_label, "22% reserve");
console.log("codex account usage mapping: 10/10 ok");

// --- Codex pace -------------------------------------------------------------
assert.equal(paceStage(0), "on_track");
assert.equal(paceStage(4), "slightly_ahead");
assert.equal(paceStage(8), "ahead");
assert.equal(paceStage(13), "far_ahead");
assert.equal(paceStage(-4), "slightly_behind");
assert.equal(paceStage(-8), "behind");
assert.equal(paceStage(-13), "far_behind");
assert.deepEqual(paceBalance(0), { kind: "on_pace", percent: 0, label: "on pace" });
assert.deepEqual(paceBalance(8), { kind: "deficit", percent: 8, label: "8% deficit" });
assert.deepEqual(paceBalance(-6.4), { kind: "reserve", percent: 6.4, label: "6% reserve" });

const pace = accountUsagePace(
  { used_percent: 60, reset_at: 2000, limit_window_seconds: 1000 },
  1500 * 1000
);
assert.equal(pace.expected_used_percent, 50);
assert.equal(pace.delta_percent, 10);
assert.equal(pace.stage, "ahead");
assert.equal(pace.balance_kind, "deficit");
assert.equal(pace.balance_label, "10% deficit");
assert.equal(bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), { codex: { pace } }), "very happy");
assert.equal(
  bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), { codex: { pace: { stage: "slightly_behind" } } }),
  "very hungry"
);
assert.equal(
  bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), { codex: { pace: { stage: "behind" } } }),
  "very hungry"
);
assert.equal(
  bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), { codex: { pace: { stage: "far_behind" } } }),
  "very hungry"
);
assert.equal(
  bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), { codex: { pace: { stage: "far_ahead" } } }),
  "very happy"
);
console.log("codex pace: 23/23 ok");

// --- Agent activity ---------------------------------------------------------
const activityNowMs = Date.parse("2026-06-08T12:00:00Z");
assert.equal(activityStage(29 * 60), "awake");
assert.equal(activityStage(30 * 60), "restless");
assert.equal(activityStage(90 * 60), "grumpy");
assert.equal(activityStage(180 * 60), "very_grumpy");
assert.equal(activityStage(-1), "unknown");

assert.deepEqual(
  activityMetadata(activityNowMs - 10 * 60 * 1000, activityNowMs),
  {
    source: "local_logs",
    last_active_ts: Math.floor((activityNowMs - 10 * 60 * 1000) / 1000),
    idle_seconds: 600,
    stage: "awake",
  },
  "recent Codex activity -> awake"
);
assert.equal(
  activityMetadata(activityNowMs - 2 * 60 * 60 * 1000, activityNowMs).stage,
  "grumpy",
  "stale Codex activity -> grumpy"
);
assert.equal(
  activityMetadata(activityNowMs - 4 * 60 * 60 * 1000, activityNowMs).stage,
  "very_grumpy",
  "very stale Codex activity -> very_grumpy"
);
assert.equal(
  latestActivityMs([
    activityNowMs - 4 * 60 * 60 * 1000,
    activityNowMs - 5 * 60 * 1000,
  ]),
  activityNowMs - 5 * 60 * 1000,
  "Claude activity counts when Codex has no recent token event"
);
assert.deepEqual(
  activityMetadata(null, activityNowMs),
  {
    source: "local_logs",
    last_active_ts: null,
    idle_seconds: null,
    stage: "unknown",
  },
  "missing logs -> unknown"
);
console.log("agent activity: 10/10 ok");

// --- Mood priority across food and activity --------------------------------
assert.equal(
  bridgeComputeMood(0, new Date("2026-06-08T12:00:00"), {
    codex: { pace: { balance_kind: "reserve" } },
    activity: { stage: "very_grumpy" },
  }),
  "very hungry",
  "quota reserve keeps very hungry priority"
);
assert.equal(
  bridgeComputeMood(20_000, new Date("2026-06-08T12:00:00"), {
    codex: { pace: { balance_kind: "on_pace" } },
    activity: { stage: "grumpy" },
  }),
  "grumpy",
  "on-pace food allows grumpy activity mood"
);
assert.equal(
  bridgeComputeMood(20_000, new Date("2026-06-08T03:00:00"), {
    activity: { stage: "awake" },
  }),
  "happy",
  "time-of-day fallback only applies when activity is unknown"
);
assert.equal(
  bridgeComputeMood(20_000, new Date("2026-06-08T03:00:00"), {
    activity: { stage: "unknown" },
  }),
  "sleepy",
  "unknown activity keeps time-of-day fallback"
);
console.log("mood priority: 4/4 ok");

console.log("\nAll logic tests passed.");
