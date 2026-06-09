#include "transcript_log.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

namespace transcript_log {

static constexpr const char* LOG_PATH = "/transcripts.bin";
static constexpr uint32_t MAGIC = 0x54474C31;  // TGL1
static constexpr uint16_t VERSION = 1;
static constexpr uint32_t RETENTION_SECONDS =
    (uint32_t)TRANSCRIPT_LOG_RETENTION_DAYS * 24UL * 60UL * 60UL;

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t nextSeq;
};

static Entry s_entries[TRANSCRIPT_LOG_MAX_ENTRIES];
static size_t s_count = 0;
static uint32_t s_nextSeq = 1;
static bool s_ready = false;

static void emptyLog() {
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    s_nextSeq = 1;
}

static void sortNewestFirst() {
    for (size_t i = 1; i < s_count; ++i) {
        Entry v = s_entries[i];
        size_t j = i;
        while (j > 0 && s_entries[j - 1].seq < v.seq) {
            s_entries[j] = s_entries[j - 1];
            --j;
        }
        s_entries[j] = v;
    }
}

static bool save() {
    if (!s_ready) return false;

    File f = LittleFS.open(LOG_PATH, "w");
    if (!f) {
        Serial.println("[tlog] save failed: open");
        return false;
    }

    Header h = {
        MAGIC,
        VERSION,
        (uint16_t)s_count,
        s_nextSeq == 0 ? 1 : s_nextSeq,
    };
    bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h);
    if (ok && s_count > 0) {
        ok = f.write((const uint8_t*)s_entries, sizeof(Entry) * s_count) ==
             sizeof(Entry) * s_count;
    }
    f.close();

    if (!ok) Serial.println("[tlog] save failed: write");
    return ok;
}

static bool isExpired(const Entry& e, uint32_t nowSec) {
    if (nowSec == 0 || e.createdAtSec == 0) return false;
    if (e.createdAtSec > nowSec) return false;
    return nowSec - e.createdAtSec > RETENTION_SECONDS;
}

bool init(uint32_t nowSec) {
    emptyLog();
    s_ready = LittleFS.begin(true);
    if (!s_ready) {
        Serial.println("[tlog] LittleFS unavailable");
        return false;
    }

    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) {
        Serial.println("[tlog] no existing transcript log");
        save();
        return true;
    }

    Header h = {};
    if (f.readBytes((char*)&h, sizeof(h)) != sizeof(h) ||
        h.magic != MAGIC ||
        h.version != VERSION) {
        Serial.println("[tlog] resetting incompatible transcript log");
        f.close();
        save();
        return true;
    }

    s_count = h.count;
    if (s_count > TRANSCRIPT_LOG_MAX_ENTRIES) s_count = TRANSCRIPT_LOG_MAX_ENTRIES;
    s_nextSeq = h.nextSeq == 0 ? 1 : h.nextSeq;

    const size_t want = sizeof(Entry) * s_count;
    const size_t got = f.readBytes((char*)s_entries, want);
    f.close();

    if (got != want) {
        Serial.println("[tlog] resetting truncated transcript log");
        emptyLog();
        save();
        return true;
    }

    sortNewestFirst();
    prune(nowSec);
    Serial.printf("[tlog] loaded entries=%u retention_days=%u\n",
                  (unsigned)s_count,
                  (unsigned)TRANSCRIPT_LOG_RETENTION_DAYS);
    return true;
}

bool available() {
    return s_ready;
}

size_t count() {
    return s_count;
}

void prune(uint32_t nowSec) {
    if (!s_ready) return;

    size_t dst = 0;
    bool changed = false;
    for (size_t src = 0; src < s_count; ++src) {
        if (dst >= TRANSCRIPT_LOG_MAX_ENTRIES || isExpired(s_entries[src], nowSec)) {
            changed = true;
            continue;
        }
        if (dst != src) {
            s_entries[dst] = s_entries[src];
            changed = true;
        }
        ++dst;
    }

    if (dst != s_count) changed = true;
    for (size_t i = dst; i < s_count; ++i) {
        memset(&s_entries[i], 0, sizeof(s_entries[i]));
    }
    s_count = dst;
    if (changed) {
        Serial.printf("[tlog] pruned entries=%u\n", (unsigned)s_count);
        save();
    }
}

bool add(const char* text, float durationSeconds, const char* lang, uint32_t nowSec) {
    if (!s_ready || !text || !*text) return false;

    prune(nowSec);

    if (s_count >= TRANSCRIPT_LOG_MAX_ENTRIES) {
        s_count = TRANSCRIPT_LOG_MAX_ENTRIES - 1;
    }
    for (size_t i = s_count; i > 0; --i) {
        s_entries[i] = s_entries[i - 1];
    }

    Entry& e = s_entries[0];
    memset(&e, 0, sizeof(e));
    e.seq = s_nextSeq++;
    if (s_nextSeq == 0) s_nextSeq = 1;
    e.createdAtSec = nowSec;
    e.uptimeMs = millis();
    if (durationSeconds < 0.0f) durationSeconds = 0.0f;
    e.durationSecondsX10 = (uint16_t)(durationSeconds * 10.0f + 0.5f);
    strncpy(e.lang, lang ? lang : "", sizeof(e.lang) - 1);
    strncpy(e.text, text, sizeof(e.text) - 1);
    s_count++;

    const bool ok = save();
    Serial.printf("[tlog] add seq=%lu entries=%u ok=%d\n",
                  (unsigned long)e.seq,
                  (unsigned)s_count,
                  ok ? 1 : 0);
    return ok;
}

bool getNewest(size_t index, Entry& out) {
    if (!s_ready || index >= s_count) return false;
    out = s_entries[index];
    return true;
}

}  // namespace transcript_log
