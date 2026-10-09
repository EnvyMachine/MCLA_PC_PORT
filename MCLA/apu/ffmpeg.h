// Minimal run-time binding to FFmpeg's XMA2 decoder (libavcodec 61 / FFmpeg 7.1).
//
// The DLLs are loaded from <exe>\ffmpeg\ (or an OBS Studio install) instead of being
// linked, and the handful of struct fields used are accessed by offset. The offsets
// are verified against known default values at start-up; anything unexpected
// disables decoding (XMA contexts then produce silence).
#pragma once

#include <cstdint>

namespace ff
{
    // Loads and verifies FFmpeg once. Returns false if XMA2 decoding is unavailable.
    bool Available();

    // One XMA2 stream (1 or 2 channels).
    class XmaDecoder
    {
    public:
        XmaDecoder(int sampleRate, int channels);
        ~XmaDecoder();
        XmaDecoder(const XmaDecoder&) = delete;
        XmaDecoder& operator=(const XmaDecoder&) = delete;

        bool Ok() const { return ctx_ != nullptr; }
        int SampleRate() const { return sampleRate_; }
        int Channels() const { return channels_; }

        // Decodes one 2048-byte XMA packet. For each decoded block, onSamples gets
        // planar float channels and the sample count. Returns false on a decode error
        // (the decoder is flushed and can continue with the next packet).
        template <typename F>
        bool DecodePacket(const uint8_t* packet, F&& onSamples)
        {
            if (!Send(packet))
                return false;
            const float* planes[2];
            int count;
            while (Receive(planes, count))
                onSamples(planes, count);
            return true;
        }

        // Forgets partial frames (call on a discontinuity: seek, loop, new sound).
        void Flush();

    private:
        bool Send(const uint8_t* packet);
        bool Receive(const float* planes[2], int& count);

        void* ctx_ = nullptr;
        void* packet_ = nullptr;
        void* frame_ = nullptr;
        int sampleRate_, channels_;
        alignas(16) uint8_t buffer_[2048 + 64] = {};
    };
}
