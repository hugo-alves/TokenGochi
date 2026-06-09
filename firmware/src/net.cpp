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

    if (code == 200 && outBuf && outBufSize) {
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
    strncpy(out.mood, doc["mood"] | "happy", sizeof(out.mood) - 1);
    out.age_s             = doc["age_s"]             | 0;
    out.food_today        = doc["food_today"]        | 0;
    out.last_msg_ts       = doc["last_msg_ts"]       | 0;
    out.total_tokens_ever = doc["total_tokens_ever"] | 0;
    out.audio_runs_today  = doc["audio_runs_today"]  | 0;
    out.ts                = doc["ts"]                | 0;
    const char* msg = doc["last_msg"] | "";
    strncpy(out.last_msg, msg, sizeof(out.last_msg) - 1);
    JsonObject bd = doc["breakdown"];
    if (!bd.isNull()) {
        out.breakdown_claude = bd["claude"] | 0;
        out.breakdown_codex  = bd["codex"]  | 0;
    }
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
    strncpy(out.mood, mood, sizeof(out.mood) - 1);
    out.age_s              = doc["age_s"]              | 0;
    out.food_today         = doc["food_today"]         | 0;
    out.last_msg_ts        = doc["last_msg_ts"]        | 0;
    out.total_tokens_ever  = doc["total_tokens_ever"]  | 0;
    out.audio_runs_today   = doc["audio_runs_today"]   | 0;
    out.ts                 = doc["ts"]                 | 0;

    const char* msg = doc["last_msg"] | "";
    strncpy(out.last_msg, msg, sizeof(out.last_msg) - 1);

    JsonObject bd = doc["breakdown"];
    if (!bd.isNull()) {
        out.breakdown_claude = bd["claude"] | 0;
        out.breakdown_codex  = bd["codex"]  | 0;
    }
    return true;
}

}  // namespace net
