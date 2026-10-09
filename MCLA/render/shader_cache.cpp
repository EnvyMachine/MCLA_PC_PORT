#include "shader_cache.h"
#include "../gpu/shader_registry.h"
#include "../runtime/guest.h"
#include "../runtime/log.h"

#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace render
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        // Bump when the translator or the support header changes output.
        constexpr uint64_t kTranslatorVersion = 3;
        constexpr uint32_t kCacheMagic = 0x4D434C53; // 'MCLS'

        struct CacheHeader
        {
            uint32_t magic;
            uint32_t isPixel;
            uint32_t textureSlots;
            uint8_t textureDims[32];
            uint32_t colorOutputs;
            uint32_t writesDepth;
            uint32_t vertexFetchSlots[3];
            uint32_t dxilSize;
        };

        struct Job
        {
            HostShader* shader;
            bool isPixel;
            std::vector<uint8_t> container; // empty: raw microcode
            std::vector<uint8_t> program;
            bool hasLinkage = false;        // raw microcode: semantics of the partner stage
            xenos::Linkage linkage{};
        };

        std::unordered_map<uint64_t, std::unique_ptr<HostShader>> s_shaders; // command-processor thread only
        std::mutex s_jobLock;
        std::condition_variable s_jobReady;
        std::deque<Job> s_jobs;
        std::atomic<uint32_t> s_pending{ 0 };
        DxcCreateInstanceProc s_dxcCreate = nullptr;
        std::filesystem::path s_cacheDir;

        std::filesystem::path CachePath(uint64_t key)
        {
            char name[32];
            snprintf(name, sizeof(name), "%016llX.bin", (unsigned long long)key);
            return s_cacheDir / name;
        }

        bool LoadCached(HostShader& shader)
        {
            FILE* f = _wfopen(CachePath(shader.key).c_str(), L"rb");
            if (!f)
                return false;
            CacheHeader h;
            bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == kCacheMagic && h.dxilSize;
            if (ok)
            {
                shader.dxil.resize(h.dxilSize);
                ok = fread(shader.dxil.data(), 1, h.dxilSize, f) == h.dxilSize;
                shader.isPixel = h.isPixel;
                shader.textureSlots = h.textureSlots;
                memcpy(shader.textureDims, h.textureDims, 32);
                shader.colorOutputs = h.colorOutputs;
                shader.writesDepth = h.writesDepth;
                memcpy(shader.vertexFetchSlots, h.vertexFetchSlots, sizeof(h.vertexFetchSlots));
            }
            fclose(f);
            return ok;
        }

        void StoreCached(const HostShader& shader)
        {
            CacheHeader h{};
            h.magic = kCacheMagic;
            h.isPixel = shader.isPixel;
            h.textureSlots = shader.textureSlots;
            memcpy(h.textureDims, shader.textureDims, 32);
            h.colorOutputs = shader.colorOutputs;
            h.writesDepth = shader.writesDepth;
            memcpy(h.vertexFetchSlots, shader.vertexFetchSlots, sizeof(h.vertexFetchSlots));
            h.dxilSize = uint32_t(shader.dxil.size());
            auto path = CachePath(shader.key);
            auto temp = path;
            temp += ".tmp";
            if (FILE* f = _wfopen(temp.c_str(), L"wb"))
            {
                fwrite(&h, sizeof(h), 1, f);
                fwrite(shader.dxil.data(), 1, shader.dxil.size(), f);
                fclose(f);
                std::error_code ec;
                std::filesystem::rename(temp, path, ec);
            }
        }

        void DumpFailure(const HostShader& shader, const std::string& hlsl, const char* errors)
        {
            auto dir = s_cacheDir / "failed";
            std::filesystem::create_directories(dir);
            char name[40];
            snprintf(name, sizeof(name), "%016llX.hlsl", (unsigned long long)shader.key);
            if (FILE* f = _wfopen((dir / name).c_str(), L"wb"))
            {
                fwrite(hlsl.data(), 1, hlsl.size(), f);
                if (errors)
                    fprintf(f, "\n/* DXC errors:\n%s\n*/\n", errors);
                fclose(f);
            }
        }

        void Compile(IDxcCompiler3* compiler, Job& job)
        {
            HostShader& shader = *job.shader;
            xenos::Translation t;
            uint32_t programDwords = uint32_t(job.program.size() / 4);
            bool translated = job.container.empty()
                                  ? xenos::TranslateRaw(job.isPixel, job.program.data(), programDwords, t,
                                                        job.hasLinkage ? &job.linkage : nullptr)
                                  : xenos::Translate(job.container.data(), job.container.size(), job.program.data(),
                                                     programDwords, t, job.hasLinkage ? &job.linkage : nullptr);
            if (!translated)
            {
                static std::atomic<int> logged{ 0 };
                if (logged++ < 10)
                    LOGI("shaders: translation of %016llX failed: %s", (unsigned long long)shader.key, t.error.c_str());
                shader.state = HostShader::kFailed;
                return;
            }

            static const bool dumpHlsl = [] {
                char v[4];
                return GetEnvironmentVariableA("MCLA_DUMP_HLSL", v, sizeof(v)) > 0;
            }();
            if (dumpHlsl)
            {
                auto dir = s_cacheDir / "hlsl";
                std::filesystem::create_directories(dir);
                char name[40];
                snprintf(name, sizeof(name), "%016llX.hlsl", (unsigned long long)shader.key);
                if (FILE* f = _wfopen((dir / name).c_str(), L"wb"))
                {
                    fwrite(t.hlsl.data(), 1, t.hlsl.size(), f);
                    fprintf(f, "\n/* colorOutputs %X textureSlots %X vfetch %X %X %X */\n", t.colorOutputs, t.textureSlots,
                            t.vertexFetchSlots[0], t.vertexFetchSlots[1], t.vertexFetchSlots[2]);
                    fclose(f);
                }
            }

            const wchar_t* args[] = { L"-T", t.isPixel ? L"ps_6_0" : L"vs_6_0", L"-E", L"main", L"-HV", L"2021",
                                      L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect", L"-Wno-ignored-attributes" };
            DxcBuffer source{ t.hlsl.data(), t.hlsl.size(), DXC_CP_UTF8 };
            ComPtr<IDxcResult> result;
            HRESULT status = E_FAIL;
            if (SUCCEEDED(compiler->Compile(&source, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))))
                result->GetStatus(&status);
            if (FAILED(status))
            {
                ComPtr<IDxcBlobUtf8> errors;
                if (result)
                    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
                const char* text = errors ? errors->GetStringPointer() : "(no output)";
                static std::atomic<int> logged{ 0 };
                if (logged++ < 10)
                    LOGI("shaders: DXC failed for %016llX:\n%.600s", (unsigned long long)shader.key, text);
                DumpFailure(shader, t.hlsl, text);
                shader.state = HostShader::kFailed;
                return;
            }
            ComPtr<IDxcBlob> object;
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
            auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
            shader.dxil.assign(bytes, bytes + object->GetBufferSize());
            shader.isPixel = t.isPixel;
            shader.textureSlots = t.textureSlots;
            memcpy(shader.textureDims, t.textureDims, 32);
            shader.colorOutputs = t.colorOutputs;
            shader.writesDepth = t.writesDepth;
            memcpy(shader.vertexFetchSlots, t.vertexFetchSlots, sizeof(t.vertexFetchSlots));
            StoreCached(shader);
            shader.state = HostShader::kReady;
        }

        void Worker()
        {
            SetThreadDescription(GetCurrentThread(), L"MCLA shader compiler");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            ComPtr<IDxcCompiler3> compiler;
            if (FAILED(s_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
            {
                LOGI("shaders: could not create a DXC compiler");
                return;
            }
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lock(s_jobLock);
                    s_jobReady.wait(lock, [] { return !s_jobs.empty(); });
                    job = std::move(s_jobs.front());
                    s_jobs.pop_front();
                }
                if (!LoadCached(*job.shader))
                    Compile(compiler.Get(), job);
                else
                    job.shader->state = HostShader::kReady;
                s_pending--;
            }
        }
    }

    bool InitShaderCache()
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto dir = std::filesystem::path(exe).parent_path();
        HMODULE dxc = LoadLibraryW((dir / L"dxcompiler.dll").c_str());
        if (!dxc)
        {
            LOGI("shaders: dxcompiler.dll not found next to the executable");
            return false;
        }
        s_dxcCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance"));
        s_cacheDir = dir / "shader_cache";
        std::filesystem::create_directories(s_cacheDir);
        unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; i++)
            std::thread(Worker).detach();
        LOGI("shaders: %u compiler threads, cache in %s", threads, s_cacheDir.string().c_str());
        return true;
    }

    HostShader* GetHostShader(const gpu::GuestShader* guest, uint32_t codeAddress, uint32_t dwords,
                              const gpu::GuestShader* partner)
    {
        if (!guest || !s_dxcCreate)
            return nullptr;
        // The support header is part of every shader, so editing it changes every key.
        static const uint64_t seedBase =
            (kTranslatorVersion << 56) ^ XXH3_64bits(xenos::CommonHlsl(), strlen(xenos::CommonHlsl()));
        uint64_t key = XXH3_64bits_withSeed(g_base + codeAddress, size_t(dwords) * 4, guest->hash ^ seedBase);
        // Interpolator N links VS export N to PS register N. Raw microcode takes the
        // partner container's semantics; a container vertex shader whose loaded copy
        // D3D re-linked for a pixel shader reading a different set takes the pixel
        // shader's semantics.
        // A raw pixel shader reads register i as TEXCOORDi, so a container vertex
        // shader drawn with one links its exports to those (MCLA's clears use two
        // vertex shaders with one raw pixel shader; the second's container names its
        // color export differently, and the clear came out black every few frames).
        xenos::Linkage linkage{};
        bool partnerLinkage = false;
        if (partner && !partner->container.empty())
            partnerLinkage = xenos::ContainerLinkage(partner->container.data(), partner->container.size(), linkage);
        else if (partner && partner->isPixel && !guest->isPixel && !guest->container.empty())
        {
            for (uint32_t i = 0; i < 16; i++)
            {
                linkage.valid[i] = true;
                linkage.usage[i] = 5;
                linkage.usageIndex[i] = uint8_t(i);
            }
            partnerLinkage = true;
        }
        bool hasLinkage = false;
        if (partnerLinkage)
        {
            xenos::Linkage own{};
            if (guest->container.empty())
            {
                for (uint32_t i = 0; i < 16; i++) // raw default: TEXCOORDi
                {
                    own.valid[i] = true;
                    own.usage[i] = 5;
                    own.usageIndex[i] = uint8_t(i);
                }
                hasLinkage = true;
            }
            else if (!guest->isPixel && partner->isPixel &&
                     xenos::ContainerLinkage(guest->container.data(), guest->container.size(), own))
                hasLinkage = true;
            if (hasLinkage)
            {
                bool same = true;
                for (uint32_t i = 0; i < 16; i++)
                    if (linkage.valid[i] && (!own.valid[i] || own.usage[i] != linkage.usage[i] ||
                                             own.usageIndex[i] != linkage.usageIndex[i]))
                        same = false;
                if (same)
                    hasLinkage = false;
                else
                    key ^= XXH3_64bits(&linkage, sizeof(linkage)) * 0x9E3779B97F4A7C15ull;
            }
        }
        auto& slot = s_shaders[key];
        if (slot)
            return slot.get();
        slot = std::make_unique<HostShader>();
        slot->key = key;
        if (guest->container.empty())
        {
            static int rawLogged = 0;
            if (rawLogged++ < 40)
                LOGI("shaders: raw %s %016llX (%u dwords), partner %s%s", guest->isPixel ? "pixel" : "vertex",
                     (unsigned long long)key, dwords, !partner ? "none" : partner->container.empty() ? "raw" : "container",
                     hasLinkage ? ", linked to partner semantics" : "");
        }
        else if (hasLinkage)
        {
            static int relinkLogged = 0;
            if (relinkLogged++ < 20)
                LOGI("shaders: vertex %016llX (container %016llX) re-linked to pixel shader %016llX's semantics",
                     (unsigned long long)key, (unsigned long long)guest->hash, (unsigned long long)partner->hash);
        }
        static int logged = 0;
        if (!guest->container.empty() && logged++ < 6)
        {
            // The loaded program should match the container's (apart from patched vertex fetches).
            const uint8_t* c = guest->container.data();
            auto be32 = [&](size_t o) { return uint32_t(c[o]) << 24 | uint32_t(c[o + 1]) << 16 | uint32_t(c[o + 2]) << 8 | c[o + 3]; };
            uint32_t info = be32(0x18), program = be32(4) + be32(info);
            LOGI("shaders: %016llX from %08X (%u dwords): %08X %08X %08X; container program (%u bytes): %08X %08X %08X",
                 (unsigned long long)key, codeAddress, dwords, Load32(codeAddress), Load32(codeAddress + 4),
                 Load32(codeAddress + 8), be32(info + 4), be32(program), be32(program + 4), be32(program + 8));
        }
        Job job;
        job.shader = slot.get();
        job.isPixel = guest->isPixel;
        job.container = guest->container;
        job.program.assign(g_base + codeAddress, g_base + codeAddress + size_t(dwords) * 4);
        job.hasLinkage = hasLinkage;
        job.linkage = linkage;
        s_pending++;
        {
            std::lock_guard lock(s_jobLock);
            s_jobs.push_back(std::move(job));
        }
        s_jobReady.notify_one();
        return slot.get();
    }

    uint32_t PendingShaderCompiles() { return s_pending.load(); }

    bool CompileHlsl(const char* source, const wchar_t* target, std::vector<uint8_t>& dxil)
    {
        ComPtr<IDxcCompiler3> compiler;
        if (!s_dxcCreate || FAILED(s_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
            return false;
        const wchar_t* args[] = { L"-T", target, L"-E", L"main", L"-HV", L"2021", L"-O3", L"-Qstrip_debug" };
        DxcBuffer buffer{ source, strlen(source), DXC_CP_UTF8 };
        ComPtr<IDxcResult> result;
        HRESULT status = E_FAIL;
        if (SUCCEEDED(compiler->Compile(&buffer, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result))))
            result->GetStatus(&status);
        if (FAILED(status))
        {
            ComPtr<IDxcBlobUtf8> errors;
            if (result)
                result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            LOGI("shaders: built-in shader failed to compile:\n%s", errors ? errors->GetStringPointer() : "(no output)");
            return false;
        }
        ComPtr<IDxcBlob> object;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
        auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
        dxil.assign(bytes, bytes + object->GetBufferSize());
        return true;
    }

    int TestShaderDirectory(const wchar_t* directory)
    {
        ComPtr<IDxcCompiler3> compiler;
        if (!s_dxcCreate || FAILED(s_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
            return -1;
        int total = 0, failed = 0;
        for (auto& entry : std::filesystem::directory_iterator(directory))
        {
            FILE* f = _wfopen(entry.path().c_str(), L"rb");
            if (!f)
                continue;
            std::vector<uint8_t> data(std::filesystem::file_size(entry.path()));
            fread(data.data(), 1, data.size(), f);
            fclose(f);
            auto be32 = [&](size_t o) { return uint32_t(data[o]) << 24 | uint32_t(data[o + 1]) << 16 | uint32_t(data[o + 2]) << 8 | data[o + 3]; };
            uint32_t virtualSize = be32(4), shaderInfo = be32(0x18);
            uint32_t physicalOffset = be32(shaderInfo), size = be32(shaderInfo + 4);
            Job job;
            HostShader shader;
            shader.key = XXH3_64bits(data.data(), data.size());
            job.shader = &shader;
            job.isPixel = (be32(0) & 1) == 0;
            job.container = data;
            job.program.assign(data.begin() + virtualSize + physicalOffset, data.begin() + virtualSize + physicalOffset + size);
            Compile(compiler.Get(), job);
            total++;
            if (shader.state != HostShader::kReady)
                failed++;
        }
        LOGI("shaders: test compiled %d shaders, %d failed (HLSL of failures in %s)", total, failed,
             (s_cacheDir / "failed").string().c_str());
        return failed;
    }
}
