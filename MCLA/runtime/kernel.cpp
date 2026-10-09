#include "kernel.h"
#include "guest.h"
#include "log.h"
#include "memory.h"
#include "thread.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

// Read by PPC_QUERY_TIMEBASE (MCLARecompLib/ppc_overrides.h).
uint64_t g_tscOrigin;
uint64_t g_tscToTimebase = uint64_t(1) << 32;

namespace kern
{
    // X_DISPATCHER_HEADER: u8 type, u8 absolute, u8 size, u8 inserted,
    // be32 signal_state, X_LIST_ENTRY wait_list.
    static uint8_t Type(uint32_t obj) { return Load8(obj); }
    static int32_t Signal(uint32_t obj) { return int32_t(Load32(obj + 4)); }
    static void SetSignal(uint32_t obj, int32_t v) { Store32(obj + 4, uint32_t(v)); }

    static void InitHeader(uint32_t obj, uint8_t type, uint8_t sizeWords, int32_t signal)
    {
        Store8(obj + 0, type);
        Store8(obj + 1, 0);
        Store8(obj + 2, sizeWords);
        Store8(obj + 3, 0);
        SetSignal(obj, signal);
        Store32(obj + 8, obj + 8); // empty wait list
        Store32(obj + 12, obj + 8);
    }

    static std::mutex s_dispatchLock;
    static std::condition_variable s_dispatchCv;

    // ---- handles -------------------------------------------------------------

    static std::mutex s_handleLock;
    static std::unordered_map<uint32_t, HandleEntry> s_handles;
    static uint32_t s_nextHandle = 0xF8000004;

    void Init()
    {
    }

    uint32_t CreateHandle(HandleKind kind, uint32_t object, std::shared_ptr<HostObject> host)
    {
        std::lock_guard lock(s_handleLock);
        uint32_t h = s_nextHandle;
        s_nextHandle += 4;
        s_handles[h] = HandleEntry{ kind, object, std::move(host) };
        return h;
    }

    bool LookupHandle(uint32_t handle, HandleEntry& out)
    {
        if (handle == CurrentThreadHandle)
        {
            out = { HandleKind::Thread, thread::CurrentKThread(), {} };
            return true;
        }
        std::lock_guard lock(s_handleLock);
        auto it = s_handles.find(handle);
        if (it == s_handles.end())
            return false;
        out = it->second;
        return true;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(s_handleLock);
        return s_handles.erase(handle) != 0;
    }

    uint32_t DuplicateHandle(uint32_t handle)
    {
        HandleEntry e;
        if (!LookupHandle(handle, e))
            return 0;
        return CreateHandle(e.kind, e.object, e.host);
    }

    uint32_t ObjectFromHandle(uint32_t handle)
    {
        HandleEntry e;
        return LookupHandle(handle, e) ? e.object : 0;
    }

    // ---- object creation -----------------------------------------------------

    uint32_t NewEvent(bool manualReset, bool initialState)
    {
        uint32_t ev = mem::SystemAlloc(0x10);
        InitHeader(ev, manualReset ? NotificationEvent : SynchronizationEvent, 4, initialState ? 1 : 0);
        return ev;
    }

    void InitSemaphore(uint32_t sem, int32_t count, int32_t limit)
    {
        InitHeader(sem, Semaphore, 5, count);
        Store32(sem + 0x10, uint32_t(limit));
    }

    uint32_t NewSemaphore(int32_t count, int32_t limit)
    {
        uint32_t sem = mem::SystemAlloc(0x14);
        InitSemaphore(sem, count, limit);
        return sem;
    }

    uint32_t NewMutant(bool initialOwner)
    {
        // X_KMUTANT: header, list entry (0x10), owner (0x18), abandoned (0x1C)
        uint32_t m = mem::SystemAlloc(0x20);
        InitHeader(m, Mutant, 8, initialOwner ? 0 : 1);
        Store32(m + 0x18, initialOwner ? thread::CurrentKThread() : 0);
        return m;
    }

    uint32_t NewTimer(bool manualReset)
    {
        uint32_t t = mem::SystemAlloc(0x28);
        InitHeader(t, manualReset ? NotificationTimer : SynchronizationTimer, 10, 0);
        return t;
    }

    // ---- signalling ----------------------------------------------------------

    int32_t SetEvent(uint32_t ev)
    {
        std::lock_guard lock(s_dispatchLock);
        int32_t prev = Signal(ev);
        SetSignal(ev, 1);
        s_dispatchCv.notify_all();
        return prev;
    }

    int32_t ResetEvent(uint32_t ev)
    {
        std::lock_guard lock(s_dispatchLock);
        int32_t prev = Signal(ev);
        SetSignal(ev, 0);
        return prev;
    }

