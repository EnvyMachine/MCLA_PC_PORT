// Development screenshots: MCLA_SCREENSHOTS=<n> saves the presented front buffer
// every n frames to <exe dir>/screenshots/frame_<number>.png.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace render
{
    // Interval in frames, 0 if disabled.
    uint32_t ScreenshotInterval();

    // Reads back `texture` (RGBA8, RGB10A2 or RGBA16F; in `state`, which it is
    // returned to) and writes it as a PNG. Stalls until the GPU is idle.
    void SaveScreenshot(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, uint64_t frame);

    // The same for any texture (also R32F, RG16F, RG32F), saved as <name>.png in
    // the screenshots folder; logs the value range (float data is clamped in the PNG).
    void SaveTexturePng(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, const char* name);
}
