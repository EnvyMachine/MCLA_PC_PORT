#include "texture_cache.h"
#include "descriptors.h"
#include "device.h"
#include "screenshot.h"
#include "upload.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"
#include "../runtime/memory_watch.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

// The memory layout rules (tiled addressing, strides, packed mip tails) follow
// Xenia's gpu/texture_address.h and gpu/texture_util.cc (BSD license), which
// document them in detail; they are reimplemented here.

namespace render
{
    namespace
    {
        uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
        uint32_t Log2Floor(uint32_t v) { return 31 - uint32_t(__builtin_clz(v | 1)); }
        uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(__builtin_clz(v - 1)); }
        uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }
        int32_t SignExtend(uint32_t v, uint32_t bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }

        // ---- Formats ----

        // How guest blocks become host data.
        enum class Convert : uint8_t
        {
            kUnsupported,
            kCopy,        // same block layout on the host
            k565,         // 16bpp packed -> RGBA8
            k1555,
            k655,
            k4444,
            k10_11_11,    // -> RGBA16
            k11_11_10,
            kDepth24,     // -> R32_FLOAT
            kDepth24Float,
            kDXT3A,       // 4x4 blocks expanded to texels
            kCTX1,
            kDXT1ToRGBA8, // BC data with sizes the host cannot create
            kDXT3ToRGBA8,
            kDXT5ToRGBA8,
            kDXNToRG8,
            kDXT5AToR8,
        };

        bool ExpandsBlocks(Convert c) { return c >= Convert::kDXT3A; }

        // Swizzle of guest components to host components, 3 bits each.
        constexpr uint16_t Swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
        {
            return uint16_t(x | y << 3 | z << 6 | w << 9);
        }
        constexpr uint16_t kRRRR = Swizzle(0, 0, 0, 0), kRGGG = Swizzle(0, 1, 1, 1), kRGBB = Swizzle(0, 1, 2, 2),
                           kRGBA = Swizzle(0, 1, 2, 3);

        struct HostFormat
        {
            Convert convert = Convert::kUnsupported;
            DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN, unorm = DXGI_FORMAT_UNKNOWN, snorm = DXGI_FORMAT_UNKNOWN,
                        srgb = DXGI_FORMAT_UNKNOWN;
            uint8_t bytesPerTexel = 0; // for expanding conversions
        };

        struct FormatInfo
        {
            uint8_t blockW = 1, blockH = 1, bytesPerBlock = 0;
            bool numeric = true; // false for float and depth data: signs do not apply
            bool blockCompressed = false;
            uint16_t swizzle = kRGBA;
            HostFormat host;
            HostFormat decompressed; // for block-compressed formats when the size is not 4-aligned
            const char* name = "?";
        };

        FormatInfo s_formats[64];
        bool s_unalignedBlockTextures = false;

        void InitFormats()
        {
            auto set = [](uint32_t f, const char* name, uint8_t bw, uint8_t bh, uint8_t bpb, uint16_t swizzle, Convert c,
                          DXGI_FORMAT resource, DXGI_FORMAT unorm, DXGI_FORMAT snorm = DXGI_FORMAT_UNKNOWN,
                          DXGI_FORMAT srgb = DXGI_FORMAT_UNKNOWN) -> FormatInfo& {
                FormatInfo& i = s_formats[f];
                i.name = name;
                i.blockW = bw;
                i.blockH = bh;
                i.bytesPerBlock = bpb;
                i.swizzle = swizzle;
                i.host = { c, resource, unorm, snorm, srgb, 0 };
                return i;
            };
            constexpr DXGI_FORMAT kUnknown = DXGI_FORMAT_UNKNOWN;
            for (uint32_t f : { 2u, 8u, 9u })
                set(f, "8", 1, 1, 1, kRRRR, Convert::kCopy, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM);
            set(3, "1_5_5_5", 1, 1, 2, kRGBA, Convert::k1555, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                kUnknown, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).host.bytesPerTexel = 4;
            set(4, "5_6_5", 1, 1, 2, kRGBB, Convert::k565, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                kUnknown, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).host.bytesPerTexel = 4;
            set(5, "6_5_5", 1, 1, 2, kRGBB, Convert::k655, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                kUnknown, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).host.bytesPerTexel = 4;
            for (uint32_t f : { 6u, 14u, 50u })
                set(f, "8_8_8_8", 1, 1, 4, kRGBA, Convert::kCopy, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                    DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            for (uint32_t f : { 7u, 54u })
                set(f, "2_10_10_10", 1, 1, 4, kRGBA, Convert::kCopy, DXGI_FORMAT_R10G10B10A2_TYPELESS,
                    DXGI_FORMAT_R10G10B10A2_UNORM);
            set(10, "8_8", 1, 1, 2, kRGGG, Convert::kCopy, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM,
                DXGI_FORMAT_R8G8_SNORM);
            set(15, "4_4_4_4", 1, 1, 2, kRGBA, Convert::k4444, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                kUnknown, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).host.bytesPerTexel = 4;
            for (uint32_t f : { 16u, 55u })
                set(f, "10_11_11", 1, 1, 4, kRGBB, Convert::k10_11_11, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                    DXGI_FORMAT_R16G16B16A16_UNORM).host.bytesPerTexel = 8;
            for (uint32_t f : { 17u, 56u })
                set(f, "11_11_10", 1, 1, 4, kRGBB, Convert::k11_11_10, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                    DXGI_FORMAT_R16G16B16A16_UNORM).host.bytesPerTexel = 8;
            struct Bc
            {
                uint32_t formats[2];
                const char* name;
                uint8_t bpb;
                uint16_t swizzle;
                DXGI_FORMAT resource, unorm, srgb;
                Convert decompress;
                DXGI_FORMAT decompressedResource, decompressedUnorm, decompressedSrgb;
                uint8_t decompressedBytes;
            };
            const Bc bcs[] = {
                { { 18, 51 }, "DXT1", 8, kRGBA, DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB,
                  Convert::kDXT1ToRGBA8, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4 },
                { { 19, 52 }, "DXT2_3", 16, kRGBA, DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB,
                  Convert::kDXT3ToRGBA8, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4 },
                { { 20, 53 }, "DXT4_5", 16, kRGBA, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB,
                  Convert::kDXT5ToRGBA8, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4 },
                { { 49, 49 }, "DXN", 16, kRGGG, DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, kUnknown, Convert::kDXNToRG8,
                  DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kUnknown, 2 },
                { { 59, 59 }, "DXT5A", 8, kRRRR, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, kUnknown,
                  Convert::kDXT5AToR8, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kUnknown, 1 },
            };
            for (const Bc& bc : bcs)
            {
                for (uint32_t f : bc.formats)
                {
                    FormatInfo& i = set(f, bc.name, 4, 4, bc.bpb, bc.swizzle, Convert::kCopy, bc.resource, bc.unorm,
                                        kUnknown, bc.srgb);
                    i.blockCompressed = true;
                    i.decompressed = { bc.decompress, bc.decompressedResource, bc.decompressedUnorm, kUnknown,
                                       bc.decompressedSrgb, bc.decompressedBytes };
                }
            }
            set(22, "24_8", 1, 1, 4, kRRRR, Convert::kDepth24, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT).host.bytesPerTexel = 4;
            set(23, "24_8_FLOAT", 1, 1, 4, kRRRR, Convert::kDepth24Float, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT)
                .host.bytesPerTexel = 4;
            set(24, "16", 1, 1, 2, kRRRR, Convert::kCopy, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM);
            set(25, "16_16", 1, 1, 4, kRGGG, Convert::kCopy, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM,
                DXGI_FORMAT_R16G16_SNORM);
            set(26, "16_16_16_16", 1, 1, 8, kRGBA, Convert::kCopy, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM);
            for (uint32_t f : { 27u, 30u })
                set(f, "16_FLOAT", 1, 1, 2, kRRRR, Convert::kCopy, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT);
            for (uint32_t f : { 28u, 31u })
                set(f, "16_16_FLOAT", 1, 1, 4, kRGGG, Convert::kCopy, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT);
            for (uint32_t f : { 29u, 32u })
                set(f, "16_16_16_16_FLOAT", 1, 1, 8, kRGBA, Convert::kCopy, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT);
            set(36, "32_FLOAT", 1, 1, 4, kRRRR, Convert::kCopy, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT);
            set(37, "32_32_FLOAT", 1, 1, 8, kRGGG, Convert::kCopy, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT);
            set(38, "32_32_32_32_FLOAT", 1, 1, 16, kRGBA, Convert::kCopy, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DXGI_FORMAT_R32G32B32A32_FLOAT);
            set(58, "DXT3A", 4, 4, 8, kRRRR, Convert::kDXT3A, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM).host.bytesPerTexel = 1;
            set(60, "CTX1", 4, 4, 8, kRGGG, Convert::kCTX1, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM)
                .host.bytesPerTexel = 2;
            for (uint32_t f : { 22u, 23u, 27u, 28u, 29u, 30u, 31u, 32u, 36u, 37u, 38u })
                s_formats[f].numeric = false;
        }

        // ---- Fetch constant ----

        enum class Dimension : uint32_t { k1D, k2D, k3D, kCube };
        enum class HostDimension : uint8_t { k2D, k3D, kCube };

        struct Fetch
        {
            const uint32_t* d;
            uint32_t Type() const { return d[0] & 3; }
            uint32_t Signs() const { return (d[0] >> 2) & 0xFF; }
            uint32_t Clamp(uint32_t axis) const { return (d[0] >> (10 + axis * 3)) & 7; }
            uint32_t Pitch() const { return (d[0] >> 22) & 0x1FF; }
            bool Tiled() const { return d[0] >> 31; }
            uint32_t Format() const { return d[1] & 0x3F; }
            uint32_t Endian() const { return (d[1] >> 6) & 3; }
            bool Stacked() const { return (d[1] >> 10) & 1; }
            uint32_t BasePage() const { return (d[1] >> 12) & 0x1FFFF; }
            uint32_t FetchSwizzle() const { return (d[3] >> 1) & 0xFFF; }
            int32_t ExpAdjust() const { return SignExtend(d[3] >> 13, 6); }
            uint32_t MagFilter() const { return (d[3] >> 19) & 3; }
            uint32_t MinFilter() const { return (d[3] >> 21) & 3; }
            uint32_t MipFilter() const { return (d[3] >> 23) & 3; }
            uint32_t Aniso() const { return (d[3] >> 25) & 7; }
            uint32_t MipMinLevel() const { return (d[4] >> 2) & 0xF; }
            uint32_t MipMaxLevel() const { return (d[4] >> 6) & 0xF; }
            float LodBias() const { return float(SignExtend(d[4] >> 12, 10)) / 32.0f; }
            uint32_t BorderColor() const { return d[5] & 3; }
            Dimension Dim() const { return Dimension((d[5] >> 9) & 3); }
            bool PackedMips() const { return (d[5] >> 11) & 1; }
            uint32_t MipPage() const { return (d[5] >> 12) & 0x1FFFF; }
        };

        uint32_t PackedMipLevel(uint32_t width, uint32_t height)
        {
            uint32_t log2Size = Log2Ceil(std::min(width, height));
            return log2Size > 4 ? log2Size - 4 : 0;
        }

        // Everything about a texture's memory and host resource; zero-padded and
        // hashed whole.
        struct TextureKey
        {
            uint32_t basePage, mipPage;
            uint32_t width, height, depth; // depth: Z size (3D) or layers (stacked, cube)
            uint16_t pitch;                // base row pitch in texels / 32
            uint8_t format, endian, tiled, packed, mipMax;
            Dimension dimension;
            HostDimension hostDimension;
            bool decompress;
        };

        // ---- Tiled addressing ----

        template <typename T> T TiledCombine(T outerInnerBytes, uint32_t bank, uint32_t pipe, uint32_t yLsb)
        {
            return T(int32_t((yLsb << 4) | (pipe << 6) | (bank << 11))) | (outerInnerBytes & 0xF) |
                   (((outerInnerBytes >> 4) & 1) << 5) | (((outerInnerBytes >> 5) & 7) << 8) | ((outerInnerBytes >> 8) << 12);
        }

        int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitchBlocks, uint32_t log2Bpb)
        {
            int32_t outer = ((y >> 5) * int32_t(pitchBlocks >> 5) + (x >> 5)) << 6;
            int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
            int32_t bytes = (outer | inner) << log2Bpb;
            uint32_t bank = (y >> 4) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return TiledCombine<int32_t>(bytes, bank, pipe, y & 1);
        }

        int64_t Tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitchBlocks, uint32_t heightBlocks, uint32_t log2Bpb)
        {
            int32_t outer = ((((z >> 2) * int32_t(heightBlocks >> 4) + (y >> 4)) * int32_t(pitchBlocks >> 5)) + (x >> 5)) << 7;
            int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
            int64_t bytes = int64_t(outer | inner) << log2Bpb;
            uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (bank << 1);
            return TiledCombine<int64_t>(bytes, bank, pipe, y & 1);
        }

        uint32_t TiledUpperBound2D(uint32_t right, uint32_t bottom, uint32_t pitchBlocks, uint32_t log2Bpb)
        {
            if (!right || !bottom)
                return 0;
            uint32_t bound = uint32_t(Tiled2D(int32_t((right - 1) & ~31u), int32_t((bottom - 1) & ~31u), pitchBlocks, log2Bpb));
            return bound + (log2Bpb == 0 ? 0xA00u : log2Bpb == 1 ? 0xC00u : (0x400u << log2Bpb));
        }

        uint32_t TiledUpperBound3D(uint32_t right, uint32_t bottom, uint32_t back, uint32_t pitchBlocks,
                                   uint32_t heightBlocks, uint32_t log2Bpb)
        {
            if (!right || !bottom || !back)
                return 0;
            uint32_t yFirst = right > pitchBlocks ? 0 : (bottom - 1) & ~31u;
            uint32_t zFirst = (back - 1) & ~3u;
            int64_t bound = 0;
            for (uint32_t z = zFirst; z < back; z++)
                for (uint32_t y = yFirst; y < bottom; y++)
                    for (uint32_t x = 7;; x += 8)
                    {
                        uint32_t xLast = std::min(x, right - 1);
                        bound = std::max(bound, Tiled3D(int32_t(xLast), int32_t(y), int32_t(z), pitchBlocks, heightBlocks, log2Bpb));
                        if (xLast == right - 1)
                            break;
                    }
            return uint32_t(bound + (int64_t(1) << log2Bpb));
        }

        // Offset of a packed mip inside the mip tail, in blocks. False if the
        // level is not in the tail.
        bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, const FormatInfo& f, uint32_t mip,
                             uint32_t& x, uint32_t& y, uint32_t& z)
        {
            uint32_t log2W = Log2Ceil(width), log2H = Log2Ceil(height);
            uint32_t log2Size = std::min(log2W, log2H);
            x = y = z = 0;
            if (log2Size > 4 + mip)
                return false;
            uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
            uint32_t packedMip = mip - packedBase;
            if (packedMip < 3)
            {
                if (log2W > log2H)
                    y = 16 >> packedMip;
                else
                    x = 16 >> packedMip;
            }
            else
            {
                uint32_t offset;
                if (log2W > log2H)
                    x = offset = (1u << (log2W - packedBase)) >> (packedMip - 2);
                else
                    y = offset = (1u << (log2H - packedBase)) >> (packedMip - 2);
                if (offset < 4)
                {
                    uint32_t log2D = Log2Ceil(depth);
                    z = log2D > 1 + mip ? (log2D - mip) * 4 : 4;
                }
            }
            x >>= Log2Floor(f.blockW);
            y >>= Log2Floor(f.blockH);
            return true;
        }

        // ---- Guest layout ----

        struct GuestLevel
        {
            uint32_t pitchBlocks;  // row pitch, tile-aligned
            uint32_t heightBlocks; // block rows between Z slices
            uint32_t sliceStride;  // bytes between array slices (4 KB aligned)
            uint32_t extent;       // bytes the level's data reaches, all slices
        };

        struct GuestLayout
        {
            GuestLevel base{};
            GuestLevel mips[16]{};
            uint32_t mipOffsets[16]{};
            uint32_t mipsExtent = 0;
            uint32_t packedLevel = UINT32_MAX;
            uint32_t arraySize = 1;
            uint32_t maxLevel = 0;
        };

        GuestLayout ComputeLayout(const TextureKey& k, const FormatInfo& f)
        {
            GuestLayout l;
            uint32_t width = k.width, height = k.dimension == Dimension::k1D ? 1 : k.height;
            uint32_t depth = k.dimension == Dimension::k3D ? k.depth : 1;
            l.arraySize = k.dimension == Dimension::k2D ? k.depth : k.dimension == Dimension::kCube ? 6 : 1;
            uint32_t mipArraySize = k.dimension == Dimension::k2D && l.arraySize > 1 ? Align(l.arraySize, 4) : l.arraySize;
            l.maxLevel = std::min<uint32_t>(k.mipMax, Log2Floor(std::max({ width, height, depth })));
            l.packedLevel = k.packed ? PackedMipLevel(width, height) : UINT32_MAX;
            uint32_t bpb = f.bytesPerBlock, log2Bpb = Log2Floor(bpb);
            uint32_t maxStored = std::min(l.maxLevel, l.packedLevel);
            uint32_t loopLast = l.packedLevel == 0 ? (l.maxLevel != 0 ? 1 : 0) : maxStored;
            uint32_t mipOffset = 0;
            for (uint32_t loop = k.basePage ? 0 : 1; loop <= loopLast; loop++)
            {
                bool isBase = loop == 0;
                uint32_t level = l.packedLevel == 0 ? 0 : loop;
                GuestLevel& out = isBase ? l.base : l.mips[level];
                uint32_t pitchTexels, rowsTexels;
                if (isBase)
                {
                    pitchTexels = uint32_t(k.pitch) << 5;
                    rowsTexels = l.packedLevel == 0 ? NextPow2(height) : height;
                }
                else
                {
                    pitchTexels = std::max(NextPow2(width) >> level, 1u);
                    rowsTexels = std::max(NextPow2(height) >> level, 1u);
                }
                uint32_t pitchAlignment = 32;
                if (!k.tiled && !isBase)
                    pitchAlignment = std::max(256u / bpb, 32u);
                out.pitchBlocks = Align((pitchTexels + f.blockW - 1) / f.blockW, pitchAlignment);
                out.heightBlocks = k.dimension != Dimension::k1D ? Align((rowsTexels + f.blockH - 1) / f.blockH, 32) : 1;
                uint32_t zStride = out.pitchBlocks * bpb * out.heightBlocks;
                uint32_t sliceStride = zStride;
                if (k.dimension == Dimension::k3D)
                    sliceStride *= Align(isBase && l.packedLevel != 0 ? depth : std::max(NextPow2(depth) >> level, 1u), 4);
                out.sliceStride = Align(sliceStride, 4096);

                uint32_t xExtent, yExtent, zExtent;
                if (level == l.packedLevel)
                {
                    xExtent = yExtent = zExtent = 0;
                    for (uint32_t sub = l.packedLevel; sub <= (isBase ? 0 : l.maxLevel); sub++)
                    {
                        uint32_t px, py, pz;
                        PackedMipOffset(width, height, depth, f, sub, px, py, pz);
                        xExtent = std::max(xExtent, px + (std::max(width >> sub, 1u) + f.blockW - 1) / f.blockW);
                        yExtent = std::max(yExtent, py + (std::max(height >> sub, 1u) + f.blockH - 1) / f.blockH);
                        zExtent = std::max(zExtent, pz + std::max(depth >> sub, 1u));
                    }
                }
                else
                {
                    xExtent = (std::max(width >> level, 1u) + f.blockW - 1) / f.blockW;
                    yExtent = (std::max(height >> level, 1u) + f.blockH - 1) / f.blockH;
                    zExtent = std::max(depth >> level, 1u);
                }
                uint32_t sliceExtent;
                if (k.tiled)
                    sliceExtent = k.dimension == Dimension::k3D
                                      ? TiledUpperBound3D(xExtent, yExtent, zExtent, out.pitchBlocks, out.heightBlocks, log2Bpb)
                                      : TiledUpperBound2D(xExtent, yExtent, out.pitchBlocks, log2Bpb);
                else
                    sliceExtent = zStride * (zExtent - 1) + out.pitchBlocks * bpb * (yExtent - 1) + bpb * xExtent;
                out.extent = out.sliceStride * (l.arraySize - 1) + sliceExtent;
                if (!isBase)
                {
                    l.mipOffsets[level] = mipOffset;
                    l.mipsExtent = std::max(l.mipsExtent, mipOffset + out.extent);
                    mipOffset += out.sliceStride * mipArraySize;
                }
            }
            return l;
        }

        // Where one host mip level comes from.
        struct LevelSource
        {
            uint32_t phys;
            const GuestLevel* layout;
            uint32_t px, py, pz; // packed mip tail offset, in blocks
        };

        LevelSource SourceForLevel(const TextureKey& k, const FormatInfo& f, const GuestLayout& l, uint32_t level)
        {
            LevelSource s{};
            uint32_t depth = k.dimension == Dimension::k3D ? k.depth : 1;
            if (level == 0)
            {
                s.phys = k.basePage << 12;
                s.layout = &l.base;
            }
            else
            {
                uint32_t stored = std::min(level, l.packedLevel);
                s.phys = (k.mipPage << 12) + l.mipOffsets[stored];
                s.layout = &l.mips[stored];
            }
            if (level >= l.packedLevel)
                PackedMipOffset(k.width, k.height, depth, f, level, s.px, s.py, s.pz);
            return s;
        }

        // ---- Conversion ----

        void EndianSwap(uint8_t* data, uint32_t size, uint32_t endian)
        {
            switch (endian)
            {
            case 1: // 8in16
                for (uint32_t i = 0; i + 1 < size; i += 2)
                    std::swap(data[i], data[i + 1]);
                break;
            case 2: // 8in32
                for (uint32_t i = 0; i + 3 < size; i += 4)
                {
                    uint32_t v;
                    memcpy(&v, data + i, 4);
                    v = __builtin_bswap32(v);
                    memcpy(data + i, &v, 4);
                }
                break;
            case 3: // 16in32
                for (uint32_t i = 0; i + 3 < size; i += 4)
                {
                    uint32_t v;
                    memcpy(&v, data + i, 4);
                    v = (v >> 16) | (v << 16);
                    memcpy(data + i, &v, 4);
                }
                break;
            }
        }

        uint16_t Read16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
        uint32_t Read32(const uint8_t* p)
        {
            uint32_t v;
            memcpy(&v, p, 4);
            return v;
        }
        uint32_t Expand4(uint32_t v) { return v * 17; }
        uint32_t Expand5(uint32_t v) { return (v << 3) | (v >> 2); }
        uint32_t Expand6(uint32_t v) { return (v << 2) | (v >> 4); }
        uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

        // DXT color block -> 16 RGBA8 texels.
        void DecodeColorBlock(const uint8_t* b, uint32_t out[16], bool dxt1)
        {
            uint32_t c0 = Read16(b), c1 = Read16(b + 2), indices = Read32(b + 4);
            uint32_t r[4], g[4], bl[4], a[4] = { 255, 255, 255, 255 };
            r[0] = Expand5(c0 >> 11), g[0] = Expand6((c0 >> 5) & 63), bl[0] = Expand5(c0 & 31);
            r[1] = Expand5(c1 >> 11), g[1] = Expand6((c1 >> 5) & 63), bl[1] = Expand5(c1 & 31);
            if (!dxt1 || c0 > c1)
            {
                r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, bl[2] = (2 * bl[0] + bl[1]) / 3;
                r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, bl[3] = (bl[0] + 2 * bl[1]) / 3;
            }
            else
            {
                r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, bl[2] = (bl[0] + bl[1]) / 2;
                r[3] = g[3] = bl[3] = a[3] = 0;
            }
            for (uint32_t i = 0; i < 16; i++)
            {
                uint32_t idx = (indices >> (i * 2)) & 3;
                out[i] = Rgba(r[idx], g[idx], bl[idx], a[idx]);
            }
        }

        // DXT5/BC4 alpha block -> 16 values.
        void DecodeAlphaBlock(const uint8_t* b, uint8_t out[16])
        {
            uint32_t a[8];
            a[0] = b[0];
            a[1] = b[1];
            if (a[0] > a[1])
                for (uint32_t i = 1; i < 7; i++)
                    a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
            else
            {
                for (uint32_t i = 1; i < 5; i++)
                    a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                a[6] = 0;
                a[7] = 255;
            }
            uint64_t bits = 0;
            for (uint32_t i = 0; i < 6; i++)
                bits |= uint64_t(b[2 + i]) << (8 * i);
            for (uint32_t i = 0; i < 16; i++)
                out[i] = uint8_t(a[(bits >> (3 * i)) & 7]);
        }

        float Float24ToFloat32(uint32_t f24)
        {
            uint32_t e = (f24 >> 20) & 0xF, m = f24 & 0xFFFFF;
            if (!e)
            {
                if (!m)
                    return 0.0f;
                // Denormal: normalize.
                e = 1;
                while (!(m & 0x100000))
                {
                    m <<= 1;
                    e--;
                }
                m &= 0xFFFFF;
            }
            uint32_t bits = ((e + 127 - 15) << 23) | (m << 3);
            float f;
            memcpy(&f, &bits, 4);
            return f;
        }

        // Converts one row of guest blocks (endian-swapped) to host rows: one row,
        // or for conversions that expand blocks, up to blockH texel rows.
        void ConvertRow(Convert c, const uint8_t* src, uint32_t blocks, uint32_t bpb, uint8_t* dst, uint32_t pitch, uint32_t rows)
        {
            switch (c)
            {
            case Convert::kCopy:
                memcpy(dst, src, size_t(blocks) * bpb);
                break;
            case Convert::k565:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t v = Read16(src + i * 2);
                    uint32_t p = Rgba(Expand5(v & 31), Expand6((v >> 5) & 63), Expand5(v >> 11), 255);
                    memcpy(dst + i * 4, &p, 4);
                }
                break;
            case Convert::k1555:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t v = Read16(src + i * 2);
                    uint32_t p = Rgba(Expand5(v & 31), Expand5((v >> 5) & 31), Expand5((v >> 10) & 31), (v >> 15) ? 255 : 0);
                    memcpy(dst + i * 4, &p, 4);
                }
                break;
            case Convert::k655:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t v = Read16(src + i * 2);
                    uint32_t p = Rgba(Expand5(v & 31), Expand5((v >> 5) & 31), Expand6(v >> 10), 255);
                    memcpy(dst + i * 4, &p, 4);
                }
                break;
            case Convert::k4444:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t v = Read16(src + i * 2);
                    uint32_t p = Rgba(Expand4(v & 15), Expand4((v >> 4) & 15), Expand4((v >> 8) & 15), Expand4(v >> 12));
                    memcpy(dst + i * 4, &p, 4);
                }
                break;
            case Convert::k10_11_11:
            case Convert::k11_11_10:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t v = Read32(src + i * 4), x, y, z;
                    if (c == Convert::k10_11_11)
                        x = v & 0x7FF, y = (v >> 11) & 0x7FF, z = v >> 22;
                    else
                        x = v & 0x3FF, y = (v >> 10) & 0x7FF, z = v >> 21;
                    uint32_t xb = c == Convert::k10_11_11 ? 11 : 10, zb = c == Convert::k10_11_11 ? 10 : 11;
                    uint16_t p[4] = { uint16_t(x * 65535 / ((1u << xb) - 1)), uint16_t(y * 65535 / 2047),
                                      uint16_t(z * 65535 / ((1u << zb) - 1)), 65535 };
                    memcpy(dst + i * 8, p, 8);
                }
                break;
            case Convert::kDepth24:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    float d = float(Read32(src + i * 4) >> 8) / 16777215.0f;
                    memcpy(dst + i * 4, &d, 4);
                }
                break;
            case Convert::kDepth24Float:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    float d = Float24ToFloat32(Read32(src + i * 4) >> 8);
                    memcpy(dst + i * 4, &d, 4);
                }
                break;
            case Convert::kDXT3A:
                for (uint32_t i = 0; i < blocks; i++)
                    for (uint32_t y = 0; y < rows; y++)
                    {
                        uint32_t bits = Read16(src + i * 8 + y * 2);
                        for (uint32_t x = 0; x < 4; x++)
                            dst[y * pitch + i * 4 + x] = uint8_t(Expand4((bits >> (x * 4)) & 15));
                    }
                break;
            case Convert::kCTX1:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint32_t ends = Read32(src + i * 8), indices = Read32(src + i * 8 + 4);
                    uint32_t r[4] = { (ends >> 8) & 0xFF, ends >> 24 }, g[4] = { ends & 0xFF, (ends >> 16) & 0xFF };
                    r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3;
                    r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3;
                    for (uint32_t y = 0; y < rows; y++)
                        for (uint32_t x = 0; x < 4; x++)
                        {
                            uint32_t idx = (indices >> ((y * 4 + x) * 2)) & 3;
                            dst[y * pitch + (i * 4 + x) * 2] = uint8_t(r[idx]);
                            dst[y * pitch + (i * 4 + x) * 2 + 1] = uint8_t(g[idx]);
                        }
                }
                break;
            case Convert::kDXT1ToRGBA8:
            case Convert::kDXT3ToRGBA8:
            case Convert::kDXT5ToRGBA8:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    const uint8_t* b = src + size_t(i) * bpb;
                    uint32_t texels[16];
                    DecodeColorBlock(c == Convert::kDXT1ToRGBA8 ? b : b + 8, texels, c == Convert::kDXT1ToRGBA8);
                    if (c == Convert::kDXT3ToRGBA8)
                        for (uint32_t t = 0; t < 16; t++)
                            texels[t] = (texels[t] & 0xFFFFFF) | Expand4((Read16(b + (t / 4) * 2) >> ((t % 4) * 4)) & 15) << 24;
                    else if (c == Convert::kDXT5ToRGBA8)
                    {
                        uint8_t alpha[16];
                        DecodeAlphaBlock(b, alpha);
                        for (uint32_t t = 0; t < 16; t++)
                            texels[t] = (texels[t] & 0xFFFFFF) | uint32_t(alpha[t]) << 24;
                    }
                    for (uint32_t y = 0; y < rows; y++)
                        memcpy(dst + y * pitch + i * 16, texels + y * 4, 16);
                }
                break;
            case Convert::kDXNToRG8:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint8_t x[16], y[16];
                    DecodeAlphaBlock(src + i * 16, x);
                    DecodeAlphaBlock(src + i * 16 + 8, y);
                    for (uint32_t row = 0; row < rows; row++)
                        for (uint32_t t = 0; t < 4; t++)
                        {
                            dst[row * pitch + (i * 4 + t) * 2] = x[row * 4 + t];
                            dst[row * pitch + (i * 4 + t) * 2 + 1] = y[row * 4 + t];
                        }
                }
                break;
            case Convert::kDXT5AToR8:
                for (uint32_t i = 0; i < blocks; i++)
                {
                    uint8_t a[16];
                    DecodeAlphaBlock(src + i * 8, a);
                    for (uint32_t row = 0; row < rows; row++)
                        memcpy(dst + row * pitch + i * 4, a + row * 4, 4);
                }
                break;
            default:
                break;
            }
        }

        // ---- Textures ----

        // ---- Texture memory ----
        // Textures are placed resources in 64 MB heaps: a committed resource costs a
        // kernel allocation (~1 ms), which made streaming new areas hitch.
        struct TextureHeap
        {
            ComPtr<ID3D12Heap> heap;
            std::map<uint64_t, uint64_t> free; // offset -> size
        };
        std::vector<std::unique_ptr<TextureHeap>> s_textureHeaps;
        constexpr uint64_t kTextureHeapSize = 64ull << 20;

        struct TextureMemory
        {
            TextureHeap* heap = nullptr;
            uint64_t offset = 0, size = 0;
        };
        std::vector<std::pair<uint64_t, TextureMemory>> s_pendingFrees; // frame, memory

        bool AllocateTextureMemory(uint64_t size, uint64_t alignment, TextureMemory& out)
        {
            auto tryHeap = [&](TextureHeap& h) {
                for (auto it = h.free.begin(); it != h.free.end(); ++it)
                {
                    uint64_t start = it->first, blockSize = it->second;
                    uint64_t aligned = (start + alignment - 1) & ~(alignment - 1);
                    if (aligned + size > start + blockSize)
                        continue;
                    h.free.erase(it);
                    if (aligned > start)
                        h.free[start] = aligned - start;
                    if (aligned + size < start + blockSize)
                        h.free[aligned + size] = start + blockSize - (aligned + size);
                    out = { &h, aligned, size };
                    return true;
                }
                return false;
            };
            for (auto& h : s_textureHeaps)
                if (tryHeap(*h))
                    return true;
            if (size > kTextureHeapSize / 2)
                return false; // the caller makes a committed resource
            auto h = std::make_unique<TextureHeap>();
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = kTextureHeapSize;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            if (FAILED(Device()->CreateHeap(&hd, IID_PPV_ARGS(&h->heap))))
                return false;
            h->free[0] = kTextureHeapSize;
            s_textureHeaps.push_back(std::move(h));
            LOGI("textures: heap %zu (%llu MB)", s_textureHeaps.size(), (unsigned long long)(kTextureHeapSize >> 20));
            return tryHeap(*s_textureHeaps.back());
        }

        void FreeTextureMemory(const TextureMemory& m)
        {
            auto& free = m.heap->free;
            uint64_t start = m.offset, size = m.size;
            auto next = free.lower_bound(start);
            if (next != free.end() && next->first == start + size) // merge with the following block
            {
                size += next->second;
                next = free.erase(next);
            }
            if (next != free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == start) // merge with the preceding block
                {
                    prev->second += size;
                    return;
                }
            }
            free[start] = size;
        }

        // Memory of destroyed textures is reused once the GPU is done with them.
        void ReleasePendingTextureMemory()
        {
            uint64_t completed = CompletedFrame();
            std::erase_if(s_pendingFrees, [&](const auto& p) {
                if (p.first > completed)
                    return false;
                FreeTextureMemory(p.second);
                return true;
            });
        }

        struct Texture
        {
            TextureKey key{};
            const FormatInfo* format = nullptr;
            const HostFormat* host = nullptr;
            ComPtr<ID3D12Resource> resource;
            TextureMemory memory; // heap == null: committed resource
            bool aliasingBarrier = false; // placed in reused memory: barrier before first use
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
            uint32_t levels = 1, layers = 1;
            uint32_t ranges[2][2] = {}; // watched guest pages: base and mips, [first, end)
            bool dirty = true;
            uint64_t lastUsed = 0;
            std::vector<std::pair<uint32_t, uint32_t>> views; // view key -> descriptor
        };

        std::unordered_map<uint64_t, std::unique_ptr<Texture>> s_textures;
        // 64 KB buckets of guest physical memory -> textures with data in them.
        constexpr uint32_t kBucketShift = 16;
        std::unordered_map<uint32_t, std::vector<Texture*>> s_buckets;

        // Fetch constant + dimension -> texture and descriptor, so repeated
        // bindings skip the key computation.
        struct Binding
        {
            Texture* texture = nullptr;
            uint32_t descriptor = 0;
        };
        std::unordered_map<uint64_t, Binding> s_bindings;

        std::unordered_map<uint64_t, uint32_t> s_samplers;
        std::vector<uint32_t> s_dirtyPages;
        std::vector<uint8_t> s_row;
        uint64_t s_uploadBytes = 0;
        bool s_logTextures = false;

        template <typename Fn> void ForEachBucket(const Texture& t, Fn fn)
        {
            for (const auto& range : t.ranges)
            {
                if (range[0] == range[1])
                    continue;
                uint32_t first = range[0] >> (kBucketShift - 12), last = (range[1] - 1) >> (kBucketShift - 12);
                for (uint32_t b = first; b <= last; b++)
                    fn(b);
            }
        }

        void ProcessDirtyPages()
        {
            s_dirtyPages.clear();
            if (!mem::TakeDirtyPages(s_dirtyPages))
                return;
            for (uint32_t page : s_dirtyPages)
            {
                auto it = s_buckets.find(page >> (kBucketShift - 12));
                if (it == s_buckets.end())
                    continue;
                for (Texture* t : it->second)
                    for (const auto& range : t->ranges)
                        if (page >= range[0] && page < range[1])
                            t->dirty = true;
            }
        }

        void Transition(Texture& t, D3D12_RESOURCE_STATES next)
        {
            if (t.state == next)
                return;
            D3D12_RESOURCE_BARRIER b{};
            b.Transition.pResource = t.resource.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t.state;
            b.Transition.StateAfter = next;
            CommandList()->ResourceBarrier(1, &b);
            t.state = next;
        }

        constexpr D3D12_RESOURCE_STATES kShaderRead =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        int64_t s_frameLoadTicks = 0;
        uint64_t s_frameLoadBytes = 0;

        void LoadImpl(Texture& t);

        // Untiles, swaps and converts the guest data into the host texture.
        void Load(Texture& t)
        {
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            uint64_t before = s_uploadBytes;
            LoadImpl(t);
            QueryPerformanceCounter(&end);
            s_frameLoadTicks += end.QuadPart - start.QuadPart;
            s_frameLoadBytes += s_uploadBytes - before;
            // MCLA_DUMP_TEXTURE=<physical address>: save that texture (mip 0) as a PNG when loaded.
            static const uint32_t dumpAddress = [] {
                char v[16];
                return GetEnvironmentVariableA("MCLA_DUMP_TEXTURE", v, sizeof(v)) > 0 ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
            }();
            if (dumpAddress && t.key.basePage == (dumpAddress & 0x1FFFFFFF) >> 12)
            {
                char name[48];
                snprintf(name, sizeof(name), "texture_%08X_%ux%u_fmt%u", dumpAddress, t.key.width, t.key.height, t.key.format);
                SaveTexturePng(t.resource.Get(), t.state, name);
            }
        }

        void LoadImpl(Texture& t)
        {
            const TextureKey& k = t.key;
            const FormatInfo& f = *t.format;
            const HostFormat& h = *t.host;
            GuestLayout layout = ComputeLayout(k, f);
            // Watch first: a write racing with the reads below is caught and
            // reloads the texture next time.
            for (const auto& range : t.ranges)
                if (range[1] > range[0])
                    mem::WatchPages(range[0], range[1] - range[0]);
            t.dirty = false;

            D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
            uint32_t subresources = t.levels * (k.hostDimension == HostDimension::k3D ? 1 : t.layers);
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
            std::vector<UINT> numRows(subresources);
            std::vector<UINT64> rowSizes(subresources);
            UINT64 total = 0;
            Device()->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), numRows.data(), rowSizes.data(),
                                            &total);
            UploadSpace upload = AllocateUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            s_uploadBytes += total;

            uint32_t bpb = f.bytesPerBlock, log2Bpb = Log2Floor(bpb);
            bool is3D = k.dimension == Dimension::k3D;
            bool expands = ExpandsBlocks(h.convert);
            uint32_t rowsPerBlockRow = expands ? f.blockH : 1;
            const uint8_t* memory = g_base + 0xA0000000;
            for (uint32_t level = 0; level < t.levels; level++)
            {
                if (level == 0 ? !k.basePage : !k.mipPage)
                    continue; // never sampled (the sampler's LOD range excludes it)
                LevelSource src = SourceForLevel(k, f, layout, level);
                const GuestLevel& gl = *src.layout;
                uint32_t w = std::max(k.width >> level, 1u), hgt = std::max(k.height >> level, 1u);
                uint32_t d = is3D ? std::max(k.depth >> level, 1u) : 1;
                uint32_t blocksX = (w + f.blockW - 1) / f.blockW, blocksY = (hgt + f.blockH - 1) / f.blockH;
                s_row.resize(size_t(blocksX) * bpb);
                for (uint32_t layer = 0; layer < (is3D ? 1 : t.layers); layer++)
                {
                    uint32_t sub = level + layer * t.levels;
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[sub];
                    uint32_t pitch = fp.Footprint.RowPitch;
                    uint8_t* dstBase = upload.cpu + fp.Offset;
                    uint32_t sliceBase = src.phys + layer * gl.sliceStride;
                    for (uint32_t z = 0; z < d; z++)
                    {
                        uint8_t* dstSlice = dstBase + size_t(z) * pitch * numRows[sub];
                        for (uint32_t by = 0; by < blocksY; by++)
                        {
                            uint32_t y = by + src.py, zz = z + src.pz;
                            if (!k.tiled)
                            {
                                uint32_t offset = sliceBase + ((zz * gl.heightBlocks + y) * gl.pitchBlocks + src.px) * bpb;
                                memcpy(s_row.data(), memory + (offset & 0x1FFFFFFF), s_row.size());
                            }
                            else
                            {
                                for (uint32_t bx = 0; bx < blocksX; bx++)
                                {
                                    int64_t offset = is3D ? Tiled3D(int32_t(bx + src.px), int32_t(y), int32_t(zz), gl.pitchBlocks,
                                                                    gl.heightBlocks, log2Bpb)
                                                          : Tiled2D(int32_t(bx + src.px), int32_t(y), gl.pitchBlocks, log2Bpb);
                                    memcpy(s_row.data() + size_t(bx) * bpb,
                                           memory + ((sliceBase + uint32_t(offset)) & 0x1FFFFFFF), bpb);
                                }
                            }
                            EndianSwap(s_row.data(), uint32_t(s_row.size()), k.endian);
                            uint32_t row = by * rowsPerBlockRow;
                            if (row >= numRows[sub])
                                break;
                            ConvertRow(h.convert, s_row.data(), blocksX, bpb, dstSlice + size_t(row) * pitch, pitch,
                                       std::min(rowsPerBlockRow, numRows[sub] - row));
                        }
                    }
                }
            }

            ID3D12GraphicsCommandList* list = CommandList();
            if (t.aliasingBarrier)
            {
                // Placed in heap memory another texture may have used: the copy below
                // initializes it.
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                b.Aliasing.pResourceAfter = t.resource.Get();
                list->ResourceBarrier(1, &b);
                t.aliasingBarrier = false;
            }
            Transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32_t sub = 0; sub < subresources; sub++)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{ t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                dst.SubresourceIndex = sub;
                D3D12_TEXTURE_COPY_LOCATION srcLoc{ upload.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                srcLoc.PlacedFootprint = footprints[sub];
                srcLoc.PlacedFootprint.Offset += upload.offset;
                list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
            }
            Transition(t, kShaderRead);
        }

        // Builds the key for a fetch constant; false if it cannot be used.
        bool MakeKey(const Fetch& f, uint32_t fetchDimension, TextureKey& k)
        {
            memset(&k, 0, sizeof(k));
            const FormatInfo& info = s_formats[f.Format()];
            if (info.host.convert == Convert::kUnsupported)
                return false;
            k.dimension = f.Dim();
            uint32_t w = 1, h = 1, d = 1;
            switch (k.dimension)
            {
            case Dimension::k1D:
                w = (f.d[2] & 0xFFFFFF) + 1;
                break;
            case Dimension::k2D:
                w = (f.d[2] & 0x1FFF) + 1;
                h = ((f.d[2] >> 13) & 0x1FFF) + 1;
                d = f.Stacked() ? (f.d[2] >> 26) + 1 : 1;
                break;
            case Dimension::k3D:
                w = (f.d[2] & 0x7FF) + 1;
                h = ((f.d[2] >> 11) & 0x7FF) + 1;
                d = (f.d[2] >> 22) + 1;
                break;
            case Dimension::kCube:
                w = (f.d[2] & 0x1FFF) + 1;
                h = ((f.d[2] >> 13) & 0x1FFF) + 1;
                d = 6;
                break;
            }
            if (w > 16384 || h > 16384)
                return false;
            // The host resource follows how the shader samples it.
            switch (fetchDimension)
            {
            case 0:
            case 1:
                if (k.dimension == Dimension::k3D || k.dimension == Dimension::kCube)
                    return false;
                k.hostDimension = HostDimension::k2D;
                break;
            case 2:
                if (k.dimension != Dimension::k3D)
                    return false; // stacked 2D sampled as 3D: not yet
                k.hostDimension = HostDimension::k3D;
                break;
            default:
                if (k.dimension != Dimension::kCube)
                    return false;
                k.hostDimension = HostDimension::kCube;
                break;
            }
            k.width = w;
            k.height = h;
            k.depth = d;
            k.pitch = uint16_t(f.Pitch());
            k.format = uint8_t(f.Format());
            k.endian = uint8_t(f.Endian());
            k.tiled = f.Tiled();
            k.packed = f.PackedMips() && k.dimension != Dimension::k1D;

            // Which of the base level and the mips exist (Xenia's
            // GetSubresourcesFromFetchConstant).
            uint32_t longest = std::max(w, h);
            if (k.dimension == Dimension::k3D)
                longest = std::max(longest, d);
            uint32_t sizeMaxLevel = Log2Floor(longest);
            uint32_t basePage = f.BasePage(), mipPage = f.MipPage();
            if (!mipPage && basePage && k.packed && PackedMipLevel(w, h) == 0)
                mipPage = basePage;
            uint32_t mipMin = 0, mipMax = 0;
            if (mipPage)
            {
                mipMin = std::min(f.MipMinLevel(), sizeMaxLevel);
                mipMax = std::max(std::min(f.MipMaxLevel(), sizeMaxLevel), mipMin);
            }
            if (mipMax)
            {
                if (mipMin && basePage == mipPage)
                    basePage = 0;
            }
            else
            {
                mipPage = 0;
            }
            if (!basePage && !mipPage)
                return false;
            k.basePage = basePage;
            k.mipPage = mipPage;
            k.mipMax = uint8_t(mipMax);
            k.decompress = info.blockCompressed && !s_unalignedBlockTextures && ((w & 3) || (h & 3));
            return true;
        }

        std::unique_ptr<Texture> CreateTexture(const TextureKey& k)
        {
            const FormatInfo& f = s_formats[k.format];
            auto t = std::make_unique<Texture>();
            t->key = k;
            t->format = &f;
            t->host = k.decompress ? &f.decompressed : &f.host;
            t->levels = k.mipMax + 1u;
            t->layers = k.hostDimension == HostDimension::k3D ? k.depth : (k.hostDimension == HostDimension::kCube ? 6 : 1);

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = k.hostDimension == HostDimension::k3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                   : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = k.width;
            desc.Height = k.dimension == Dimension::k1D ? 1 : k.height;
            desc.DepthOrArraySize = uint16_t(t->layers);
            desc.MipLevels = uint16_t(t->levels);
            desc.Format = t->host->resource;
            desc.SampleDesc.Count = 1;
            // Small textures can use 4 KB placement (the runtime says when not).
            desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
            D3D12_RESOURCE_ALLOCATION_INFO info = Device()->GetResourceAllocationInfo(0, 1, &desc);
            if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
            {
                desc.Alignment = 0;
                info = Device()->GetResourceAllocationInfo(0, 1, &desc);
            }
            HRESULT hr = E_FAIL;
            if (info.SizeInBytes != UINT64_MAX && AllocateTextureMemory(info.SizeInBytes, info.Alignment, t->memory))
            {
                hr = Device()->CreatePlacedResource(t->memory.heap->heap.Get(), t->memory.offset, &desc, t->state, nullptr,
                                                    IID_PPV_ARGS(&t->resource));
                if (FAILED(hr))
                {
                    FreeTextureMemory(t->memory);
                    t->memory = {};
                }
                else
                    t->aliasingBarrier = true;
            }
            if (FAILED(hr))
            {
                desc.Alignment = 0;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                hr = Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t->state, nullptr,
                                                       IID_PPV_ARGS(&t->resource));
            }
            if (FAILED(hr))
            {
                LOGI("textures: could not create %s %ux%ux%u (%u levels): %08lX", f.name, k.width, k.height, t->layers,
                     t->levels, hr);
                return nullptr;
            }

            // The guest memory it is loaded from.
            GuestLayout layout = ComputeLayout(k, f);
            if (k.basePage && layout.base.extent)
            {
                t->ranges[0][0] = k.basePage;
                t->ranges[0][1] = std::min((k.basePage << 12) + layout.base.extent + 0xFFFu, 0x20000000u) >> 12;
            }
            if (k.mipPage && layout.mipsExtent)
            {
                t->ranges[1][0] = k.mipPage;
                t->ranges[1][1] = std::min((k.mipPage << 12) + layout.mipsExtent + 0xFFFu, 0x20000000u) >> 12;
            }
            Texture* raw = t.get();
            ForEachBucket(*raw, [&](uint32_t b) {
                auto& list = s_buckets[b];
                if (std::find(list.begin(), list.end(), raw) == list.end())
                    list.push_back(raw);
            });
            if (s_logTextures)
                LOGI("textures: %s %ux%ux%u dim %u->%u, %u levels, %s, pitch %u, endian %u%s%s, base %08X mips %08X (%u + %u bytes)",
                     f.name, k.width, k.height, k.depth, uint32_t(k.dimension), uint32_t(k.hostDimension), t->levels,
                     k.tiled ? "tiled" : "linear", uint32_t(k.pitch) << 5, k.endian, k.packed ? ", packed mips" : "",
                     k.decompress ? ", decompressed" : "", k.basePage << 12, k.mipPage << 12, layout.base.extent,
                     layout.mipsExtent);
            return t;
        }

        void DestroyTexture(Texture* t)
        {
            ForEachBucket(*t, [&](uint32_t b) {
                auto it = s_buckets.find(b);
                if (it == s_buckets.end())
                    return;
                auto& list = it->second;
                list.erase(std::remove(list.begin(), list.end(), t), list.end());
                if (list.empty())
                    s_buckets.erase(it);
            });
            for (const auto& view : t->views)
                FreeDescriptor(HeapKind::kView, view.second);
            DeferRelease(std::move(t->resource));
            if (t->memory.heap)
                s_pendingFrees.push_back({ FrameNumber(), t->memory });
            t->memory = {};
        }

        // The view for a fetch constant's swizzle and signs, plus the conversions
        // left to the shader (bits 16-29 of the result).
        uint32_t GetView(Texture& t, const Fetch& f)
        {
            const FormatInfo& info = *t.format;
            const HostFormat& h = *t.host;
            uint32_t fetchSwizzle = f.FetchSwizzle(), signs = f.Signs();
            uint32_t mapping[4], componentSign[4];
            bool anyComponent = false, allSigned = true;
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t s = (fetchSwizzle >> (i * 3)) & 7;
                if (s < 4)
                {
                    mapping[i] = (info.swizzle >> (s * 3)) & 7;
                    componentSign[i] = info.numeric ? (signs >> (s * 2)) & 3 : 0;
                    anyComponent = true;
                    allSigned = allSigned && componentSign[i] == 1;
                }
                else
                {
                    mapping[i] = s == 5 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
                    componentSign[i] = 0;
                }
            }
            DXGI_FORMAT viewFormat = h.unorm;
            uint32_t ops = 0;
            if (anyComponent && allSigned && h.snorm != DXGI_FORMAT_UNKNOWN)
            {
                viewFormat = h.snorm;
            }
            else
            {
                // An sRGB view linearizes host R, G and B: usable when exactly the
                // gamma components read those.
                bool anyGamma = false, srgbFits = h.srgb != DXGI_FORMAT_UNKNOWN;
                for (uint32_t i = 0; i < 4; i++)
                {
                    if (mapping[i] > 3)
                        continue;
                    bool gamma = componentSign[i] == 3;
                    anyGamma = anyGamma || gamma;
                    srgbFits = srgbFits && gamma == (mapping[i] < 3);
                }
                bool srgb = anyGamma && srgbFits;
                if (srgb)
                    viewFormat = h.srgb;
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t op = componentSign[i];
                    if (op == 3 && srgb)
                        op = 0;
                    ops |= op << (i * 2);
                }
            }
            uint32_t componentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
            uint32_t viewKey = componentMapping | uint32_t(viewFormat) << 16;
            uint32_t descriptor = 0;
            for (const auto& v : t.views)
                if (v.first == viewKey)
                    descriptor = v.second;
            if (!descriptor)
            {
                descriptor = AllocateDescriptor(HeapKind::kView);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = viewFormat;
                srv.Shader4ComponentMapping = componentMapping;
                switch (t.key.hostDimension)
                {
                case HostDimension::k2D:
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Texture2D.MipLevels = t.levels;
                    break;
                case HostDimension::k3D:
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    srv.Texture3D.MipLevels = t.levels;
                    break;
                case HostDimension::kCube:
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    srv.TextureCube.MipLevels = t.levels;
                    break;
                }
                Device()->CreateShaderResourceView(t.resource.Get(), &srv, CpuHandle(HeapKind::kView, descriptor));
                t.views.push_back({ viewKey, descriptor });
            }
            return descriptor | ops << 16 | (uint32_t(f.ExpAdjust()) & 0x3F) << 24;
        }

        D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp)
        {
            switch (clamp)
            {
            case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case 2: case 4: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP; // to edge; to halfway
            case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            default: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE; // mirror then clamp (edge, halfway, border)
            }
        }
    }

    void InitTextureCache()
    {
        InitFormats();
        D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8{};
        if (SUCCEEDED(Device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8))))
            s_unalignedBlockTextures = options8.UnalignedBlockTexturesSupported;
        char v[4];
        s_logTextures = GetEnvironmentVariableA("MCLA_LOG_TEXTURES", v, sizeof(v)) > 0;
        LOGI("textures: unaligned block-compressed textures %s", s_unalignedBlockTextures ? "supported" : "decompressed");
    }

    uint32_t GetTextureDescriptor(const uint32_t fetch[6], uint32_t fetchDimension)
    {
        ProcessDirtyPages();
        uint64_t bindingKey = XXH3_64bits_withSeed(fetch, 24, fetchDimension);
        auto it = s_bindings.find(bindingKey);
        if (it == s_bindings.end())
        {
            Binding binding;
            Fetch f{ fetch };
            TextureKey key;
            if (f.Type() == 2 && MakeKey(f, fetchDimension, key))
            {
                uint64_t hash = XXH3_64bits(&key, sizeof(key));
                auto found = s_textures.find(hash);
                if (found == s_textures.end())
                {
                    if (std::unique_ptr<Texture> t = CreateTexture(key))
                        found = s_textures.emplace(hash, std::move(t)).first;
                }
                if (found != s_textures.end())
                {
                    binding.texture = found->second.get();
                    binding.descriptor = GetView(*binding.texture, f);
                }
            }
            else if (f.Type() == 2)
            {
                static bool logged[64];
                if (!logged[f.Format()])
                {
                    logged[f.Format()] = true;
                    LOGI("textures: not loading format %u (%s), dimension %u sampled as %u", f.Format(),
                         s_formats[f.Format()].name, uint32_t(f.Dim()), fetchDimension);
                }
            }
            it = s_bindings.emplace(bindingKey, binding).first;
        }
        Binding& b = it->second;
        if (!b.texture)
            return 0;
        Texture& t = *b.texture;
        t.lastUsed = FrameNumber();
        if (t.dirty)
            Load(t);
        return b.descriptor;
    }

    uint32_t GetSamplerDescriptor(const uint32_t fetch[6])
    {
        Fetch f{ fetch };
        uint64_t key = uint64_t((fetch[0] >> 10) & 0x1FF) | uint64_t((fetch[3] >> 19) & 0x1FFF) << 9 |
                       uint64_t(fetch[4] & 0x3FFFFC) << 22 | uint64_t(fetch[5] & 3) << 44;
        auto it = s_samplers.find(key);
        if (it != s_samplers.end())
            return it->second;

        D3D12_SAMPLER_DESC sd{};
        uint32_t aniso = f.Aniso();
        // Filters set to "use the fetch constant" in the fetch constant itself: linear.
        bool magLinear = f.MagFilter() != 0, minLinear = f.MinFilter() != 0, mipLinear = f.MipFilter() == 1 || f.MipFilter() == 3;
        if (aniso >= 2 && aniso <= 5)
        {
            sd.Filter = D3D12_FILTER_ANISOTROPIC;
            sd.MaxAnisotropy = 1u << (aniso - 1);
        }
        else
        {
            sd.Filter = D3D12_ENCODE_BASIC_FILTER(minLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                  magLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                  mipLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                                  D3D12_FILTER_REDUCTION_TYPE_STANDARD);
            sd.MaxAnisotropy = 1;
        }
        sd.AddressU = AddressMode(f.Clamp(0));
        sd.AddressV = AddressMode(f.Clamp(1));
        sd.AddressW = AddressMode(f.Clamp(2));
        sd.MipLODBias = std::clamp(f.LodBias(), -16.0f, 15.99f);
        sd.MinLOD = float(f.MipMinLevel());
        sd.MaxLOD = f.MipFilter() == 2 ? sd.MinLOD : float(std::max(f.MipMaxLevel(), f.MipMinLevel()));
        float border = f.BorderColor() == 1 ? 1.0f : 0.0f;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = border;
        uint32_t index = AllocateDescriptor(HeapKind::kSampler);
        if (!index)
            return 0;
        Device()->CreateSampler(&sd, CpuHandle(HeapKind::kSampler, index));
        s_samplers.emplace(key, index);
        return index;
    }

    void TextureCacheEndFrame()
    {
        ReleasePendingTextureMemory();
        // Free textures unused for ~20 seconds.
        constexpr uint64_t kUnusedFrames = 1200;
        uint64_t frame = FrameNumber();
        if (frame % 120 != 0 || frame < kUnusedFrames)
            return;
        bool freed = false;
        for (auto it = s_textures.begin(); it != s_textures.end();)
        {
            if (it->second->lastUsed + kUnusedFrames < frame)
            {
                DestroyTexture(it->second.get());
                it = s_textures.erase(it);
                freed = true;
            }
            else
            {
                ++it;
            }
        }
        if (freed)
            s_bindings.clear();
    }

    double TextureLoadMsThisFrame()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(s_frameLoadTicks) * 1000.0 / double(f.QuadPart);
    }

    uint64_t TextureUploadBytesThisFrame() { return s_frameLoadBytes; }

    void ResetTextureFrameStats()
    {
        s_frameLoadTicks = 0;
        s_frameLoadBytes = 0;
    }

    uint64_t TakeTextureUploadBytes()
    {
        uint64_t v = s_uploadBytes;
        s_uploadBytes = 0;
        return v;
    }
}
