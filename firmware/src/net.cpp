#include "net.h"
#include "config.h"
#include "secrets.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

namespace net {

static int g_lastStatus = 0;

// These roots cover the documented workers.dev chain plus common Google Trust
// and Let's Encrypt public endpoint chains. Custom HTTPS endpoints can override
// them through TLS_ROOT_CA in config.h.
// Sources: https://pki.goog/repository/ and
// https://letsencrypt.org/certs/isrgrootx1.pem
static const char kWorkersDevRootCa[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

int lastStatus() { return g_lastStatus; }

static bool useHttps() {
    return String(PROXY_URL).startsWith("https://");
}

static bool syncTlsClock() {
    if (time(nullptr) >= TLS_MIN_VALID_EPOCH) return true;

    configTime(0, 0, TLS_NTP_SERVER_1, TLS_NTP_SERVER_2);
    const uint32_t startedAt = millis();
    while (time(nullptr) < TLS_MIN_VALID_EPOCH) {
        if (millis() - startedAt >= TLS_TIME_SYNC_TIMEOUT_MS) {
            Serial.println("net: TLS clock sync timed out");
            return false;
        }
        delay(100);
    }
    return true;
}

static bool beginRequest(HTTPClient& http, const String& url) {
    if (useHttps()) {
        if (!syncTlsClock()) return false;
        const char* rootCa = TLS_ROOT_CA ? TLS_ROOT_CA : kWorkersDevRootCa;
        if (rootCa[0] == '\0') {
            Serial.println("net: HTTPS root CA is not configured");
            return false;
        }

        static WiFiClientSecure secureClient;
        secureClient.setCACert(rootCa);
        return http.begin(secureClient, url);
    }
    return http.begin(url);
}

static bool getWithAuth(const char* path, String& body) {
    g_lastStatus = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    if (!beginRequest(http, String(PROXY_URL) + path)) return false;
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
    if (!beginRequest(http, String(PROXY_URL) + "/transcribe")) return 0;
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
    if (!beginRequest(http, String(PROXY_URL) + path)) return false;
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
    if (!beginRequest(http, String(PROXY_URL) + "/pet/reset")) return 0;
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
