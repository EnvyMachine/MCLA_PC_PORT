// D3D12 device, swap chain and per-frame command recording.
//
// All recording happens on one thread (the GPU command processor). A frame's
// command list opens on first use and is submitted by Present, which also
// throttles the CPU to kFramesInFlight frames ahead of the GPU.
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>

namespace render
{
    using Microsoft::WRL::ComPtr;

    constexpr uint32_t kFramesInFlight = 3;

    bool InitDevice(HWND hwnd);
    bool DeviceReady();

    ID3D12Device* Device();
    ID3D12CommandQueue* Queue();

    // The current frame's open command list.
    ID3D12GraphicsCommandList* CommandList();

    // Index of the frame being recorded (0..kFramesInFlight-1) and a counter that
    // only increases; resources used by frame N may be reused once
    // CompletedFrame() >= N.
    uint32_t FrameSlot();
    uint64_t FrameNumber();
    uint64_t CompletedFrame();

    // The back buffer to draw the final image into this frame (RENDER_TARGET
    // state) and its RTV.
    ID3D12Resource* BackBuffer();
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv();

    // Submits the frame and presents the back buffer.
    void Present();

    // Blocks until the GPU has finished all submitted work.
    void WaitIdle();

    // Keeps a resource alive until the GPU has finished the current frame.
    void DeferRelease(ComPtr<ID3D12Resource> resource);

    // Executes the open command list now and starts a new one in the same frame
    // (used when CPU-side work must wait for GPU results).
    void Flush();

    // Average GPU time per frame and total CPU time spent waiting for the GPU
    // since the last call.
    void TakeFrameTimings(double& gpuMsPerFrame, double& fenceWaitMs);
    // MCLA_GPU_PASSES: GPU timestamp ending a pass; time since the previous marker is
    // summed under this label and logged every 300 frames.
    void GpuMarker(const char* label);
}
