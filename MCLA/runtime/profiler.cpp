// Sampling profiler for finding where host CPU time goes.
//
// MCLA_PROFILE=<from>-<to>: between those seconds after start, sample the instruction
// pointer of every thread in the process each millisecond, then log the hottest
// functions per thread (recompiled guest functions show up as sub_<address>).
#include "log.h"

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace profiler
{
    struct ThreadSamples
    {
        HANDLE handle = nullptr;
        std::string name;
        uint64_t total = 0;
        std::unordered_map<uint64_t, uint32_t> rips;
    };

    static std::string ThreadName(HANDLE h, DWORD id)
    {
        PWSTR desc = nullptr;
        std::string name;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc)
        {
            char buf[128];
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf), nullptr, nullptr);
            name = buf;
            LocalFree(desc);
        }
        if (name.empty())
            name = "thread " + std::to_string(id);
        return name;
    }

    static void Refresh(std::unordered_map<DWORD, ThreadSamples>& threads)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te{ sizeof(te) };
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || threads.count(te.th32ThreadID))
                continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            ThreadSamples& t = threads[te.th32ThreadID];
            t.handle = h;
            t.name = ThreadName(h, te.th32ThreadID);
        }
        CloseHandle(snap);
    }

    static void Report(std::unordered_map<DWORD, ThreadSamples>& threads)
    {
        HANDLE process = GetCurrentProcess();
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        std::unordered_map<uint64_t, std::string> names;
        auto nameOf = [&](uint64_t rip) -> const std::string& {
            auto it = names.find(rip);
            if (it != names.end())
                return it->second;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            std::string n;
            if (SymFromAddr(process, rip, &displacement, symbol))
                n = symbol->Name;
            else
            {
                char b[32];
                snprintf(b, sizeof(b), "%016llX", rip);
                n = b;
            }
            return names.emplace(rip, std::move(n)).first->second;
        };

        std::vector<ThreadSamples*> order;
        for (auto& [id, t] : threads)
            if (t.total)
                order.push_back(&t);
        std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->total > b->total; });
        for (ThreadSamples* t : order)
        {
            std::unordered_map<std::string, uint32_t> byFunction;
            for (auto& [rip, n] : t->rips)
                byFunction[nameOf(rip)] += n;
            std::vector<std::pair<std::string, uint32_t>> top(byFunction.begin(), byFunction.end());
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
            LogWrite("profile: %s: %llu samples", t->name.c_str(), t->total);
            for (size_t i = 0; i < top.size() && i < 25; i++)
                LogWrite("profile:   %5.1f%% %s", 100.0 * top[i].second / t->total, top[i].first.c_str());
        }
    }

    void Start()
    {
        char buf[32];
        unsigned from = 0, to = 0;
        if (GetEnvironmentVariableA("MCLA_PROFILE", buf, sizeof(buf)) == 0 || sscanf(buf, "%u-%u", &from, &to) != 2 || to <= from)
            return;
        std::thread([from, to] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            std::this_thread::sleep_for(std::chrono::seconds(from));
            LogWrite("profile: sampling");
            timeBeginPeriod(1);
            std::unordered_map<DWORD, ThreadSamples> threads;
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(to - from);
            for (int tick = 0; std::chrono::steady_clock::now() < end; tick++)
            {
                if (tick % 1000 == 0)
                    Refresh(threads);
                for (auto& [id, t] : threads)
                {
                    if (SuspendThread(t.handle) == DWORD(-1))
                        continue;
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL;
                    // Blocked threads show up in ntdll wait functions (ZwWaitFor..., NtDelayExecution).
                    if (GetThreadContext(t.handle, &c))
                    {
                        t.rips[c.Rip]++;
                        t.total++;
                    }
                    ResumeThread(t.handle);
                }
                Sleep(1);
            }
            LogWrite("profile: done");
            Report(threads);
            LogFlush();
        }).detach();
    }
}
