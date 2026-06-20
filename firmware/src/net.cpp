#include "net.h"
#include "config.h"
#include "secrets.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

namespace net {

static int g_lastStatus = 0;

int lastStatus() { return g_lastStatus; }

static bool useHttps() {
    return String(PROXY_URL).startsWith("https://");
}

static void beginRequest(HTTPClient& http, const String& url) {
    if (useHttps()) {
        static WiFiClientSecure secureClient;
        // TODO: production hardening should replace this with a proper trust chain.
        secureClient.setInsecure();
        http.begin(secureClient, url);
        return;
    }
    http.begin(url);
}

static bool getWithAuth(const char* path, String& body) {
    g_lastStatus = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    beginRequest(http, String(PROXY_URL) + path);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
    int code = http.GET();
    g_lastStatus = code;
    if (code != 200) {
        http.end();
        return false;
    }
    body = http.getString();
    http.end();
    return true;
}

bool pingBridge() {
    String body;
    return getWithAuth("/health", body);
}

int postTranscribe(const uint8_t* wav, size_t wavSize,
                   char* outBuf, size_t outBufSize, size_t* outTextLen) {
    g_lastStatus = 0;
    if (outTextLen) *outTextLen = 0;
    if (outBuf && outBufSize) outBuf[0] = '\0';
    if (WiFi.status() != WL_CONNECTED) return 0;

    HTTPClient http;
    beginRequest(http, String(PROXY_URL) + "/transcribe");
    http.setTimeout(TRANSCRIBE_TIMEOUT_MS);
    http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
    http.addHeader("Content-Type",  "audio/wav");
    int code = http.POST((uint8_t*)wav, wavSize);
    g_lastStatus = code;

    if (code > 0 && outBuf && outBufSize) {
        String response = http.getString();
        size_t pos = response.length();
        if (pos > outBufSize - 1) pos = outBufSize - 1;
        memcpy(outBuf, response.c_str(), pos);
        outBuf[pos] = '\0';
        if (outTextLen) *outTextLen = pos;
    }
    http.end();
    return code;
}

// Static buffer for the GET response from /pet/state (called internally).
static bool getJson(const char* path, JsonDocument& doc) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    beginRequest(http, String(PROXY_URL) + path);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
    int code = http.GET();
    g_lastStatus = code;
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    return deserializeJson(doc, body) == DeserializationError::Ok;
}

static void copyString(char* out, size_t outSize, const char* value) {
    if (!out || outSize == 0) return;
    strncpy(out, value ? value : "", outSize - 1);
    out[outSize - 1] = '\0';
}

static bool percentVariantToX10(JsonVariant value, int16_t& out, bool signedValue = false) {
    if (!value.is<float>() && !value.is<int>()) return false;
    float percent = value | 0.0f;
    if (signedValue) {
        if (percent < -100.0f) percent = -100.0f;
        if (percent > 100.0f) percent = 100.0f;
    } else {
        if (percent < 0.0f) percent = 0.0f;
        if (percent > 100.0f) percent = 100.0f;
    }
    out = (int16_t)(percent * 10.0f + (percent >= 0.0f ? 0.5f : -0.5f));
    return true;
}

static const char* paceKindFromStage(const char* stage) {
    if (!stage || !*stage) return "";
    if (strcmp(stage, "on_track") == 0) return "on_pace";
    if (strcmp(stage, "slightly_ahead") == 0 ||
        strcmp(stage, "ahead") == 0 ||
        strcmp(stage, "far_ahead") == 0) {
        return "deficit";
    }
    if (strcmp(stage, "slightly_behind") == 0 ||
        strcmp(stage, "behind") == 0 ||
        strcmp(stage, "far_behind") == 0) {
        return "reserve";
    }
    return "";
}

static void derivePaceFields(PetState& out) {
    if (!out.codex_pace_kind[0] && out.codex_pace_delta_x10 != INT16_MIN) {
        if (out.codex_pace_delta_x10 > 20) {
            copyString(out.codex_pace_kind, sizeof(out.codex_pace_kind), "deficit");
        } else if (out.codex_pace_delta_x10 < -20) {
            copyString(out.codex_pace_kind, sizeof(out.codex_pace_kind), "reserve");
        } else {
            copyString(out.codex_pace_kind, sizeof(out.codex_pace_kind), "on_pace");
        }
    }

    if (out.codex_balance_percent_x10 < 0 && out.codex_pace_delta_x10 != INT16_MIN) {
        int16_t absDelta = out.codex_pace_delta_x10 < 0
            ? (int16_t)(-out.codex_pace_delta_x10)
            : out.codex_pace_delta_x10;
        out.codex_balance_percent_x10 = absDelta;
    }

    if (!out.codex_pace_label[0] && out.codex_pace_kind[0]) {
        if (strcmp(out.codex_pace_kind, "on_pace") == 0) {
            copyString(out.codex_pace_label, sizeof(out.codex_pace_label), "on pace");
        } else {
            int pct = out.codex_balance_percent_x10 >= 0
                ? (out.codex_balance_percent_x10 + 5) / 10
                : 0;
            snprintf(out.codex_pace_label,
                     sizeof(out.codex_pace_label),
                     "%d%% %s",
                     pct,
                     out.codex_pace_kind);
        }
    }
}