    bool ReleaseSemaphore(uint32_t sem, int32_t adjustment, int32_t* previous)
    {
        std::lock_guard lock(s_dispatchLock);
        int32_t prev = Signal(sem);
        int32_t limit = int32_t(Load32(sem + 0x10));
        if (previous)
            *previous = prev;
        if (adjustment <= 0 || int64_t(prev) + adjustment > limit)
            return false;
        SetSignal(sem, prev + adjustment);
        s_dispatchCv.notify_all();
        return true;
    }

    bool ReleaseMutant(uint32_t mutant, int32_t* previous)
    {
        std::lock_guard lock(s_dispatchLock);
        int32_t prev = Signal(mutant);
        if (previous)
            *previous = prev;
        if (Load32(mutant + 0x18) != thread::CurrentKThread())
            return false;
        SetSignal(mutant, prev + 1);
        if (prev + 1 > 0)
            Store32(mutant + 0x18, 0);
        s_dispatchCv.notify_all();
        return true;
    }

    void SignalObject(uint32_t object)
    {
        std::lock_guard lock(s_dispatchLock);
        SetSignal(object, 1);
        s_dispatchCv.notify_all();
    }

    // ---- waiting -------------------------------------------------------------

    static bool IsSignaled(uint32_t obj, uint32_t self)
    {
        switch (Type(obj))
        {
        case Mutant:
            return Signal(obj) > 0 || Load32(obj + 0x18) == self;
        case Semaphore:
            return Signal(obj) > 0;
        default:
            return Signal(obj) != 0;
        }
    }

    static void Acquire(uint32_t obj, uint32_t self)
    {
        switch (Type(obj))
        {
        case SynchronizationEvent:
        case SynchronizationTimer:
            SetSignal(obj, 0);
            break;
        case Semaphore:
            SetSignal(obj, Signal(obj) - 1);
            break;
        case Mutant:
            SetSignal(obj, Signal(obj) - 1);
            Store32(obj + 0x18, self);
            break;
        default:
            break;
        }
    }

    std::optional<int64_t> ReadTimeout(uint32_t timeoutPtr)
    {
        if (!timeoutPtr)
            return std::nullopt;
        return int64_t(Load64(timeoutPtr));
    }

    static std::chrono::steady_clock::time_point Deadline(int64_t timeout)
    {
        auto now = std::chrono::steady_clock::now();
        int64_t rel100ns = timeout < 0 ? -timeout : int64_t(uint64_t(timeout) - SystemTime());
        if (rel100ns < 0)
            rel100ns = 0;
        return now + std::chrono::microseconds(rel100ns / 10);
    }

