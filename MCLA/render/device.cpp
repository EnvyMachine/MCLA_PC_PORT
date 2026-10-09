#include "device.h"
#include "window.h"
#include "../runtime/log.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace render
{
    namespace
    {
        constexpr uint32_t kBackBufferCount = 2;
        constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        ComPtr<IDXGIFactory6> s_factory;
        ComPtr<ID3D12Device> s_device;
        ComPtr<ID3D12CommandQueue> s_queue;
        ComPtr<IDXGISwapChain3> s_swapChain;
        ComPtr<ID3D12Resource> s_backBuffers[kBackBufferCount];
        ComPtr<ID3D12DescriptorHeap> s_rtvHeap;
        uint32_t s_rtvSize = 0;
        ComPtr<ID3D12CommandAllocator> s_allocators[kFramesInFlight];
        ComPtr<ID3D12GraphicsCommandList> s_list;
        HANDLE s_fenceEvent = nullptr;

        // Frame fence: signalled with the frame number when a frame is presented.
        ComPtr<ID3D12Fence> s_frameFence;
        // Submission fence: signalled after every submission (frames and flushes).
        ComPtr<ID3D12Fence> s_submitFence;
        uint64_t s_submitValue = 0;

        uint64_t s_frameNumber = 1; // frame being recorded
        std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> s_deferred; // (frame, resource)
        bool s_listOpen = false;
        bool s_allocatorReset = false; // this frame's allocator has been recycled
        bool s_backBufferInUse = false; // back buffer transitioned to RENDER_TARGET
        bool s_tearing = false;
        uint32_t s_width = 0, s_height = 0;

        // Frame timing: GPU timestamps at the start and end of each frame's work,
        // read back once the frame's slot comes round again, and the CPU time
        // spent waiting for the GPU.
        ComPtr<ID3D12QueryHeap> s_timestamps;
        ComPtr<ID3D12Resource> s_timestampReadback;
        uint64_t s_timestampFrequency = 1;
        bool s_timestampPending[kFramesInFlight] = {};
        double s_gpuMs = 0, s_fenceWaitMs = 0;
        uint32_t s_gpuFrames = 0;
        LARGE_INTEGER s_qpcFrequency{};

        // MCLA_GPU_PASSES: extra timestamps between passes (GpuMarker), summed per label.
        constexpr uint32_t kMarkersPerFrame = 1024;
        ComPtr<ID3D12QueryHeap> s_markerHeap;
        ComPtr<ID3D12Resource> s_markerReadback;
        std::vector<std::string> s_markerLabels[kFramesInFlight];
        uint32_t s_markerCount[kFramesInFlight] = {};
        std::map<std::string, double> s_passMs;
        uint32_t s_passFrames = 0;

        bool GpuPassesEnabled()
        {
            static bool on = GetEnvironmentVariableA("MCLA_GPU_PASSES", nullptr, 0) > 0;
            return on;
        }

        void CollectMarkers(uint32_t slot)
        {
            uint32_t n = s_markerCount[slot];
            s_markerCount[slot] = 0;
            if (n < 2)
                return;
            D3D12_RANGE range{ size_t(slot) * kMarkersPerFrame * 8, size_t(slot + 1) * kMarkersPerFrame * 8 };
            uint64_t* ticks = nullptr;
            if (FAILED(s_markerReadback->Map(0, &range, reinterpret_cast<void**>(&ticks))))
                return;
            const uint64_t* t = ticks + size_t(slot) * kMarkersPerFrame;
            for (uint32_t i = 1; i < n; i++)
                if (t[i] >= t[i - 1])
                    s_passMs[s_markerLabels[slot][i]] += double(t[i] - t[i - 1]) * 1000.0 / double(s_timestampFrequency);
            D3D12_RANGE none{ 0, 0 };
            s_markerReadback->Unmap(0, &none);
            if (++s_passFrames == 300)
            {
                std::vector<std::pair<double, std::string>> sorted;
                double total = 0;
                for (auto& [label, ms] : s_passMs)
                {
                    sorted.push_back({ ms / s_passFrames, label });
                    total += ms / s_passFrames;
                }
                std::sort(sorted.rbegin(), sorted.rend());
                LOGI("gpu passes: %.1f ms per frame in %zu labels", total, sorted.size());
                for (size_t i = 0; i < sorted.size() && i < 16; i++)
                    LOGI("gpu passes:   %5.2f ms  %s", sorted[i].first, sorted[i].second.c_str());
                s_passMs.clear();
                s_passFrames = 0;
            }
        }

        void Check(HRESULT hr, const char* what)
        {
            if (FAILED(hr))
            {
                LOGI("render: %s failed (%08lX)", what, hr);
                if (s_device && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET))
                    LOGI("render: device removed reason %08lX", s_device->GetDeviceRemovedReason());
                LogFlush();
                ExitProcess(1);
            }
        }

        void WaitForFence(ID3D12Fence* fence, uint64_t value)
        {
            if (fence->GetCompletedValue() < value)
            {
                LARGE_INTEGER start, end;
                QueryPerformanceCounter(&start);
                Check(fence->SetEventOnCompletion(value, s_fenceEvent), "SetEventOnCompletion");
                WaitForSingleObject(s_fenceEvent, INFINITE);
                QueryPerformanceCounter(&end);
                s_fenceWaitMs += double(end.QuadPart - start.QuadPart) * 1000.0 / double(s_qpcFrequency.QuadPart);
            }
        }

        // Adds up the GPU time of the frame that last used slot (complete now).
        void CollectTimestamps(uint32_t slot)
        {
            if (!s_timestampPending[slot])
                return;
            s_timestampPending[slot] = false;
            D3D12_RANGE range{ slot * 16, slot * 16 + 16 };
            uint64_t* ticks = nullptr;
            if (FAILED(s_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&ticks))))
                return;
            uint64_t begin = ticks[slot * 2], end = ticks[slot * 2 + 1];
            D3D12_RANGE none{ 0, 0 };
            s_timestampReadback->Unmap(0, &none);
            if (end > begin)
            {
                s_gpuMs += double(end - begin) * 1000.0 / double(s_timestampFrequency);
                s_gpuFrames++;
            }
        }

        void CreateBackBufferViews()
        {
            for (uint32_t i = 0; i < kBackBufferCount; i++)
            {
                Check(s_swapChain->GetBuffer(i, IID_PPV_ARGS(&s_backBuffers[i])), "GetBuffer");
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += i * s_rtvSize;
                s_device->CreateRenderTargetView(s_backBuffers[i].Get(), nullptr, rtv);
            }
        }

        void ResizeIfNeeded()
        {
            uint32_t w, h;
            GetClientSize(w, h);
            if (!w || !h || (w == s_width && h == s_height))
                return;
            WaitIdle();
            for (auto& b : s_backBuffers)
                b.Reset();
            Check(s_swapChain->ResizeBuffers(kBackBufferCount, w, h, kBackBufferFormat,
                                             s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                  "ResizeBuffers");
            s_width = w;
            s_height = h;
            CreateBackBufferViews();
        }

        void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            s_list->ResourceBarrier(1, &b);
        }

        void OpenList()
        {
            if (s_listOpen)
                return;
            uint32_t slot = FrameSlot();
            if (!s_allocatorReset)
            {
                // The allocator was last used kFramesInFlight frames ago.
                WaitForFence(s_frameFence.Get(), s_frameNumber > kFramesInFlight ? s_frameNumber - kFramesInFlight : 0);
                Check(s_allocators[slot]->Reset(), "allocator Reset");
                CollectTimestamps(slot);
                if (GpuPassesEnabled())
                    CollectMarkers(slot);
                Check(s_list->Reset(s_allocators[slot].Get(), nullptr), "list Reset");
                s_listOpen = true;
                s_allocatorReset = true;
                s_list->EndQuery(s_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
                if (GpuPassesEnabled())
                    GpuMarker("frame start");
                return;
            }
            Check(s_list->Reset(s_allocators[slot].Get(), nullptr), "list Reset");
            s_listOpen = true;
        }

        void Submit()
        {
            Check(s_list->Close(), "list Close");
            ID3D12CommandList* lists[] = { s_list.Get() };
            s_queue->ExecuteCommandLists(1, lists);
            s_listOpen = false;
            Check(s_queue->Signal(s_submitFence.Get(), ++s_submitValue), "Signal");
        }
    }

    bool InitDevice(HWND hwnd)
    {
        char v[8];
        UINT factoryFlags = 0;
        if (GetEnvironmentVariableA("MCLA_D3D12_DEBUG", v, sizeof(v)) > 0)
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            {
                debug->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                LOGI("render: D3D12 debug layer enabled");
            }
        }
        Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&s_factory)), "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; s_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                         DXGI_ERROR_NOT_FOUND;
             i++)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s_device))))
            {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                LOGI("render: D3D12 device on %s (%llu MB video memory)", name,
                     (unsigned long long)(desc.DedicatedVideoMemory >> 20));
                break;
            }
        }
        if (!s_device)
        {
            LOGI("render: no D3D12 (feature level 12_0) adapter found");
            return false;
        }

        if (factoryFlags & DXGI_CREATE_FACTORY_DEBUG)
        {
            // Route debug-layer messages into the log (ID3D12InfoQueue1, Windows 11).
            ComPtr<ID3D12InfoQueue1> queue;
            if (SUCCEEDED(s_device.As(&queue)))
            {
                DWORD cookie = 0;
                queue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                        static std::atomic<int> logged{ 0 };
                        if (severity <= D3D12_MESSAGE_SEVERITY_WARNING && logged++ < 200)
                            LOGI("d3d12: [%d] %s", int(id), text);
                    },
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            }
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(s_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s_queue)), "CreateCommandQueue");

        BOOL tearing = FALSE;
        s_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        s_tearing = tearing;

        GetClientSize(s_width, s_height);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = s_width;
        sd.Height = s_height;
        sd.Format = kBackBufferFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackBufferCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        Check(s_factory->CreateSwapChainForHwnd(s_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
        s_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        Check(sc1.As(&s_swapChain), "IDXGISwapChain3");

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBackBufferCount;
        Check(s_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_rtvHeap)), "CreateDescriptorHeap(RTV)");
        s_rtvSize = s_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CreateBackBufferViews();

        for (auto& a : s_allocators)
            Check(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "CreateCommandAllocator");
        Check(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_allocators[0].Get(), nullptr, IID_PPV_ARGS(&s_list)),
              "CreateCommandList");
        s_list->Close();
        Check(s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_frameFence)), "CreateFence");
        Check(s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_submitFence)), "CreateFence");
        s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        QueryPerformanceFrequency(&s_qpcFrequency);
        s_queue->GetTimestampFrequency(&s_timestampFrequency);
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = kFramesInFlight * 2;
        Check(s_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&s_timestamps)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kFramesInFlight * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(s_device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&s_timestampReadback)),
              "CreateCommittedResource(timestamps)");

        if (GpuPassesEnabled())
        {
            qhd.Count = kFramesInFlight * kMarkersPerFrame;
            Check(s_device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&s_markerHeap)), "CreateQueryHeap(markers)");
            rd.Width = uint64_t(kFramesInFlight) * kMarkersPerFrame * 8;
            Check(s_device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&s_markerReadback)),
                  "CreateCommittedResource(markers)");
        }

        LOGI("render: swap chain %ux%u%s", s_width, s_height, s_tearing ? " (tearing supported, off unless MCLA_TEARING)" : "");
        return true;
    }

    bool DeviceReady() { return s_device != nullptr; }
    ID3D12Device* Device() { return s_device.Get(); }
    ID3D12CommandQueue* Queue() { return s_queue.Get(); }
    uint32_t FrameSlot() { return uint32_t(s_frameNumber % kFramesInFlight); }
    uint64_t FrameNumber() { return s_frameNumber; }
    uint64_t CompletedFrame() { return s_frameFence->GetCompletedValue(); }

    ID3D12GraphicsCommandList* CommandList()
    {
        OpenList();
        return s_list.Get();
    }

    ID3D12Resource* BackBuffer()
    {
        ID3D12Resource* back = s_backBuffers[s_swapChain->GetCurrentBackBufferIndex()].Get();
        CommandList();
        if (!s_backBufferInUse)
        {
            Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s_backBufferInUse = true;
        }
        return back;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += s_swapChain->GetCurrentBackBufferIndex() * s_rtvSize;
        return rtv;
    }

    void Flush()
    {
        if (!s_listOpen)
            return;
        Submit();
        OpenList();
    }

    void Present()
    {
        if (!s_backBufferInUse)
        {
            // Nothing drew the final image this frame: show a dark blue placeholder.
            BackBuffer();
            const float placeholder[4] = { 0.02f, 0.03f, 0.08f, 1 };
            s_list->ClearRenderTargetView(BackBufferRtv(), placeholder, 0, nullptr);
        }
        Transition(s_backBuffers[s_swapChain->GetCurrentBackBufferIndex()].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
        s_backBufferInUse = false;
        uint32_t slot = FrameSlot();
        if (GpuPassesEnabled() && s_markerCount[slot])
        {
            GpuMarker("present (final passes)");
            s_list->ResolveQueryData(s_markerHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarkersPerFrame,
                                     s_markerCount[slot], s_markerReadback.Get(), uint64_t(slot) * kMarkersPerFrame * 8);
        }
        s_list->EndQuery(s_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        s_list->ResolveQueryData(s_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, s_timestampReadback.Get(),
                                 slot * 16);
        s_timestampPending[slot] = true;
        Submit();

        // The title paces itself (30 fps from its vblank waits), so present without
        // waiting; flip-model presents still land on a display refresh (no tearing)
        // unless MCLA_TEARING is set.
        static const bool tearing = s_tearing && GetEnvironmentVariableA("MCLA_TEARING", nullptr, 0) > 0;
        Check(s_swapChain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0), "Present");
        Check(s_queue->Signal(s_frameFence.Get(), s_frameNumber), "Signal");
        s_frameNumber++;
        s_allocatorReset = false;
        ResizeIfNeeded();

        uint64_t completed = CompletedFrame();
        std::erase_if(s_deferred, [&](const auto& d) { return d.first <= completed; });
    }

    void DeferRelease(ComPtr<ID3D12Resource> resource)
    {
        s_deferred.emplace_back(s_frameNumber, std::move(resource));
    }

    void WaitIdle()
    {
        if (s_queue)
            WaitForFence(s_submitFence.Get(), s_submitValue);
    }

    void GpuMarker(const char* label)
    {
        if (!GpuPassesEnabled() || !s_listOpen)
            return;
        uint32_t slot = FrameSlot();
        uint32_t& n = s_markerCount[slot];
        if (n >= kMarkersPerFrame)
            return;
        if (s_markerLabels[slot].size() <= n)
            s_markerLabels[slot].resize(n + 1);
        s_markerLabels[slot][n] = label;
        s_list->EndQuery(s_markerHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kMarkersPerFrame + n);
        n++;
    }

    void TakeFrameTimings(double& gpuMsPerFrame, double& fenceWaitMs)
    {
        gpuMsPerFrame = s_gpuFrames ? s_gpuMs / s_gpuFrames : 0.0;
        fenceWaitMs = s_fenceWaitMs;
        s_gpuMs = s_fenceWaitMs = 0;
        s_gpuFrames = 0;
    }
}
