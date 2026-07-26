#include "audio.h"
#include "device_settings.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace audio {

static int16_t* s_pcm = nullptr;
static uint32_t s_slotIdx = 0;
static uint32_t s_startMs = 0;
static uint32_t s_maxSeconds = DEFAULT_SECONDS;
static bool s_recording = false;
static bool s_micActive = false;
static uint8_t s_volumePercent = device_settings::VOLUME_PERCENT_DEFAULT;
static CaptureStats s_lastStats = {};

static constexpr uint32_t SLOT_MS = (SLOT_SAMPLES * 1000UL) / SAMPLE_RATE;
static constexpr uint32_t PRE_ROLL_MS = 300;
static constexpr uint32_t TRAIL_MS = 250;
static constexpr uint32_t SILENCE_AUTO_STOP_MS = 900;
static constexpr uint32_t MIN_SPEECH_MS = 300;
static constexpr uint32_t VOICE_RMS_THRESHOLD = 220;
static constexpr uint32_t VOICE_PEAK_THRESHOLD = 1000;

static uint32_t s_analyzedSlots = 0;
static uint32_t s_firstVoiceSlot = 0;
static uint32_t s_lastVoiceSlot = 0;
static uint32_t s_voiceSlots = 0;
static bool s_seenVoice = false;
static bool s_autoStopReady = false;

// WAV lives in PSRAM; we keep the assembled file alive until the next start.
static uint8_t* s_wav = nullptr;
static size_t   s_wavSize = 0;

