#include "log.h"
#include "guest.h"
#include "thread.h"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

bool g_traceImports = false;

static FILE* s_file;
static std::mutex s_mutex;
static auto s_start = std::chrono::steady_clock::now();
static thread_local const char* t_lastImport = "(none)";

void LogInit(const char* path)
{
    s_file = fopen(path, "w");
    char buf[32];
    g_traceImports = GetEnvironmentVariableA("MCLA_TRACE", buf, sizeof(buf)) > 0 && buf[0] == '1';

    // MCLA_TRACE_WINDOW=<from>-<to>: trace imports only between those seconds.
    if (GetEnvironmentVariableA("MCLA_TRACE_WINDOW", buf, sizeof(buf)) > 0)
    {
        unsigned from = 0, to = 0;
        if (sscanf(buf, "%u-%u", &from, &to) == 2 && to > from)
            std::thread([from, to] {
                std::this_thread::sleep_for(std::chrono::seconds(from));
                LogWrite("trace: on");
                g_traceImports = true;
                std::this_thread::sleep_for(std::chrono::seconds(to - from));
                g_traceImports = false;
                LogWrite("trace: off");
            }).detach();
    }
}

void LogWrite(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    std::lock_guard lock(s_mutex);
    fprintf(stdout, "[%9.3f %5lu] %s\n", t, GetCurrentThreadId(), msg);
    if (s_file)
    {
        fprintf(s_file, "[%9.3f %5lu] %s\n", t, GetCurrentThreadId(), msg);
        fflush(s_file); // the process may be killed or crash at any point
    }
}

void LogFlush()
{
    std::lock_guard lock(s_mutex);
    fflush(stdout);
    if (s_file)
        fflush(s_file);
}

void NoteImportCall(const char* name)
{
    t_lastImport = name;
    if (GuestThread* t = thread::Current())
        t->lastImport = name;
}

const char* LastImportCall()
{
    return t_lastImport;
}

void TraceImport(const char* name, PPCContext& ctx)
{
    NoteImportCall(name);
    if (g_traceImports)
        LogWrite("%s(%08X, %08X, %08X, %08X) lr=%08X", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
}
