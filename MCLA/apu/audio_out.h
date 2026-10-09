// Host audio output for the frames the title's audio engine submits
// (XAudioSubmitRenderDriverFrame): 6 channels (5.1) x 256 samples at 48 kHz,
// big-endian floats, channel-major. Played through XAudio2, which downmixes to the
// host's speaker layout.
#pragma once

#include <cstdint>

namespace audio
{
    // Creates the output (once). False when no device is available.
    bool InitOutput();
    // Queues one guest frame (guest address of 6 x 256 floats).
    void SubmitFrame(uint32_t guestSamples);
    // Frames queued but not yet played, or -1 without an output.
    int QueuedFrames();
    // Statistics since the last call: frames submitted, submits that found the
    // device starved (audible gaps), frames dropped because the queue was full.
    void TakeOutputStats(uint64_t& submitted, uint64_t& underruns, uint64_t& dropped);
}
