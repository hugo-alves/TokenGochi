CREATE TABLE IF NOT EXISTS app_state (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  pet_birth_ts INTEGER NOT NULL,
  last_msg TEXT NOT NULL,
  last_msg_ts INTEGER NOT NULL,
  total_tokens_ever INTEGER NOT NULL,
  peak_today_total INTEGER NOT NULL,
  peak_today_date TEXT NOT NULL,
  audio_runs_today INTEGER NOT NULL,
  audio_runs_today_date TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS token_counters (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  tokens_today INTEGER NOT NULL,
  breakdown_claude INTEGER NOT NULL,
  breakdown_codex INTEGER NOT NULL,
  ts INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS transcriptions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  text TEXT NOT NULL,
  language TEXT,
  duration_ms INTEGER NOT NULL,
  ms_groq INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
