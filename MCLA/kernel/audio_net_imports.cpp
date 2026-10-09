// Audio (XAudio render driver) and networking imports. XMA lives in apu/xma.cpp.
// Audio: the render-driver client is called back for each 256-sample frame (48 kHz)
// the output needs, like the console's audio DMA; submitted frames go to the host
// output (apu/audio_out.cpp).
// Networking: reports no link, so the title stays offline.
#include "imports.h"
#include "../apu/audio_out.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    std::atomic<uint32_t> s_audioCallback, s_audioCallbackArg;
    std::atomic<bool> s_audioRunning;
    std::atomic<uint64_t> s_framesSubmitted;

    void AudioThread()
    {
        PPCContext& ctx = thread::AttachHostThread("MCLA audio");
        SetThreadDescription(GetCurrentThread(), L"MCLA audio");
        bool output = audio::InitOutput();
        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::microseconds(5333); // 256 samples @ 48 kHz
        constexpr int kQueueTarget = 6;                      // ~32 ms of output latency
        uint64_t callbacks = 0;
        while (s_audioRunning)
        {
            if (output)
            {
                // Ask for a frame whenever the device queue (plus frames requested but
                // not yet submitted) runs low. The timer keeps the guest going if the
                // device stalls or the title stops submitting.
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                for (;;)
                {
                    int64_t outstanding = int64_t(callbacks) - int64_t(s_framesSubmitted.load());
                    if (outstanding < 0 || outstanding > 2)
                        outstanding = outstanding < 0 ? 0 : 2;
                    if (audio::QueuedFrames() + outstanding < kQueueTarget || std::chrono::steady_clock::now() > deadline)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            else
            {
                next += period;
                std::this_thread::sleep_until(next);
            }
            if (uint32_t cb = s_audioCallback.load())
            {
                callbacks++;
                ctx.r3.u64 = s_audioCallbackArg.load();
                CallGuest(ctx, cb);
            }
        }
    }

    constexpr uint32_t kWsaENetDown = 10050;
}

GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    TRACE("XAudioRegisterRenderDriverClient");
    // callback_ptr -> { be32 callback, be32 callback_arg }
    uint32_t cbPtr = ctx.r3.u32, pDriver = ctx.r4.u32;
    s_audioCallbackArg = Load32(cbPtr + 4);
    s_audioCallback = Load32(cbPtr);
    Store32(pDriver, 0x41550000);
    LOGI("audio render client: callback %08X arg %08X", Load32(cbPtr), Load32(cbPtr + 4));
    if (!s_audioRunning.exchange(true))
        std::thread(AudioThread).detach();
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    TRACE("XAudioUnregisterRenderDriverClient");
    s_audioCallback = 0;
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    // (driver, samples*) - 6 channels x 256 big-endian float samples, channel-major.
    uint64_t n = ++s_framesSubmitted;
    if (n == 1)
        LOGI("first audio frame submitted");
    audio::SubmitFrame(ctx.r4.u32);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    TRACE("XAudioGetSpeakerConfig");
    Store32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    TRACE("XAudioGetVoiceCategoryVolume");
    StoreF32(ctx.r4.u32, 1.0f);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask)
{
    TRACE("XAudioGetVoiceCategoryVolumeChangeMask");
    Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

// ---- networking: NetDll_* take a caller id first, real arguments from r4 --------

GUEST_IMPORT(NetDll_WSAStartup)
{
    TRACE("NetDll_WSAStartup");
    if (uint32_t data = ctx.r5.u32)
    {
        memset(g_base + data, 0, 0x190);
        Store16(data + 0, uint16_t(ctx.r4.u32)); // wVersion
        Store16(data + 2, 0x0202);               // wHighVersion
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_WSACleanup) { TRACE("NetDll_WSACleanup"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_WSAGetLastError) { TRACE("NetDll_WSAGetLastError"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetStartup) { TRACE("NetDll_XNetStartup"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetCleanup) { TRACE("NetDll_XNetCleanup"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { TRACE("NetDll_XNetGetEthernetLinkStatus"); ctx.r3.u64 = 0; }

GUEST_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    TRACE("NetDll_XNetGetTitleXnAddr");
    if (ctx.r4.u32)
        memset(g_base + ctx.r4.u32, 0, 0x24);
    ctx.r3.u64 = 1; // XNET_GET_XNADDR_NONE
}

GUEST_IMPORT(NetDll_XNetGetConnectStatus) { TRACE("NetDll_XNetGetConnectStatus"); ctx.r3.u64 = 3; } // lost

GUEST_IMPORT(NetDll_socket) { TRACE("NetDll_socket"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_closesocket) { TRACE("NetDll_closesocket"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_bind) { TRACE("NetDll_bind"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_connect) { TRACE("NetDll_connect"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_listen) { TRACE("NetDll_listen"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_accept) { TRACE("NetDll_accept"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_send) { TRACE("NetDll_send"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_sendto) { TRACE("NetDll_sendto"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_recv) { TRACE("NetDll_recv"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_recvfrom) { TRACE("NetDll_recvfrom"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_select) { TRACE("NetDll_select"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_setsockopt) { TRACE("NetDll_setsockopt"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_getsockname) { TRACE("NetDll_getsockname"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll_ioctlsocket) { TRACE("NetDll_ioctlsocket"); ctx.r3.s64 = -1; }
GUEST_IMPORT(NetDll___WSAFDIsSet) { TRACE("NetDll___WSAFDIsSet"); ctx.r3.u64 = 0; }

GUEST_IMPORT(NetDll_inet_addr)
{
    TRACE("NetDll_inet_addr");
    unsigned a, b, c, d;
    const char* s = ctx.r4.u32 ? GuestPtr<const char>(ctx.r4.u32) : "";
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        ctx.r3.u64 = (a << 24) | (b << 16) | (c << 8) | d; // network order on a big-endian guest
    else
        ctx.r3.u64 = 0xFFFFFFFF; // INADDR_NONE
}

GUEST_IMPORT(NetDll_XNetConnect) { TRACE("NetDll_XNetConnect"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetQosListen) { TRACE("NetDll_XNetQosListen"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetQosLookup) { TRACE("NetDll_XNetQosLookup"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetQosRelease) { TRACE("NetDll_XNetQosRelease"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetRegisterKey) { TRACE("NetDll_XNetRegisterKey"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetUnregisterKey) { TRACE("NetDll_XNetUnregisterKey"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetServerToInAddr) { TRACE("NetDll_XNetServerToInAddr"); ctx.r3.u64 = kWsaENetDown; }
GUEST_IMPORT(NetDll_XNetUnregisterInAddr) { TRACE("NetDll_XNetUnregisterInAddr"); ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetXnAddrToInAddr) { TRACE("NetDll_XNetXnAddrToInAddr"); ctx.r3.u64 = kWsaENetDown; }
