#include "audio_out.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace audio
{
    namespace
    {
        constexpr uint32_t kChannels = 6, kSamples = 256, kRate = 48000;
        constexpr uint32_t kRingFrames = 32;

        std::mutex s_lock;
        IXAudio2* s_xaudio = nullptr;
        IXAudio2MasteringVoice* s_master = nullptr;
        IXAudio2SourceVoice* s_source = nullptr;
        float s_ring[kRingFrames][kChannels * kSamples];
        uint32_t s_next = 0;
        bool s_tried = false, s_ok = false;
        // MCLA_VOLUME=<0..2>: output gain.
        float s_volume = 1.0f;
        uint64_t s_submitted = 0, s_underruns = 0, s_dropped = 0;
    }

    void TakeOutputStats(uint64_t& submitted, uint64_t& underruns, uint64_t& dropped)
    {
        std::lock_guard lock(s_lock);
        submitted = s_submitted;
        underruns = s_underruns;
        dropped = s_dropped;
        s_submitted = s_underruns = s_dropped = 0;
    }

    bool InitOutput()
    {
        std::lock_guard lock(s_lock);
        if (s_tried)
            return s_ok;
        s_tried = true;
        if (char v[16]; GetEnvironmentVariableA("MCLA_VOLUME", v, sizeof(v)) > 0)
            s_volume = float(atof(v));
        if (GetEnvironmentVariableA("MCLA_NO_AUDIO", nullptr, 0) > 0)
        {
            LOGI("audio: output disabled (MCLA_NO_AUDIO)");
            return false;
        }
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = XAudio2Create(&s_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr))
            hr = s_xaudio->CreateMasteringVoice(&s_master);
        if (SUCCEEDED(hr))
        {
            WAVEFORMATEXTENSIBLE fmt{};
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = kChannels;
            fmt.Format.nSamplesPerSec = kRate;
            fmt.Format.wBitsPerSample = 32;
            fmt.Format.nBlockAlign = kChannels * 4;
            fmt.Format.nAvgBytesPerSec = kRate * kChannels * 4;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = 32;
            fmt.dwChannelMask = 0x3F; // FL FR FC LFE BL BR (5.1), the console's order
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            fmt.SubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
            hr = s_xaudio->CreateSourceVoice(&s_source, &fmt.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO);
        }
        if (SUCCEEDED(hr))
            hr = s_source->Start();
        if (FAILED(hr))
        {
            LOGI("audio: XAudio2 output unavailable (%08lX); audio is silent", hr);
            if (s_xaudio)
                s_xaudio->Release();
            s_xaudio = nullptr;
            s_source = nullptr;
            return false;
        }
        if (s_volume != 1.0f)
            s_source->SetVolume(s_volume);
        LOGI("audio: XAudio2 output, 5.1 float at %u Hz, volume %.2f", kRate, s_volume);
        s_ok = true;
        return true;
    }

    int QueuedFrames()
    {
        if (!s_ok)
            return -1;
        XAUDIO2_VOICE_STATE state;
        s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return int(state.BuffersQueued);
    }

    // MCLA_AUDIO_DUMP=<seconds>: also write that much output to audio_dump.wav
    // (stereo downmix, 16-bit) next to the executable, for checking without speakers.
    static void Dump(uint32_t guestSamples)
    {
        static FILE* file = nullptr;
        static uint32_t framesLeft = [] {
            char v[16];
            return GetEnvironmentVariableA("MCLA_AUDIO_DUMP", v, sizeof(v)) > 0 ? uint32_t(atof(v) * kRate / kSamples) : 0u;
        }();
        static uint32_t written = 0;
        if (!framesLeft)
            return;
        if (!file)
        {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path(exe);
            path = path.substr(0, path.find_last_of(L'\\') + 1) + L"audio_dump.wav";
            file = _wfopen(path.c_str(), L"wb");
            if (!file)
            {
                framesLeft = 0;
                return;
            }
            uint8_t header[44] = {};
            fwrite(header, 1, 44, file);
        }
        int16_t pcm[kSamples * 2];
        for (uint32_t i = 0; i < kSamples; i++)
        {
            auto s = [&](uint32_t ch) { return LoadF32(guestSamples + (ch * kSamples + i) * 4); };
            float c = s(2) * 0.707f, l = s(0) + c + s(4) * 0.707f, r = s(1) + c + s(5) * 0.707f;
            pcm[i * 2] = int16_t(l < -1 ? -32767 : l > 1 ? 32767 : l * 32767);
            pcm[i * 2 + 1] = int16_t(r < -1 ? -32767 : r > 1 ? 32767 : r * 32767);
        }
        fwrite(pcm, sizeof(pcm), 1, file);
        written += sizeof(pcm);
        if (--framesLeft == 0)
        {
            auto put32 = [&](long at, uint32_t v) { fseek(file, at, SEEK_SET); fwrite(&v, 4, 1, file); };
            auto put16 = [&](long at, uint16_t v) { fseek(file, at, SEEK_SET); fwrite(&v, 2, 1, file); };
            fseek(file, 0, SEEK_SET);
            fwrite("RIFF", 1, 4, file);
            put32(4, 36 + written);
            fseek(file, 8, SEEK_SET);
            fwrite("WAVEfmt ", 1, 8, file);
            put32(16, 16);
            put16(20, 1);
            put16(22, 2);
            put32(24, kRate);
            put32(28, kRate * 4);
            put16(32, 4);
            put16(34, 16);
            fseek(file, 36, SEEK_SET);
            fwrite("data", 1, 4, file);
            put32(40, written);
            fclose(file);
            LOGI("audio: wrote audio_dump.wav (%u bytes)", written);
        }
    }

    void SubmitFrame(uint32_t guestSamples)
    {
        if (!guestSamples)
            return;
        Dump(guestSamples);
        if (!s_ok)
            return;
        std::lock_guard lock(s_lock);
        int queued = QueuedFrames();
        s_submitted++;
        if (queued == 0 && s_submitted > 100)
            s_underruns++; // the device ran dry before this frame arrived (audible gap)
        if (queued >= int(kRingFrames) - 1)
        {
            s_dropped++;
            return; // the title is ahead of the device: drop rather than overwrite a queued buffer
        }
        float* out = s_ring[s_next];
        s_next = (s_next + 1) % kRingFrames;
        // Channel-major big-endian input -> interleaved little-endian output.
        for (uint32_t ch = 0; ch < kChannels; ch++)
            for (uint32_t i = 0; i < kSamples; i++)
                out[i * kChannels + ch] = LoadF32(guestSamples + (ch * kSamples + i) * 4);
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = sizeof(s_ring[0]);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(out);
        s_source->SubmitSourceBuffer(&buffer);
    }
}
