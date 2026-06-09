export function computeMood(foodToday: number, now = new Date(), usage: TokenUsageMetadata | null = null): string {
  const paceStage = usage?.codex?.pace?.stage ?? null;
  if (paceStage) {
    if (paceStage === "far_behind") return "very hungry";
    if (paceStage === "behind") return "hungry";
    if (paceStage === "slightly_behind") return "peckish";
    if (paceStage === "far_ahead") return "very happy";
    if (paceStage === "ahead") return "excited";
    if (paceStage === "slightly_ahead" || paceStage === "on_track") return "happy";
  }

  const h = now.getHours();
  if (h < 7 || h >= 23) return "sleepy";
  if (foodToday === 0 && h >= 22) return "sick";
  if (foodToday < 5_000) return "hungry";
  return "happy";
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
  codex?: {
    source?: string;
    plan_type?: string | null;
    primary_used_percent?: number | null;
    secondary_used_percent?: number | null;
    metric_used_percent?: number | null;
    primary_reset_at?: number | null;
    secondary_reset_at?: number | null;
    pace?: {
      stage?: string | null;
      delta_percent?: number | null;
      expected_used_percent?: number | null;
      actual_used_percent?: number | null;
      eta_seconds?: number | null;
      will_last_to_reset?: boolean | null;
    } | null;
    synthetic_tokens_per_percent?: number | null;
    additional_rate_limits?: unknown[];
    error?: string;
  } | null;
}
