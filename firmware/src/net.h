#pragma once
// HTTP wrappers for the bridge. Adds the bearer token, returns true on 200
// with the response body in `out`. Anything else is a failure.

#include "pet_state.h"
#include <stdint.h>

namespace net {

// GET /pet/state -> fills `out`. Returns true on success.
bool fetchPetState(PetState& out);

// HTTP status code from the last call (0 if connection failed).
int  lastStatus();

// Quick bearer-token roundtrip — used by the boot screen to show "online".
bool pingBridge();

// POST /transcribe with raw WAV bytes (audio/wav). On 200, copies the JSON
// response body into `outBuf` (up to `outBufSize` bytes, always NUL-terminated).
// Returns the HTTP status code. outTextLen receives the JSON length (excl. NUL).
int  postTranscribe(const uint8_t* wav, size_t wavSize,
                    char* outBuf, size_t outBufSize, size_t* outTextLen);

// POST /pet/reset. On 200, fetches the new state and writes it into `out`
// (which must have the same fields as /pet/state). Returns the HTTP status.
int  postReset(PetState& out);

}  // namespace net
