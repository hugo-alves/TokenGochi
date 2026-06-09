#pragma once
// Audio capture and playback for the StopWatch.
// M5Unified's M5.Mic uses a slot-based ping-pong DMA buffer; we feed it from
// loop() and accumulate PCM into a PSRAM-backed scratch buffer. On stop we
// build a WAV (PCM, 16 kHz, 16-bit, mono) around the captured samples and
// hand the byte span to the caller.
//
// IMPORTANT: M5.Mic and M5.Speaker share the ES8311 codec and cannot be
// active at the same time. This module toggles between them. When the mic is
// active, calling chirp() will spin up the speaker and tear the mic back
// down on the way out.

#include <stdint.h>
#include <stddef.h>

namespace audio {

constexpr int    SAMPLE_RATE   = 16000;       // Hz
constexpr int    SLOT_SAMPLES  = 800;          // 50 ms; tolerates display redraw latency
constexpr int    DEFAULT_SECONDS = 10;
constexpr int    MAX_SECONDS   = 30;
constexpr int    SLOTS_PER_SECOND = SAMPLE_RATE / SLOT_SAMPLES;
constexpr int    MAX_SLOTS     = SLOTS_PER_SECOND * MAX_SECONDS;
constexpr int    TOTAL_SAMPLES = SLOT_SAMPLES * MAX_SLOTS;
constexpr size_t WAV_HEADER    = 44;
constexpr size_t MAX_WAV_BYTES = WAV_HEADER + TOTAL_SAMPLES * 2;

struct CaptureStats {
    uint32_t slots;
    uint32_t samples;
    uint32_t durationMs;
    int16_t minSample;
    int16_t maxSample;
    uint32_t peakAbs;
    uint32_t rms;
    uint32_t zeroCrossings;
};

// Allocate PSRAM once. Call from setup() before begin().
void init();

// Begin recording. Ends the speaker if it was active. Cheap to call repeatedly.
// maxSeconds is clamped to the supported device settings range.
void startRecording(uint32_t maxSeconds = DEFAULT_SECONDS);

// Try to advance the recorder by one slot. Returns:
//   1 = a new slot was queued
//   0 = the queue is full (or recording isn't running)
// Call this from loop() while the button is held.
int pumpRecording();

// Seconds elapsed since startRecording() (0 if not recording).
uint32_t elapsedSeconds();

// Milliseconds elapsed since startRecording() (0 if not recording).
uint32_t elapsedMillis();

// Current runtime capture cap.
uint32_t maxDurationSeconds();

// Stop recording, free the mic, and emit a WAV buffer (header + PCM).
// Returns false if nothing was recorded. *wavOut and *sizeOut are valid
// until the next call to startRecording() or chirp().
bool stopRecording(const uint8_t** wavOut, size_t* sizeOut);

// Stop recording, free the mic, and discard the captured PCM.
void cancelRecording();

// Play a short tone (for button press / error chirps). Blocks for `ms`.
// Re-enables the speaker and disables the mic if needed.
void chirp(uint16_t freqHz, uint16_t ms);

// True if the mic is currently the active peripheral.
bool micActive();

// Non-secret stats for the most recently assembled WAV. Does not expose audio.
const CaptureStats& lastStats();

}  // namespace audio
