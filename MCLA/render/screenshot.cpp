#include "screenshot.h"
#include "device.h"
#include "../runtime/log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace render
{
    namespace
    {
        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
        {
            static uint32_t table[256];
            if (!table[1])
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
            crc = ~crc;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void PutBE32(std::vector<uint8_t>& out, uint32_t v)
        {
            for (int s = 24; s >= 0; s -= 8)
                out.push_back(uint8_t(v >> s));
        }

        void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
        {
            PutBE32(png, uint32_t(data.size()));
            size_t start = png.size();
            png.insert(png.end(), type, type + 4);
            png.insert(png.end(), data.begin(), data.end());
            PutBE32(png, Crc32(png.data() + start, png.size() - start));
        }

        // RGB rows -> PNG with uncompressed (stored) deflate blocks.
        std::vector<uint8_t> EncodePng(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height)
        {
            std::vector<uint8_t> raw;
            raw.reserve(size_t(width * 3 + 1) * height);
            for (uint32_t y = 0; y < height; y++)
            {
                raw.push_back(0); // filter: none
                raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3, rgb.begin() + size_t(y + 1) * width * 3);
            }
            std::vector<uint8_t> z = { 0x78, 0x01 };
            for (size_t pos = 0; pos < raw.size();)
            {
                size_t n = std::min<size_t>(65535, raw.size() - pos);
                bool last = pos + n == raw.size();
                z.push_back(last ? 1 : 0);
                z.push_back(uint8_t(n));
                z.push_back(uint8_t(n >> 8));
                z.push_back(uint8_t(~n));
                z.push_back(uint8_t(~n >> 8));
                z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
                pos += n;
            }
            uint32_t a = 1, b = 0;
            for (uint8_t v : raw)
            {
                a = (a + v) % 65521;
                b = (b + a) % 65521;
            }
            PutBE32(z, (b << 16) | a);

            std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
            std::vector<uint8_t> ihdr;
            PutBE32(ihdr, width);
            PutBE32(ihdr, height);
            ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB
            Chunk(png, "IHDR", ihdr);
            Chunk(png, "IDAT", z);
            Chunk(png, "IEND", {});
            return png;
        }

        float HalfToFloat(uint16_t h)
        {
            uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
            float v = exp == 0 ? std::ldexp(float(mant), -24) : std::ldexp(float(mant | 0x400), int(exp) - 25);
            return sign ? -v : v;
        }
    }

    static bool DumpAlpha()
    {
        static bool on = GetEnvironmentVariableA("MCLA_DUMP_ALPHA", nullptr, 0) > 0;
        return on;
    }

    uint32_t ScreenshotInterval()
    {
        static uint32_t interval = [] {
            char v[16];
            return GetEnvironmentVariableA("MCLA_SCREENSHOTS", v, sizeof(v)) > 0 ? uint32_t(atoi(v)) : 0u;
        }();
        return interval;
    }

    void SaveScreenshot(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, uint64_t frame)
    {
        char name[40];
        snprintf(name, sizeof(name), "frame_%05llu", (unsigned long long)frame);
        SaveTexturePng(texture, state, name);
    }

    void SaveTexturePng(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, const char* baseName)
    {
        D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 total = 0;
        Device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if (FAILED(Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback))))
            return;

        ID3D12GraphicsCommandList* list = CommandList();
        D3D12_RESOURCE_BARRIER b{};
        b.Transition.pResource = texture;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
            list->ResourceBarrier(1, &b);
        Flush();
        WaitIdle();

        uint8_t* data = nullptr;
        D3D12_RANGE range{ 0, size_t(total) };
        readback->Map(0, &range, reinterpret_cast<void**>(&data));
        uint32_t width = uint32_t(desc.Width), height = desc.Height;
        std::vector<uint8_t> rgb(size_t(width) * height * 3);
        float minValue = 1e30f, maxValue = -1e30f;
        double sum = 0.0;
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = data + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch;
            for (uint32_t x = 0; x < width; x++)
            {
                float c[3] = {};
                switch (desc.Format)
                {
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                {
                    uint32_t v;
                    memcpy(&v, row + x * 4, 4);
                    for (int i = 0; i < 3; i++)
                        c[i] = float((v >> (10 * i)) & 0x3FF) / 1023.0f;
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row + x * 8);
                    for (int i = 0; i < 3; i++)
                        c[i] = HalfToFloat(p[i]);
                    break;
                }
                case DXGI_FORMAT_R16G16_TYPELESS:
                case DXGI_FORMAT_R16G16_FLOAT:
                {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row + x * 4);
                    c[0] = HalfToFloat(p[0]);
                    c[1] = HalfToFloat(p[1]);
                    break;
                }
                case DXGI_FORMAT_R32_TYPELESS:
                case DXGI_FORMAT_R32_FLOAT:
                    memcpy(&c[0], row + x * 4, 4);
                    c[1] = c[2] = c[0];
                    break;
                case DXGI_FORMAT_R32G32_TYPELESS:
                case DXGI_FORMAT_R32G32_FLOAT:
                    memcpy(c, row + x * 8, 8);
                    break;
                default: // 8-bit RGBA (MCLA_DUMP_ALPHA: alpha as gray)
                    for (int i = 0; i < 3; i++)
                        c[i] = float(row[x * 4 + (DumpAlpha() ? 3 : i)]) / 255.0f;
                    break;
                }
                uint8_t* o = &rgb[(size_t(y) * width + x) * 3];
                for (int i = 0; i < 3; i++)
                {
                    minValue = std::min(minValue, c[i]);
                    maxValue = std::max(maxValue, c[i]);
                    sum += c[i];
                    o[i] = uint8_t(std::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
        readback->Unmap(0, nullptr);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto dir = std::filesystem::path(exe).parent_path() / "screenshots";
        std::filesystem::create_directories(dir);
        std::string name = std::string(baseName) + ".png";
        std::vector<uint8_t> png = EncodePng(rgb, width, height);
        if (FILE* f = _wfopen((dir / name).c_str(), L"wb"))
        {
            fwrite(png.data(), 1, png.size(), f);
            fclose(f);
        }
        LOGI("render: saved %s (%ux%u, format %d, values %g..%g, mean %g)", name.c_str(), width, height, int(desc.Format),
             minValue, maxValue, sum / (double(width) * height * 3));
    }
}
