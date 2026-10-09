#include "loader.h"
#include "guest.h"
#include "log.h"
#include "memory.h"
#include "thread.h"

#include <image.h>
#include <xex.h>

#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

XexInfo g_xex;

// Ordinal -> "__imp__Name" tables from XenonUtils (xex.cpp).
extern std::unordered_map<size_t, const char*> XboxKernelExports;
extern std::unordered_map<size_t, const char*> XamExports;

namespace
{
    std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    }

    uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

    // Raw optional-header lookup on the original file bytes.
    const uint8_t* OptHeader(const std::vector<uint8_t>& xex, uint32_t key, uint32_t* inlineValue = nullptr)
    {
        uint32_t count = BE32(&xex[0x14]);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* e = &xex[0x18 + i * 8];
            if (BE32(e) != key)
                continue;
            uint32_t v = BE32(e + 4);
            if ((key & 0xFF) <= 1)
            {
                if (inlineValue)
                    *inlineValue = v;
                return e + 4;
            }
            return &xex[v];
        }
        return nullptr;
    }

    // xboxkrnl ordinal -> guest address of the exported variable.
    std::map<uint32_t, uint32_t> CreateKernelVariables()
    {
        std::map<uint32_t, uint32_t> vars;
        auto dword = [](uint32_t value) {
            uint32_t a = mem::SystemAlloc(4);
            Store32(a, value);
            return a;
        };

        // ExThreadObjectType: object type descriptor, only compared by address.
        vars[0x1B] = mem::SystemAlloc(0x40);
        // KeDebugMonitorData / KeCertMonitorData: no debugger attached.
        vars[0x59] = dword(0);
        vars[0x266] = dword(0);
        // KeTimeStampBundle: be64 interrupt_time, be64 system_time, be32 tick_count.
        g_xex.timeStampBundle = vars[0xAD] = mem::SystemAlloc(0x18);
        // XboxHardwareInfo: flags 0x20, 6 hardware threads.
        uint32_t hw = vars[0x156] = mem::SystemAlloc(0x10);
        Store32(hw + 0, 0x20);
        Store8(hw + 4, 6);
        // XboxKrnlVersion 2.0.7371.0, matching the title's import libraries.
        uint32_t kv = vars[0x158] = mem::SystemAlloc(8);
        Store16(kv + 0, 2);
        Store16(kv + 2, 0);
        Store16(kv + 4, 7371);
        Store16(kv + 6, 0);
        // XexExecutableModuleHandle: pointer to the module handle (LDR entry).
        vars[0x193] = dword(g_xex.ldrEntry);
        // ExLoadedCommandLine: char[]
        uint32_t cmd = vars[0x1AE] = mem::SystemAlloc(0x400);
        strcpy(GuestPtr<char>(cmd), "\"default.xex\"");
        // VdGlobalDevice: D3D stores its device pointer here.
        g_xex.vdGlobalDevice = vars[0x1BE] = dword(0);
        // VdGpuClockInMHz
        vars[0x1C0] = dword(500);
        // VdHSIOCalibrationLock: RTL_CRITICAL_SECTION, initialised unowned.
        uint32_t cs = g_xex.vdHsioCalibrationLock = vars[0x1C1] = mem::SystemAlloc(0x1C);
        Store8(cs + 0, 1);
        Store32(cs + 0x10, 0xFFFFFFFF);
        return vars;
    }
}

uint32_t XexHeaderField(uint32_t header, uint32_t key)
{
    uint32_t count = Load32(header + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t e = header + 0x18 + i * 8;
        if (Load32(e) != key)
            continue;
        uint32_t v = Load32(e + 4);
        return (key & 0xFF) <= 1 ? v : header + v;
    }
    return 0;
}

