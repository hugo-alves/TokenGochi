export function computeMood(foodToday: number, now = new Date()): string {
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
  ts: number;
}

export interface TokenSnapshot {
  tokens_today: number;
  breakdown: {
    claude: number;
    codex: number;
  };
  ts: number;
}
