#pragma once
// Device-only transcription history. Entries live on the watch filesystem and
// are not fetched back from the bridge or Cloudflare.

#include "config.h"
#include <stddef.h>
#include <stdint.h>

namespace transcript_log {

struct Entry {
    uint32_t seq;
    uint32_t createdAtSec;
    uint32_t uptimeMs;
    uint16_t durationSecondsX10;
    char lang[TRANSCRIPT_LOG_LANG_BYTES];
    char text[TRANSCRIPT_LOG_TEXT_BYTES];
};

bool init(uint32_t nowSec);
bool available();
size_t count();

bool add(const char* text, float durationSeconds, const char* lang, uint32_t nowSec);
bool getNewest(size_t index, Entry& out);
void prune(uint32_t nowSec);

}  // namespace transcript_log