static bool activityIsGrumpy(const PetState& out) {
    return strcmp(out.activity_stage, "grumpy") == 0 ||
           strcmp(out.activity_stage, "very_grumpy") == 0 ||
           strcmp(out.activity_stage, "very grumpy") == 0;
}

static void applyUsageMood(PetState& out) {
    if (strcmp(out.codex_pace_kind, "reserve") == 0) {
        copyString(out.mood, sizeof(out.mood), "very hungry");
    } else if (strcmp(out.codex_pace_kind, "deficit") == 0) {
        copyString(out.mood, sizeof(out.mood), "very happy");
    } else if (activityIsGrumpy(out)) {
        copyString(out.mood, sizeof(out.mood), "grumpy");
    } else if (strcmp(out.codex_pace_kind, "on_pace") == 0) {
        copyString(out.mood, sizeof(out.mood), "happy");
    }
}

static void parseUsageMetadata(JsonDocument& doc, PetState& out) {
    JsonObject usage = doc["usage"];
    if (usage.isNull()) return;

    JsonObject activity = usage["activity"];
    if (!activity.isNull()) {
        copyString(out.activity_stage, sizeof(out.activity_stage), activity["stage"] | "unknown");
        out.activity_idle_seconds = activity["idle_seconds"] | -1;
        out.activity_last_active_ts = activity["last_active_ts"] | 0;
    }

    JsonObject codex = usage["codex"];
    if (codex.isNull()) {
        applyUsageMood(out);
        return;
    }

    const char* source = codex["source"] | "";
    if (strcmp(source, "codex_account") != 0) {
        applyUsageMood(out);
        return;
    }

    percentVariantToX10(codex["metric_used_percent"], out.codex_usage_percent_x10);

    JsonObject pace = codex["pace"];
    if (!pace.isNull()) {
        percentVariantToX10(pace["actual_used_percent"], out.codex_usage_percent_x10);
        percentVariantToX10(pace["expected_used_percent"], out.codex_expected_percent_x10);
        percentVariantToX10(pace["delta_percent"], out.codex_pace_delta_x10, true);
        percentVariantToX10(pace["balance_percent"], out.codex_balance_percent_x10);

        const char* kind = pace["balance_kind"] | "";
        if (!kind[0]) kind = paceKindFromStage(pace["stage"] | "");
        copyString(out.codex_pace_kind, sizeof(out.codex_pace_kind), kind);
        copyString(out.codex_pace_label, sizeof(out.codex_pace_label), pace["balance_label"] | "");
        derivePaceFields(out);
    }

    const char* plan = codex["plan_type"] | "";
    copyString(out.codex_plan, sizeof(out.codex_plan), plan);
    applyUsageMood(out);
}

int postReset(PetState& out) {
    petStateReset(out);
    if (WiFi.status() != WL_CONNECTED) return 0;
    HTTPClient http;
    beginRequest(http, String(PROXY_URL) + "/pet/reset");
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
    int code = http.POST((uint8_t*)"", 0);
    g_lastStatus = code;
    if (code != 200) { http.end(); return code; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return 0;
    copyString(out.mood, sizeof(out.mood), doc["mood"] | "happy");
    out.age_s             = doc["age_s"]             | 0;
    out.food_today        = doc["food_today"]        | 0;
    out.last_msg_ts       = doc["last_msg_ts"]       | 0;
    out.total_tokens_ever = doc["total_tokens_ever"] | 0;
    out.audio_runs_today  = doc["audio_runs_today"]  | 0;
    out.ts                = doc["ts"]                | 0;
    const char* msg = doc["last_msg"] | "";
    copyString(out.last_msg, sizeof(out.last_msg), msg);
    JsonObject bd = doc["breakdown"];
    if (!bd.isNull()) {
        out.breakdown_claude = bd["claude"] | 0;
        out.breakdown_codex  = bd["codex"]  | 0;
    }
    parseUsageMetadata(doc, out);
    return code;
}

bool fetchPetState(PetState& out) {
    petStateReset(out);

    String body;
    if (!getWithAuth("/pet/state", body)) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("net: bad json: %s\n", err.c_str());
        return false;
    }

    const char* mood = doc["mood"] | "unknown";
    copyString(out.mood, sizeof(out.mood), mood);
    out.age_s              = doc["age_s"]              | 0;
    out.food_today         = doc["food_today"]         | 0;
    out.last_msg_ts        = doc["last_msg_ts"]        | 0;
    out.total_tokens_ever  = doc["total_tokens_ever"]  | 0;
    out.audio_runs_today   = doc["audio_runs_today"]   | 0;
    out.ts                 = doc["ts"]                 | 0;

    const char* msg = doc["last_msg"] | "";
    copyString(out.last_msg, sizeof(out.last_msg), msg);

    JsonObject bd = doc["breakdown"];
    if (!bd.isNull()) {
        out.breakdown_claude = bd["claude"] | 0;
        out.breakdown_codex  = bd["codex"]  | 0;
    }
    parseUsageMetadata(doc, out);
    return true;
}

}  // namespace net
