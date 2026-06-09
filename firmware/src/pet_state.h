#pragma once
// Pet state — mirrors the JSON shape returned by GET /pet/state on the bridge.

#include <stdint.h>
#include <string.h>

struct PetState {
    char     mood[16];          // pace-aware mood label, e.g. "very hungry"
    int32_t  age_s;
    int32_t  food_today;
    char     last_msg[256];
    int32_t  last_msg_ts;
    int32_t  total_tokens_ever;
    int32_t  audio_runs_today;
    int32_t  ts;                // unix seconds, server clock
    int32_t  breakdown_claude;
    int32_t  breakdown_codex;
    int16_t  codex_usage_percent_x10;  // -1 when backend is reporting raw tokens
    char     codex_plan[16];
};

inline void petStateReset(PetState& s) {
    memset(&s, 0, sizeof(s));
    strncpy(s.mood, "unknown", sizeof(s.mood) - 1);
    s.codex_usage_percent_x10 = -1;
}