static void writeLe16(uint8_t* p, int16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void writeLe32(uint8_t* p, int32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t clampSeconds(uint32_t seconds) {
    if (seconds <= 10) return 10;
    if (seconds <= 20) return 20;
    return 30;
}

static uint32_t maxSlots() {
    return SLOTS_PER_SECOND * s_maxSeconds;
}

static uint32_t slotsForMs(uint32_t ms) {
    return (ms + SLOT_MS - 1) / SLOT_MS;
}

static void analyzeSlot(uint32_t slotIdx) {
    if (!s_pcm || slotIdx >= maxSlots()) return;
    const int16_t* slot = s_pcm + slotIdx * SLOT_SAMPLES;
    uint32_t peakAbs = 0;
    uint64_t sumSquares = 0;
    for (size_t i = 0; i < SLOT_SAMPLES; ++i) {
        const int16_t v = slot[i];
        const int32_t absV = v < 0 ? -(int32_t)v : (int32_t)v;
        if ((uint32_t)absV > peakAbs) peakAbs = (uint32_t)absV;
        sumSquares += (uint64_t)absV * (uint64_t)absV;
    }
    const uint32_t rms = (uint32_t)sqrt((double)sumSquares / (double)SLOT_SAMPLES);
    const bool voiced = rms >= VOICE_RMS_THRESHOLD || peakAbs >= VOICE_PEAK_THRESHOLD;
    if (voiced) {
        if (!s_seenVoice) {
            const uint32_t preSlots = slotsForMs(PRE_ROLL_MS);
            s_firstVoiceSlot = slotIdx > preSlots ? slotIdx - preSlots : 0;
            s_seenVoice = true;
        }
        s_lastVoiceSlot = slotIdx;
        s_voiceSlots++;
    }

    if (s_seenVoice) {
        const uint32_t silenceSlots = slotIdx > s_lastVoiceSlot ? slotIdx - s_lastVoiceSlot : 0;
        const uint32_t speechMs = s_voiceSlots * SLOT_MS;
        if (speechMs >= MIN_SPEECH_MS && silenceSlots * SLOT_MS >= SILENCE_AUTO_STOP_MS) {
            s_autoStopReady = true;
        }
    }
}

static void analyzeCompletedSlots() {
    if (!s_recording || !s_micActive) return;
    const int pending = M5.Mic.isRecording();
    uint32_t safeSlots = s_slotIdx;
    if (pending > 0 && safeSlots > (uint32_t)pending) {
        safeSlots -= (uint32_t)pending;
    } else if (pending > 0) {
        safeSlots = 0;
    }
    while (s_analyzedSlots < safeSlots) {
        analyzeSlot(s_analyzedSlots++);
    }
}

static void updateStats(size_t rawSamples, size_t startSample, size_t totalSamples, bool enoughSpeech) {
    s_lastStats = {};
    s_lastStats.rawSamples = rawSamples;
    s_lastStats.rawDurationMs = (uint32_t)((rawSamples * 1000UL) / SAMPLE_RATE);
    s_lastStats.slots = s_slotIdx;
    s_lastStats.samples = totalSamples;
    s_lastStats.durationMs = (uint32_t)((totalSamples * 1000UL) / SAMPLE_RATE);
    s_lastStats.trimStartMs = (uint32_t)((startSample * 1000UL) / SAMPLE_RATE);
    s_lastStats.speechMs = s_voiceSlots * SLOT_MS;
    s_lastStats.voiceSlots = s_voiceSlots;
    s_lastStats.speechDetected = enoughSpeech;
    s_lastStats.trimmed = startSample > 0 || totalSamples < rawSamples;
    if (!s_pcm || totalSamples == 0) return;

    int16_t minSample = INT16_MAX;
    int16_t maxSample = INT16_MIN;
    uint32_t peakAbs = 0;
    uint32_t zeroCrossings = 0;
    uint64_t sumSquares = 0;
    int16_t prev = s_pcm[startSample];

    for (size_t i = 0; i < totalSamples; ++i) {
        int16_t v = s_pcm[startSample + i];
        if (v < minSample) minSample = v;
        if (v > maxSample) maxSample = v;
        int32_t absV = v < 0 ? -(int32_t)v : (int32_t)v;
        if ((uint32_t)absV > peakAbs) peakAbs = (uint32_t)absV;
        sumSquares += (uint64_t)absV * (uint64_t)absV;
        if (i > 0 && ((prev < 0 && v >= 0) || (prev >= 0 && v < 0))) {
            zeroCrossings++;
        }
        prev = v;
    }

    s_lastStats.minSample = minSample;
    s_lastStats.maxSample = maxSample;
    s_lastStats.peakAbs = peakAbs;
    s_lastStats.rms = (uint32_t)sqrt((double)sumSquares / (double)totalSamples);
    s_lastStats.zeroCrossings = zeroCrossings;
}

static void endMic() {
    if (s_micActive) {
        M5.Mic.end();
        s_micActive = false;
    }
}

static void beginSpeaker() {
    if (!M5.Speaker.isRunning()) {
        M5.Speaker.begin();
    }
}

static void muxToMic() {
    if (M5.Speaker.isRunning()) {
        M5.Speaker.end();
    }
    if (!s_micActive) {
        M5.Mic.begin();
        s_micActive = true;
    }
}

void init() {
    if (!s_pcm) {
        s_pcm = (int16_t*)heap_caps_malloc(TOTAL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_pcm) {
            s_pcm = (int16_t*)malloc(TOTAL_SAMPLES * sizeof(int16_t));
        }
        memset(s_pcm, 0, TOTAL_SAMPLES * sizeof(int16_t));
    }
    if (!s_wav) {
        s_wav = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_wav) {
            s_wav = (uint8_t*)malloc(MAX_WAV_BYTES);
        }
    }
    if (M5.Speaker.isRunning()) {
        M5.Speaker.end();
    }
}

void startRecording(uint32_t maxSeconds) {
    s_maxSeconds = clampSeconds(maxSeconds);
    s_slotIdx = 0;
    s_analyzedSlots = 0;
    s_firstVoiceSlot = 0;
    s_lastVoiceSlot = 0;
    s_voiceSlots = 0;
    s_seenVoice = false;
    s_autoStopReady = false;
    s_startMs = millis();
    s_wavSize = 0;
    muxToMic();
    s_recording = true;
}

int pumpRecording() {
    if (!s_recording || !s_micActive) return 0;
    int queued = 0;
    const uint32_t slotLimit = maxSlots();
    while (s_slotIdx < slotLimit && M5.Mic.isRecording() < 2) {
        int16_t* slot = s_pcm + s_slotIdx * SLOT_SAMPLES;
        if (!M5.Mic.record(slot, SLOT_SAMPLES, SAMPLE_RATE)) break;
        s_slotIdx++;
        queued++;
    }
    analyzeCompletedSlots();
    return queued;
}

uint32_t elapsedSeconds() {
    if (!s_recording) return 0;
    return (millis() - s_startMs) / 1000;
}

uint32_t elapsedMillis() {
    if (!s_recording) return 0;
    return millis() - s_startMs;
}

uint32_t maxDurationSeconds() {
    return s_maxSeconds;
}

bool autoStopReady() {
    analyzeCompletedSlots();
    return s_autoStopReady;
}

bool stopRecording(const uint8_t** wavOut, size_t* sizeOut) {
    if (M5.Mic.isRecording()) {
        while (M5.Mic.isRecording()) delay(5);
    }
    while (s_analyzedSlots < s_slotIdx) {
        analyzeSlot(s_analyzedSlots++);
    }
    s_recording = false;
    endMic();

    const size_t rawSamples = s_slotIdx * SLOT_SAMPLES;
    const bool enoughSpeech = s_seenVoice && s_voiceSlots * SLOT_MS >= MIN_SPEECH_MS;
    size_t startSlot = 0;
    size_t endSlot = s_slotIdx;
    if (enoughSpeech) {
        startSlot = s_firstVoiceSlot;
        const size_t trailSlots = slotsForMs(TRAIL_MS);
        endSlot = (size_t)s_lastVoiceSlot + trailSlots + 1;
        if (endSlot > s_slotIdx) endSlot = s_slotIdx;
        if (endSlot <= startSlot) endSlot = startSlot + 1;
    }
    const size_t startSample = startSlot * SLOT_SAMPLES;
    const size_t totalSamples = (endSlot - startSlot) * SLOT_SAMPLES;
    updateStats(rawSamples, startSample, totalSamples, enoughSpeech);
    if (rawSamples == 0 || totalSamples == 0) {
        *wavOut = nullptr;
        *sizeOut = 0;
        return false;
    }

    // Build the 44-byte WAV header in place.
    uint32_t byteRate   = SAMPLE_RATE * 1 * 2;          // 16-bit mono
    uint32_t dataBytes  = totalSamples * 2;
    uint32_t riffBytes  = 36 + dataBytes;
    uint8_t* h = s_wav;
    memcpy(h,     "RIFF", 4);     writeLe32(h + 4,  riffBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);    writeLe32(h + 16, 16);
    writeLe16(h + 20, 1);         // PCM
    writeLe16(h + 22, 1);         // mono
    writeLe32(h + 24, SAMPLE_RATE);
    writeLe32(h + 28, byteRate);
    writeLe16(h + 32, 2);         // block align
    writeLe16(h + 34, 16);        // bits per sample
    memcpy(h + 36, "data", 4);    writeLe32(h + 40, dataBytes);
    memcpy(h + 44, s_pcm + startSample, dataBytes);

    s_wavSize = 44 + dataBytes;
    *wavOut = s_wav;
    *sizeOut = s_wavSize;
    return true;
}

void cancelRecording() {
    s_recording = false;
    if (M5.Mic.isRecording()) {
        while (M5.Mic.isRecording()) delay(5);
    }
    endMic();
    s_slotIdx = 0;
    s_analyzedSlots = 0;
    s_voiceSlots = 0;
    s_seenVoice = false;
    s_autoStopReady = false;
    s_wavSize = 0;
}

void chirp(uint16_t freqHz, uint16_t ms) {
    endMic();
    beginSpeaker();
    M5.Speaker.setVolume(device_settings::volumeToHardware(s_volumePercent));
    M5.Speaker.tone(freqHz, ms);
    delay(ms + 20);
    M5.Speaker.end();
}

void setVolumePercent(uint8_t volumePercent) {
    s_volumePercent = device_settings::clampPercent(volumePercent);
    if (M5.Speaker.isRunning()) {
        M5.Speaker.setVolume(device_settings::volumeToHardware(s_volumePercent));
    }
}

uint8_t volumePercent() { return s_volumePercent; }

bool micActive() { return s_micActive; }

bool lastClipHasSpeech() { return s_lastStats.speechDetected; }

const CaptureStats& lastStats() { return s_lastStats; }

}  // namespace audio