bool LoadXex(const std::filesystem::path& path)
{
    std::vector<uint8_t> xex = ReadFile(path);
    if (xex.size() < 0x18 || memcmp(xex.data(), "XEX2", 4) != 0)
    {
        LOGI("FATAL: %s is not a XEX2 file", path.string().c_str());
        return false;
    }

    Image image = Xex2LoadImage(xex.data(), xex.size());
    if (!image.data)
    {
        LOGI("FATAL: could not decrypt/decompress %s", path.string().c_str());
        return false;
    }
    g_xex.imageBase = uint32_t(image.base);
    g_xex.imageSize = image.size;
    g_xex.entryPoint = uint32_t(image.entry_point);

    // Image and the function table that sits right after it.
    mem::Commit(g_xex.imageBase, g_xex.imageSize);
    memcpy(g_base + g_xex.imageBase, image.data.get(), image.size);
    uint64_t tableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    mem::Commit(uint32_t(tableBase), PPC_CODE_SIZE * 2);
    size_t mapped = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->guest; m++, mapped++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    LOGI("loaded %s: image %08X+%X, entry %08X, %zu functions mapped", path.filename().string().c_str(),
         g_xex.imageBase, g_xex.imageSize, g_xex.entryPoint, mapped);

    // Guest copy of the XEX header (RtlImageXexHeaderField, XamGetExecutionId).
    g_xex.headerSize = BE32(&xex[8]);
    g_xex.header = mem::SystemAlloc(g_xex.headerSize, 0x1000);
    memcpy(g_base + g_xex.header, xex.data(), g_xex.headerSize);

    uint32_t v = 0;
    if (OptHeader(xex, 0x00030000, &v)) // SYSTEM_FLAGS
        g_xex.systemFlags = v;
    g_xex.stackSize = 0x40000;
    if (OptHeader(xex, 0x00020200, &v)) // DEFAULT_STACK_SIZE
        g_xex.stackSize = v;
    thread::SetDefaultStackSize(g_xex.stackSize);
    if (const uint8_t* exec = OptHeader(xex, 0x00040006)) // EXECUTION_INFO
        g_xex.titleId = BE32(exec + 12);
    if (const uint8_t* tls = OptHeader(xex, 0x00020104)) // TLS_INFO
    {
        thread::SetTlsInfo(BE32(tls + 0), BE32(tls + 4), BE32(tls + 12), BE32(tls + 8));
        LOGI("TLS: %u slots, raw %08X size %X, data %X", BE32(tls + 0), BE32(tls + 4), BE32(tls + 12), BE32(tls + 8));
    }

    // Module handle: X_LDR_DATA_TABLE_ENTRY. Field offsets follow the Xbox 360 kernel.
    uint32_t ldr = g_xex.ldrEntry = mem::SystemAlloc(0x80);
    Store32(ldr + 0x00, ldr); // in_load_order_links (self-linked)
    Store32(ldr + 0x04, ldr);
    Store32(ldr + 0x1C, g_xex.imageBase);
    Store32(ldr + 0x20, g_xex.imageSize);
    Store32(ldr + 0x38, g_xex.imageSize);
    Store32(ldr + 0x3C, g_xex.entryPoint);
    Store32(ldr + 0x58, g_xex.header);

    // Imports. Xex2LoadImage byte-swapped every import record in place, replaced
    // each function thunk with a stub (recompiled into a call to __imp__<Name>),
    // and recorded the thunk under that name in image.symbols. Records still
    // holding a value of type 0 are import-table slots: for functions they get the
    // thunk address (so calls through the slot reach __imp__<Name>), for kernel
    // variables the address of our variable.
    auto vars = CreateKernelVariables();
    std::map<std::string, uint32_t> thunkByName;
    for (const Symbol& s : image.symbols)
        if (s.type == Symbol_Function && s.name.rfind("__imp__", 0) == 0)
            thunkByName[s.name] = uint32_t(s.address);

    const uint8_t* imp = OptHeader(xex, 0x000103FF);
    if (imp)
    {
        uint32_t size = BE32(imp), strSize = BE32(imp + 4), strCount = BE32(imp + 8);
        std::vector<std::string> names;
        for (const char* s = reinterpret_cast<const char*>(imp + 12); names.size() < strCount;)
        {
            names.emplace_back(s);
            s += (names.back().size() + 1 + 3) & ~3u;
        }

        int functions = 0, variables = 0, missing = 0, thunksSeen = 0;
        for (uint32_t o = 12 + strSize; o + 0x28 <= size;)
        {
            const uint8_t* lib = imp + o;
            uint32_t libSize = BE32(lib);
            if (!libSize)
                break;
            const std::string& libName = names[BE16(lib + 0x24)];
            auto* exports = libName == "xboxkrnl.exe" ? &XboxKernelExports : libName == "xam.xex" ? &XamExports : nullptr;
            uint16_t count = BE16(lib + 0x26);
            for (uint16_t i = 0; i < count; i++)
            {
                uint32_t slot = BE32(lib + 0x28 + i * 4);
                const uint8_t* slotData = image.data.get() + (slot - g_xex.imageBase);
                // Stub written by Xex2LoadImage over each function thunk (nop x3, blr).
                static constexpr uint32_t kThunkStub[4] = { 0x00000060, 0x00000060, 0x00000060, 0x2000804E };
                if (memcmp(slotData, kThunkStub, sizeof(kThunkStub)) == 0)
                {
                    thunksSeen++;
                    continue;
                }
                uint32_t value;
                memcpy(&value, slotData, 4); // swapped to host order by Xex2LoadImage
                uint32_t ordinal = value & 0xFFFF;
                const char* name = nullptr;
                if (exports)
                    if (auto e = exports->find(ordinal); e != exports->end())
                        name = e->second;
                if (name && thunkByName.count(name))
                {
                    Store32(slot, thunkByName[name]);
                    functions++;
                }
                else if (libName == "xboxkrnl.exe" && vars.count(ordinal))
                {
                    Store32(slot, vars[ordinal]);
                    variables++;
                }
                else
                {
                    LOGI("unresolved import %s ordinal %X (%s) at %08X", libName.c_str(), ordinal, name ? name : "?", slot);
                    Store32(slot, 0);
                    missing++;
                }
            }
            o += libSize;
        }
        LOGI("imports: %d function slots, %d variables, %d thunks, %d unresolved", functions, variables, thunksSeen, missing);
    }
    return true;
}
