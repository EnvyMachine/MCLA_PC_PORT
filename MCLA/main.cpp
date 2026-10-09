// MCLA PC runtime entry point.
//
// Usage: MCLA.exe [game folder]
//   game folder: extracted disc contents containing default.xex
//                (defaults to ..\..\game relative to the executable's build folder,
//                 i.e. <project>\game)
#include "runtime/guest.h"
#include "runtime/kernel.h"
#include "runtime/loader.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"
#include "runtime/vfs.h"
#include "render/device.h"
#include "render/renderer.h"
#include "render/shader_cache.h"
#include "render/window.h"

#include <windows.h>
#include <timeapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include <thread>

namespace crash { void Install(); void DumpAllThreads(); }
namespace apu { void XmaInit(); }
namespace profiler { void Start(); }

static std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

static std::filesystem::path FindGameDir(int argc, char** argv)
{
    // The first argument that isn't an option (--fullscreen) is the game folder.
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--", 2) != 0)
            return std::filesystem::absolute(argv[i]);
    // Walk up from the executable looking for game/default.xex.
    for (auto dir = ExeDir(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
        if (std::filesystem::exists(dir / "game" / "default.xex"))
            return dir / "game";
    return {};
}

// MCLA.ini next to the executable: MCLA_<NAME>=<value> lines set the same options as
// environment variables (which take precedence). '#' starts a comment.
static void LoadSettingsFile(const std::filesystem::path& path)
{
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || strncmp(p, "MCLA_", 5) != 0)
            continue;
        char* eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        char* value = eq + 1;
        for (char* e = value + strlen(value); e > value && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
            *--e = 0;
        for (char* e = eq; e > p && (e[-1] == ' ' || e[-1] == '\t');)
            *--e = 0;
        if (*value && GetEnvironmentVariableA(p, nullptr, 0) == 0)
            SetEnvironmentVariableA(p, value);
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    std::filesystem::path exeDir = ExeDir();
    LoadSettingsFile(exeDir / "MCLA.ini");
    LogInit((exeDir / "mcla.log").string().c_str());
    // 1 ms timer resolution: guest threads and the GPU thread sleep in short
    // intervals, which otherwise round up to the 15.6 ms default tick.
    timeBeginPeriod(1);
    crash::Install();

    // Development: MCLA.exe --test-shaders <dump dir> translates and compiles
    // every captured shader container.
    if (argc > 2 && strcmp(argv[1], "--test-shaders") == 0)
    {
        if (!render::InitShaderCache())
            return 1;
        int failed = render::TestShaderDirectory(std::filesystem::absolute(argv[2]).c_str());
        LogFlush();
        return failed == 0 ? 0 : 2;
    }

    std::filesystem::path gameDir = FindGameDir(argc, argv);
    if (gameDir.empty() || !std::filesystem::exists(gameDir / "default.xex"))
    {
        LOGI("FATAL: game folder with default.xex not found (pass it as the first argument)");
        return 1;
    }

    if (!mem::Init())
        return 1;
    kern::Init();
    kern::TimebaseFrequency(); // before any guest code reads the timebase
    // MCLA_USERDATA=<dir>: keep saves and caches somewhere else (testing with a fresh profile).
    std::filesystem::path userData = exeDir / "userdata";
    if (char v[MAX_PATH]; GetEnvironmentVariableA("MCLA_USERDATA", v, sizeof(v)) > 0)
        userData = std::filesystem::absolute(v);
    vfs::Init(gameDir, userData);
    if (!LoadXex(gameDir / "default.xex"))
        return 1;
    apu::XmaInit();

    HWND window = render::CreateGameWindow(1280, 720);
    if (!render::InitDevice(window) || !render::InitRenderer())
        return 1;

    LOGI("title %08X, entry %08X, stack %X, system flags %08X", g_xex.titleId, g_xex.entryPoint, g_xex.stackSize, g_xex.systemFlags);
    kern::TimebaseFrequency();
    kern::StartTimeStampThread(g_xex.timeStampBundle);

    profiler::Start();

    // MCLA_DUMP_AFTER=<seconds>: log every guest thread's stack after that long.
    char dumpAfter[16];
    if (GetEnvironmentVariableA("MCLA_DUMP_AFTER", dumpAfter, sizeof(dumpAfter)) > 0)
    {
        int seconds = atoi(dumpAfter);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            crash::DumpAllThreads();
        }).detach();
    }

    uint32_t exitCode = thread::RunMain(g_xex.entryPoint);
    LOGI("main thread exited with %08X", exitCode);
    LogFlush();
    return int(exitCode);
}
