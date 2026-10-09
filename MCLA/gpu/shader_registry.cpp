#include "shader_registry.h"
#include "microcode.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"
#include "../runtime/memory.h"
#include "../runtime/memory_watch.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace gpu
{
    namespace
    {
        // Container header fields (big-endian dwords).
        constexpr uint32_t kFlags = 0x00, kVirtualSize = 0x04, kPhysicalSize = 0x08, kShaderOffset = 0x18;
        // D3D shader objects: base of the physical part (the copied microcode). D3D
        // loads the program at base + Shader::physicalOffset (sub_82424670).
        constexpr uint32_t kPixelObjectMicrocode = 0x18, kVertexObjectMicrocode = 0x20;

        std::shared_mutex s_lock;
        std::unordered_map<uint64_t, std::unique_ptr<GuestShader>> s_byHash;
        std::unordered_map<uint32_t, GuestShader*> s_byMicrocode;
        std::unordered_map<uint32_t, GuestShader*> s_byInlineCopy;
        std::unordered_map<uint64_t, GuestShader*> s_byProgram[2]; // [isPixel], MicrocodeHashIgnoringVertexFetches
        std::unordered_map<uint64_t, std::unique_ptr<GuestShader>> s_raw[2]; // [isPixel], same hash

        std::filesystem::path DumpDirectory()
        {
            char v[8];
            if (GetEnvironmentVariableA("MCLA_DUMP_SHADERS", v, sizeof(v)) == 0)
                return {};
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            // build/<config>/MCLA/MCLA.exe -> project root
            auto dir = std::filesystem::path(exe).parent_path().parent_path().parent_path().parent_path() / "shaders" / "dump";
            std::filesystem::create_directories(dir);
            return dir;
        }
    }

    void RegisterShader(uint32_t containerAddress, uint32_t shaderObject)
    {
        if (!containerAddress || !shaderObject)
            return;
        uint32_t flags = Load32(containerAddress + kFlags);
        if ((flags & 0xFFFFFF00) != 0x102A1100)
        {
            LOGI("shaders: object %08X created from %08X without a container header (%08X)", shaderObject,
                 containerAddress, flags);
            return;
        }
        uint32_t virtualSize = Load32(containerAddress + kVirtualSize);
        uint32_t physicalSize = Load32(containerAddress + kPhysicalSize);
        bool isPixel = (flags & 1) == 0;
        uint32_t physicalBase = Load32(shaderObject + (isPixel ? kPixelObjectMicrocode : kVertexObjectMicrocode));
        uint32_t shaderInfo = containerAddress + Load32(containerAddress + kShaderOffset);
        uint32_t physicalOffset = Load32(shaderInfo);
        uint32_t programDwords = Load32(shaderInfo + 4) / 4;
        uint32_t microcode = mem::ToPhysical(physicalBase + physicalOffset);
        uint64_t programHash =
            MicrocodeHashIgnoringVertexFetches(g_base + containerAddress + virtualSize + physicalOffset, programDwords);

        const uint8_t* data = g_base + containerAddress;
        uint64_t hash = XXH3_64bits(data, virtualSize + physicalSize);

        static const std::filesystem::path dumpDir = DumpDirectory();
        bool added = false;
        GuestShader* shader;
        {
            std::unique_lock lock(s_lock);
            auto& slot = s_byHash[hash];
            if (!slot)
            {
                slot = std::make_unique<GuestShader>();
                slot->hash = hash;
                slot->isPixel = isPixel;
                slot->container.assign(data, data + virtualSize + physicalSize);
                added = true;
            }
            shader = slot.get();
            shader->microcodePhys = microcode;
            s_byMicrocode[microcode] = shader;
            s_byProgram[isPixel][programHash] = shader;
        }

        static const bool trap = [] {
            char v[4];
            return GetEnvironmentVariableA("MCLA_TRAP_SHADERS", v, sizeof(v)) > 0;
        }();
        static int trapped = 0;
        if (trap && trapped++ < 8)
            mem::TrapPageWrites(microcode);

        static int logged = 0;
        if (logged++ < 4)
        {
            // The loaded microcode should match the container's physical part.
            bool same = memcmp(g_base + 0xA0000000 + microcode, data + virtualSize + physicalOffset,
                               std::min<uint32_t>(physicalSize - physicalOffset, 64)) == 0;
            LOGI("shaders: %s %016llX: container %08X (virtual %u, physical %u) -> object %08X, microcode %08X%s",
                 shader->isPixel ? "pixel" : "vertex", hash, containerAddress, virtualSize, physicalSize, shaderObject,
                 microcode, same ? "" : " (contents differ)");
        }

        if (added && !dumpDir.empty())
        {
            char name[32];
            snprintf(name, sizeof(name), "%016llX.bin", hash);
            if (FILE* f = _wfopen((dumpDir / name).c_str(), L"wb"))
            {
                fwrite(shader->container.data(), 1, shader->container.size(), f);
                fclose(f);
            }
        }
    }

    GuestShader* FindShaderByMicrocode(uint32_t physAddress, bool isPixel)
    {
        std::shared_lock lock(s_lock);
        auto it = s_byMicrocode.find(physAddress);
        return it != s_byMicrocode.end() && it->second->isPixel == isPixel ? it->second : nullptr;
    }

    void RegisterInlineCopy(uint32_t copyAddress, uint32_t sourceAddress)
    {
        uint32_t source = mem::ToPhysical(sourceAddress);
        std::unique_lock lock(s_lock);
        auto it = s_byMicrocode.find(source);
        static int logged = 0;
        if ((it == s_byMicrocode.end() || copyAddress < 0xA0000000) && logged++ < 8)
            LOGI("shaders: inline copy at %08X of %08X (%s)", copyAddress, sourceAddress,
                 it == s_byMicrocode.end() ? "unknown source" : "known source");
        // Command buffer space is reused only after the GPU has consumed it, so an
        // entry is never overwritten before the command processor reads it.
        s_byInlineCopy[mem::ToPhysical(copyAddress)] = it != s_byMicrocode.end() ? it->second : nullptr;
    }

    GuestShader* FindShaderByInlineCopy(uint32_t physAddress, bool isPixel)
    {
        std::shared_lock lock(s_lock);
        auto it = s_byInlineCopy.find(physAddress);
        return it != s_byInlineCopy.end() && it->second && it->second->isPixel == isPixel ? it->second : nullptr;
    }

    GuestShader* FindShaderByProgram(uint32_t codeAddress, uint32_t dwords, bool isPixel)
    {
        uint64_t hash = MicrocodeHashIgnoringVertexFetches(g_base + codeAddress, dwords);
        std::shared_lock lock(s_lock);
        auto it = s_byProgram[isPixel].find(hash);
        return it != s_byProgram[isPixel].end() ? it->second : nullptr;
    }

    GuestShader* GetRawShader(uint32_t codeAddress, uint32_t dwords, bool isPixel)
    {
        uint64_t hash = MicrocodeHashIgnoringVertexFetches(g_base + codeAddress, dwords);
        std::unique_lock lock(s_lock);
        auto& slot = s_raw[isPixel][hash];
        if (!slot)
        {
            slot = std::make_unique<GuestShader>();
            // Distinct from container hashes and between stages (the host key's seed).
            slot->hash = hash ^ (isPixel ? 0x5241575053000000ull : 0x5241575653000000ull);
            slot->isPixel = isPixel;
            LOGI("shaders: raw %s shader %016llX (%u dwords) loaded from %08X", isPixel ? "pixel" : "vertex",
                 (unsigned long long)slot->hash, dwords, codeAddress);
        }
        return slot.get();
    }

    size_t ShaderCount()
    {
        std::shared_lock lock(s_lock);
        return s_byHash.size();
    }
}

// Mid-asm hooks (config/MCLA.toml) at the common exit of D3D's two shader create
// functions (sub_82418100 pixel, sub_82418210 vertex), so shaders from every
// caller are seen. r3 is the new object (0 on failure). The functions keep
// container + virtual size in a non-volatile register, and copy the container's
// virtual part (whose header holds the virtual size) into the object.
void PixelShaderCreatedHook(PPCContext& ctx)
{
    uint32_t object = ctx.r3.u32;
    if (object)
        gpu::RegisterShader(ctx.r27.u32 - Load32(object + 0x28 + 4), object);
}

void VertexShaderCreatedHook(PPCContext& ctx)
{
    uint32_t object = ctx.r3.u32;
    if (object)
        gpu::RegisterShader(ctx.r28.u32 - Load32(object + 0x368 + 4), object);
}

// In sub_82424320, after the vertex shader's microcode (r28, cached view) has been
// copied into the command buffer at r26 and before its fetches are patched.
void InlineShaderCopyHook_824243F4(PPCContext& ctx) { gpu::RegisterInlineCopy(ctx.r26.u32, ctx.r28.u32); }
