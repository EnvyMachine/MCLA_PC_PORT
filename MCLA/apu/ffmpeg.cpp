#include "ffmpeg.h"
#include "../runtime/log.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <mutex>

namespace ff
{
    namespace
    {
        // FFmpeg 7.1 (libavcodec 61, libavutil 59) struct offsets, x64.
        constexpr size_t kParCodecType = 0, kParCodecId = 4, kParExtradata = 16, kParExtradataSize = 24, kParFormat = 44,
                         kParProfile = 64, kParLevel = 68, kParSarDen = 84, kParFramerateDen = 92, kParColorPrimaries = 104,
                         kParColorSpace = 112, kParChLayout = 128, kParSampleRate = 152, kParBlockAlign = 156;
        constexpr size_t kPktPts = 8, kPktDts = 16, kPktData = 24, kPktSize = 32, kPktPos = 72;
        constexpr size_t kFrameData = 0, kFrameNbSamples = 112, kFrameFormat = 116;
        constexpr size_t kCodecName = 0, kCodecType = 16, kCodecId = 20;
        constexpr int kSampleFmtFltp = 8;
        constexpr int kMediaTypeAudio = 1;
        constexpr int kPaddingSize = 64; // AV_INPUT_BUFFER_PADDING_SIZE

        struct Api
        {
            unsigned (*avcodec_version)();
            unsigned (*avutil_version)();
            const void* (*avcodec_find_decoder_by_name)(const char*);
            void* (*avcodec_alloc_context3)(const void*);
            int (*avcodec_open2)(void*, const void*, void**);
            void (*avcodec_free_context)(void**);
            int (*avcodec_send_packet)(void*, const void*);
            int (*avcodec_receive_frame)(void*, void*);
            void (*avcodec_flush_buffers)(void*);
            void* (*av_packet_alloc)();
            void (*av_packet_free)(void**);
            void* (*avcodec_parameters_alloc)();
            void (*avcodec_parameters_free)(void**);
            int (*avcodec_parameters_to_context)(void*, const void*);
            void* (*av_frame_alloc)();
            void (*av_frame_free)(void**);
            void* (*av_mallocz)(size_t);
            void (*av_channel_layout_default)(void*, int);
            void (*av_log_set_level)(int);
        };
        Api s_api;
        const void* s_codec = nullptr;
        int s_codecId = 0;

