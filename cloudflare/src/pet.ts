export function computeMood(foodToday: number, now = new Date(), usage: TokenUsageMetadata | null = null): string {
  const pace = usage?.codex?.pace ?? null;
  const balanceKind = pace?.balance_kind ?? paceKindForStage(pace?.stage);
  if (balanceKind === "reserve") return "very hungry";
  if (balanceKind === "deficit") return "very happy";

  const activityStage = usage?.activity?.stage ?? "unknown";
  if (activityStage === "grumpy" || activityStage === "very_grumpy") return "grumpy";
  if (balanceKind === "on_pace") return "happy";

  if (activityStage && activityStage !== "unknown") {
    if (foodToday < 5_000) return "hungry";
    return "happy";
  }

  const h = now.getHours();
  if (h < 7 || h >= 23) return "sleepy";
  if (foodToday === 0 && h >= 22) return "sick";
  if (foodToday < 5_000) return "hungry";
  return "happy";
}

function paceKindForStage(stage?: string | null): "reserve" | "deficit" | "on_pace" | null {
  if (!stage) return null;
  if (stage === "on_track") return "on_pace";
  if (stage === "slightly_ahead" || stage === "ahead" || stage === "far_ahead") return "deficit";
  if (stage === "slightly_behind" || stage === "behind" || stage === "far_behind") return "reserve";
  return null;
}

export interface PetStatePayload {
  mood: string;
  age_s: number;
  food_today: number;
  last_msg: string;
  last_msg_ts: number;
  total_tokens_ever: number;
  audio_runs_today: number;
  breakdown: {
    claude: number;
    codex: number;
  };
  usage?: TokenUsageMetadata | null;
  ts: number;
}

export interface TokenSnapshot {
  tokens_today: number;
  breakdown: {
    claude: number;
    codex: number;
  };
  usage?: TokenUsageMetadata | null;
  ts: number;
}

export interface TokenUsageMetadata {
  source?: string;
  activity?: {
    source?: string | null;
    last_active_ts?: number | null;
    idle_seconds?: number | null;
    stage?: "awake" | "restless" | "grumpy" | "very_grumpy" | "unknown" | string | null;
  } | null;
  codex?: {
    source?: string;
    plan_type?: string | null;
    primary_used_percent?: number | null;
    secondary_used_percent?: number | null;
    metric_used_percent?: number | null;
    metric_window?: string | null;
    primary_reset_at?: number | null;
    secondary_reset_at?: number | null;
    pace?: {
      window?: string | null;
      stage?: string | null;
      delta_percent?: number | null;
      expected_used_percent?: number | null;
      actual_used_percent?: number | null;
      balance_kind?: "reserve" | "deficit" | "on_pace" | string | null;
      balance_percent?: number | null;
      balance_label?: string | null;
      eta_seconds?: number | null;
      will_last_to_reset?: boolean | null;
    } | null;
    synthetic_tokens_per_percent?: number | null;
    additional_rate_limits?: unknown[];
    error?: string;
  } | null;
}