    uint32_t Wait(const uint32_t* objects, uint32_t count, bool waitAll, std::optional<int64_t> timeout)
    {
        const uint32_t self = thread::CurrentKThread();
        auto deadline = timeout ? Deadline(*timeout) : std::chrono::steady_clock::time_point::max();
        std::unique_lock lock(s_dispatchLock);
        for (bool lastTry = false;; )
        {
            if (waitAll)
            {
                bool all = true;
                for (uint32_t i = 0; i < count && all; i++)
                    all = IsSignaled(objects[i], self);
                if (all)
                {
                    for (uint32_t i = 0; i < count; i++)
                        Acquire(objects[i], self);
                    return xstatus::Success;
                }
            }
            else
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    if (IsSignaled(objects[i], self))
                    {
                        Acquire(objects[i], self);
                        return xstatus::Success + i;
                    }
                }
            }
            if (lastTry || (timeout && *timeout == 0))
                return xstatus::Timeout;
            if (!timeout)
                s_dispatchCv.wait(lock);
            else if (s_dispatchCv.wait_until(lock, deadline) == std::cv_status::timeout)
                lastTry = true;
        }
    }

    void Sleep(std::optional<int64_t> interval)
    {
        if (!interval)
        {
            for (;;)
                std::this_thread::sleep_for(std::chrono::hours(1));
        }
        auto deadline = Deadline(*interval);
        auto now = std::chrono::steady_clock::now();
        if (deadline <= now)
            std::this_thread::yield();
        else
            std::this_thread::sleep_until(deadline);
    }

    // ---- timers --------------------------------------------------------------

    struct TimerEntry
    {
        std::chrono::steady_clock::time_point due;
        uint32_t periodMs;
        uint64_t generation;
    };
    static std::mutex s_timerLock;
    static std::condition_variable s_timerCv;
    static std::map<uint32_t, TimerEntry> s_timers;
    static uint64_t s_timerGeneration;
    static std::once_flag s_timerThreadOnce;

    static void TimerThread()
    {
        std::unique_lock lock(s_timerLock);
        for (;;)
        {
            auto next = std::chrono::steady_clock::time_point::max();
            for (auto& [obj, t] : s_timers)
                next = std::min(next, t.due);
            if (next == std::chrono::steady_clock::time_point::max())
                s_timerCv.wait(lock);
            else
                s_timerCv.wait_until(lock, next);

            auto now = std::chrono::steady_clock::now();
            for (auto it = s_timers.begin(); it != s_timers.end();)
            {
                if (it->second.due <= now)
                {
                    SignalObject(it->first);
                    if (it->second.periodMs)
                    {
                        it->second.due = now + std::chrono::milliseconds(it->second.periodMs);
                        ++it;
                    }
                    else
                        it = s_timers.erase(it);
                }
                else
                    ++it;
            }
        }
    }

    void SetTimer(uint32_t timer, int64_t dueTime, uint32_t periodMs)
    {
        std::call_once(s_timerThreadOnce, [] { std::thread(TimerThread).detach(); });
        {
            std::lock_guard lock(s_dispatchLock);
            SetSignal(timer, 0);
        }
        std::lock_guard lock(s_timerLock);
        s_timers[timer] = TimerEntry{ Deadline(dueTime), periodMs, ++s_timerGeneration };
        s_timerCv.notify_all();
    }

    void CancelTimer(uint32_t timer)
    {
        std::lock_guard lock(s_timerLock);
        s_timers.erase(timer);
    }

    // ---- TLS slots -----------------------------------------------------------

    static std::mutex s_tlsLock;
    static std::bitset<1024> s_tlsUsed;

    uint32_t TlsSlotCount()
    {
        return thread::TlsSlotCount();
    }

    uint32_t TlsAlloc()
    {
        std::lock_guard lock(s_tlsLock);
        for (uint32_t i = 0; i < TlsSlotCount(); i++)
        {
            if (!s_tlsUsed[i])
            {
                s_tlsUsed[i] = true;
                return i;
            }
        }
        return 0xFFFFFFFF;
    }

    bool TlsFree(uint32_t slot)
    {
        std::lock_guard lock(s_tlsLock);
        if (slot >= TlsSlotCount() || !s_tlsUsed[slot])
            return false;
        s_tlsUsed[slot] = false;
        return true;
    }

    // ---- time ----------------------------------------------------------------

    uint64_t SystemTime()
    {
        // MCLA_TIME_OFFSET_HOURS=<h>: shift the clock the game sees (the title
        // screen's time of day follows it), for testing night scenes.
        static const int64_t offset = [] {
            char v[16];
            return GetEnvironmentVariableA("MCLA_TIME_OFFSET_HOURS", v, sizeof(v)) > 0
                       ? int64_t(atof(v) * 3600.0) * 10000000ll
                       : 0ll;
        }();
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        return ((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) + uint64_t(offset);
    }

    uint64_t TimebaseFrequency()
    {
        // mftb (PPC_QUERY_TIMEBASE) scales the host TSC to the Xbox 360's timebase:
        // game code converts ticks with a hardcoded 1/49875000.
        static uint64_t freq = [] {
            LARGE_INTEGER qf, q0, q1;
            QueryPerformanceFrequency(&qf);
            QueryPerformanceCounter(&q0);
            uint64_t t0 = __rdtsc();
            do
                QueryPerformanceCounter(&q1);
            while (q1.QuadPart - q0.QuadPart < qf.QuadPart / 10); // 100 ms
            uint64_t t1 = __rdtsc();
            double tsc = double(t1 - t0) * double(qf.QuadPart) / double(q1.QuadPart - q0.QuadPart);
            g_tscToTimebase = uint64_t(double(kTimebaseFrequency) / tsc * 4294967296.0);
            g_tscOrigin = t0;
            LOGI("TSC frequency %.0f Hz, guest timebase %llu Hz", tsc, kTimebaseFrequency);
            return kTimebaseFrequency;
        }();
        return freq;
    }

    void StartTimeStampThread(uint32_t bundle)
    {
        // KeTimeStampBundle: be64 interrupt_time, be64 system_time, be32 tick_count.
        std::thread([bundle] {
            SetThreadDescription(GetCurrentThread(), L"MCLA timestamp");
            auto start = std::chrono::steady_clock::now();
            for (;;)
            {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                uint64_t sys = SystemTime();
                Store64(bundle + 0, uint64_t(ms) * 10000);
                Store64(bundle + 8, sys);
                Store32(bundle + 16, uint32_t(ms));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}