        template <typename T>
        T& At(void* base, size_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset); }
        template <typename T>
        T At(const void* base, size_t offset)
        {
            T v;
            memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(T));
            return v;
        }

        HMODULE LoadFrom(const std::filesystem::path& dir, const wchar_t* name)
        {
            return LoadLibraryExW((dir / name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }

        bool Load()
        {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const std::filesystem::path dirs[] = {
                std::filesystem::path(exe).parent_path() / "ffmpeg",
                std::filesystem::path(exe).parent_path(),
                L"C:\\Program Files\\obs-studio\\bin\\64bit",
            };
            HMODULE avutil = nullptr, avcodec = nullptr;
            for (const auto& dir : dirs)
            {
                if (!std::filesystem::exists(dir / L"avcodec-61.dll"))
                    continue;
                avutil = LoadFrom(dir, L"avutil-59.dll");
                avcodec = avutil ? LoadFrom(dir, L"avcodec-61.dll") : nullptr;
                if (avcodec)
                {
                    LOGI("ffmpeg: using %s", (dir / L"avcodec-61.dll").string().c_str());
                    break;
                }
            }
            if (!avcodec)
            {
                LOGI("ffmpeg: avcodec-61.dll not found (place FFmpeg 7.1 DLLs in the ffmpeg folder); XMA audio is silent");
                return false;
            }

            bool ok = true;
            auto bind = [&](HMODULE m, auto& fn, const char* name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
                if (!fn)
                {
                    LOGI("ffmpeg: missing %s", name);
                    ok = false;
                }
            };
#define BIND(m, name) bind(m, s_api.name, #name)
            BIND(avcodec, avcodec_version);
            BIND(avutil, avutil_version);
            BIND(avcodec, avcodec_find_decoder_by_name);
            BIND(avcodec, avcodec_alloc_context3);
            BIND(avcodec, avcodec_open2);
            BIND(avcodec, avcodec_free_context);
            BIND(avcodec, avcodec_send_packet);
            BIND(avcodec, avcodec_receive_frame);
            BIND(avcodec, avcodec_flush_buffers);
            BIND(avcodec, av_packet_alloc);
            BIND(avcodec, av_packet_free);
            BIND(avcodec, avcodec_parameters_alloc);
            BIND(avcodec, avcodec_parameters_free);
            BIND(avcodec, avcodec_parameters_to_context);
            BIND(avutil, av_frame_alloc);
            BIND(avutil, av_frame_free);
            BIND(avutil, av_mallocz);
            BIND(avutil, av_channel_layout_default);
            BIND(avutil, av_log_set_level);
#undef BIND
            if (!ok)
                return false;

            unsigned codecVersion = s_api.avcodec_version(), utilVersion = s_api.avutil_version();
            if (codecVersion >> 16 != 61 || utilVersion >> 16 != 59)
            {
                LOGI("ffmpeg: unsupported versions avcodec %u.%u avutil %u.%u (need 61.x / 59.x)", codecVersion >> 16,
                     (codecVersion >> 8) & 0xFF, utilVersion >> 16, (utilVersion >> 8) & 0xFF);
                return false;
            }
            s_api.av_log_set_level(-8); // AV_LOG_QUIET

            s_codec = s_api.avcodec_find_decoder_by_name("xma2");
            if (!s_codec || strcmp(At<const char*>(s_codec, kCodecName), "xma2") != 0 ||
                At<int>(s_codec, kCodecType) != kMediaTypeAudio)
            {
                LOGI("ffmpeg: no xma2 decoder in this build");
                return false;
            }
            s_codecId = At<int>(s_codec, kCodecId);

            // Verify the struct layouts against the documented defaults.
            void* par = s_api.avcodec_parameters_alloc();
            bool parOk = par && At<int>(par, kParCodecType) == -1 && At<int>(par, kParCodecId) == 0 &&
                         At<void*>(par, kParExtradata) == nullptr && At<int>(par, kParFormat) == -1 &&
                         At<int>(par, kParProfile) == -99 && At<int>(par, kParLevel) == -99 && At<int>(par, kParSarDen) == 1 &&
                         At<int>(par, kParFramerateDen) == 1 && At<int>(par, kParColorPrimaries) == 2 &&
                         At<int>(par, kParColorSpace) == 2 && At<int>(par, kParSampleRate) == 0;
            s_api.avcodec_parameters_free(&par);
            void* pkt = s_api.av_packet_alloc();
            bool pktOk = pkt && At<int64_t>(pkt, kPktPts) == INT64_MIN && At<int64_t>(pkt, kPktDts) == INT64_MIN &&
                         At<void*>(pkt, kPktData) == nullptr && At<int64_t>(pkt, kPktPos) == -1;
            s_api.av_packet_free(&pkt);
            void* frame = s_api.av_frame_alloc();
            bool frameOk = frame && At<int>(frame, kFrameFormat) == -1 && At<int>(frame, kFrameNbSamples) == 0;
            s_api.av_frame_free(&frame);
            if (!parOk || !pktOk || !frameOk)
            {
                LOGI("ffmpeg: unexpected struct layout (parameters %d, packet %d, frame %d)", parOk, pktOk, frameOk);
                return false;
            }
            LOGI("ffmpeg: avcodec %u.%u.%u, xma2 decoder (codec id %d)", codecVersion >> 16, (codecVersion >> 8) & 0xFF,
                 codecVersion & 0xFF, s_codecId);
            return true;
        }
    }

    bool Available()
    {
        static bool available = Load();
        return available;
    }

    XmaDecoder::XmaDecoder(int sampleRate, int channels) : sampleRate_(sampleRate), channels_(channels)
    {
        if (!Available())
            return;
        void* ctx = s_api.avcodec_alloc_context3(s_codec);
        void* par = s_api.avcodec_parameters_alloc();
        if (!ctx || !par)
        {
            s_api.avcodec_free_context(&ctx);
            s_api.avcodec_parameters_free(&par);
            return;
        }
        At<int>(par, kParCodecType) = kMediaTypeAudio;
        At<int>(par, kParCodecId) = s_codecId;
        // XMA2WAVEFORMATEX-sized extradata: one stream, channel mask 0 (layout from ch_layout).
        auto* extradata = static_cast<uint8_t*>(s_api.av_mallocz(34 + kPaddingSize));
        extradata[0] = 1;
        At<uint8_t*>(par, kParExtradata) = extradata;
        At<int>(par, kParExtradataSize) = 34;
        s_api.av_channel_layout_default(static_cast<uint8_t*>(par) + kParChLayout, channels);
        At<int>(par, kParSampleRate) = sampleRate;
        At<int>(par, kParBlockAlign) = 2048;
        int ret = s_api.avcodec_parameters_to_context(ctx, par);
        s_api.avcodec_parameters_free(&par);
        if (ret < 0 || s_api.avcodec_open2(ctx, s_codec, nullptr) < 0)
        {
            static int logged = 0;
            if (logged++ < 4)
                LOGI("ffmpeg: failed to open xma2 decoder (%d Hz, %d channels)", sampleRate, channels);
            s_api.avcodec_free_context(&ctx);
            return;
        }
        packet_ = s_api.av_packet_alloc();
        frame_ = s_api.av_frame_alloc();
        ctx_ = ctx;
    }

    XmaDecoder::~XmaDecoder()
    {
        if (frame_)
            s_api.av_frame_free(&frame_);
        if (packet_)
            s_api.av_packet_free(&packet_);
        if (ctx_)
            s_api.avcodec_free_context(&ctx_);
    }

    bool XmaDecoder::Send(const uint8_t* packet)
    {
        if (!ctx_)
            return false;
        memcpy(buffer_, packet, 2048);
        // The packet skip count chains a context's packets through an input buffer
        // shared with other streams; the caller follows it, so the decoder must see
        // consecutive packets of one stream.
        buffer_[3] = 0;
        At<uint8_t*>(packet_, kPktData) = buffer_;
        At<int>(packet_, kPktSize) = 2048;
        int ret = s_api.avcodec_send_packet(ctx_, packet_);
        At<uint8_t*>(packet_, kPktData) = nullptr;
        At<int>(packet_, kPktSize) = 0;
        if (ret < 0)
        {
            Flush();
            return false;
        }
        return true;
    }

    bool XmaDecoder::Receive(const float* planes[2], int& count)
    {
        if (s_api.avcodec_receive_frame(ctx_, frame_) < 0)
            return false;
        int format = At<int>(frame_, kFrameFormat);
        count = At<int>(frame_, kFrameNbSamples);
        if (format != kSampleFmtFltp || count <= 0 || count > 16384)
            return false;
        planes[0] = At<const float*>(frame_, kFrameData);
        planes[1] = channels_ > 1 ? At<const float*>(frame_, kFrameData + 8) : nullptr;
        return true;
    }

    void XmaDecoder::Flush()
    {
        if (ctx_)
            s_api.avcodec_flush_buffers(ctx_);
    }
}
