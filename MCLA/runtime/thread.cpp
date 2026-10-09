#include "thread.h"
#include "guest.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"

#include <windows.h>
#include <process.h>

#include <unordered_map>

// X_KPCR / X_KTHREAD field offsets (layouts from Xenia's xthread.h).
namespace
{
    constexpr uint32_t kPcrSize = 0x2D8;
    constexpr uint32_t kPcrTlsPtr = 0x00;
    constexpr uint32_t kPcrPcrPtr = 0x30;      // be64
    constexpr uint32_t kPcrCurrentIrql = 0x18;
    constexpr uint32_t kPcrStackBase = 0x70;
    constexpr uint32_t kPcrStackEnd = 0x74;
    constexpr uint32_t kPcrCurrentThread = 0x100; // prcb_data.current_thread
    constexpr uint32_t kPcrCurrentCpu = 0x10C;    // prcb_data.current_cpu
    constexpr uint32_t kPcrPrcb = 0x2A8;

    constexpr uint32_t kThreadSize = 0xAB0;
    constexpr uint32_t kThreadStackBase = 0x5C;
    constexpr uint32_t kThreadStackLimit = 0x60;
    constexpr uint32_t kThreadStackKernel = 0x64;
    constexpr uint32_t kThreadTlsAddress = 0x68;
    constexpr uint32_t kThreadState = 0x6C;
    constexpr uint32_t kThreadPriority = 0x70;
    constexpr uint32_t kThreadProcessType = 0x73;
    constexpr uint32_t kThreadCurrentCpu = 0xBF;
    constexpr uint32_t kThreadStackAllocBase = 0xD0;
    constexpr uint32_t kThreadId = 0x14C;
    constexpr uint32_t kThreadStartAddress = 0x150;
    constexpr uint32_t kThreadCreationFlags = 0x16C;

    // Host stack for recompiled code: guest call depth maps to host frames, which
    // are much larger than the PowerPC ones.
    constexpr unsigned kHostStackSize = 64 * 1024 * 1024;

    struct ThreadExit
    {
        uint32_t code;
    };

    uint32_t s_tlsSlotCount = 64, s_tlsRawAddress = 0, s_tlsRawSize = 0, s_tlsDataSize = 0;
    uint32_t s_defaultStackSize = 0x40000;
    std::atomic<uint32_t> s_nextThreadId{ 1 };
    std::atomic<uint32_t> s_nextCpu{ 0 };
    std::mutex s_threadsLock;
    std::unordered_map<uint32_t, GuestThread*> s_threadsByKThread;
    thread_local GuestThread* t_current;
}

// Called by the recompiled code (ppc_overrides.h) for indirect calls to an address
// with no recompiled function. Usually a function only reached through a pointer
// that the analyser merged into a neighbour: add it to config/MCLA.toml `functions`.
void PPCMissingFunction(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    GuestThread* t = thread::Current();
    LOGI("FATAL: indirect call to %08X has no recompiled function (lr=%08X, r3=%08X, thread %s)", target,
         uint32_t(ctx.lr), ctx.r3.u32, t ? t->name.c_str() : "?");
    LogFlush();
    __debugbreak();
    ExitProcess(1);
}

void CallGuest(PPCContext& ctx, uint32_t address)
{
    PPCFunc* fn = (address >= PPC_CODE_BASE && address < PPC_CODE_BASE + PPC_CODE_SIZE)
        ? PPC_LOOKUP_FUNC(g_base, address) : nullptr;
    if (!fn)
    {
        LOGI("FATAL: call to guest address %08X with no recompiled function", address);
        LogFlush();
        __debugbreak();
        ExitProcess(1);
    }
    fn(ctx, g_base);
}

namespace thread
{
    void SetTlsInfo(uint32_t slotCount, uint32_t rawAddress, uint32_t rawSize, uint32_t dataSize)
    {
        s_tlsSlotCount = slotCount ? slotCount : 64;
        s_tlsRawAddress = rawAddress;
        s_tlsRawSize = rawSize;
        s_tlsDataSize = dataSize;
    }

    uint32_t TlsSlotCount() { return s_tlsSlotCount; }
    void SetDefaultStackSize(uint32_t size) { s_defaultStackSize = size; }
    GuestThread* Current() { return t_current; }
    uint32_t CurrentKThread() { return t_current ? t_current->kthread : 0; }

    GuestThread* FromKThread(uint32_t kthread)
    {
        std::lock_guard lock(s_threadsLock);
        auto it = s_threadsByKThread.find(kthread);
        return it == s_threadsByKThread.end() ? nullptr : it->second;
    }

    // Hardware thread for a new guest thread: the processor mask in creation flags
    // bits 24-29 (highest set bit), else the creating thread's CPU, as on the console.
    static uint32_t ChooseCpu(uint32_t creationFlags)
    {
        uint32_t mask = (creationFlags >> 24) & 0x3F;
        if (mask)
            return 31 - __builtin_clz(mask);
        if (GuestThread* parent = Current())
            return Load8(parent->pcr + kPcrCurrentCpu);
        return s_nextCpu++ % 6;
    }

    // Builds PCR, KTHREAD, TLS and stack in guest memory.
    static void SetupGuestBlocks(GuestThread* t, uint32_t stackSize, uint32_t cpu)
    {
        uint32_t tlsSize = s_tlsDataSize + s_tlsSlotCount * 4;
        uint32_t blockSize = kPcrSize + kThreadSize + ((tlsSize + 15) & ~15u);
        t->block = mem::SystemAlloc(blockSize, 64);
        t->pcr = t->block;
        t->kthread = t->block + kPcrSize;
        t->tlsStatic = t->kthread + kThreadSize;
        t->tlsSlots = t->tlsStatic + s_tlsDataSize;
        if (s_tlsRawSize)
            memcpy(g_base + t->tlsStatic, g_base + s_tlsRawAddress, s_tlsRawSize);

        // Stack with a 4 KB unused gap below it.
        stackSize = (std::max(stackSize, 0x4000u) + 0xFFF) & ~0xFFFu;
        t->stackAlloc = mem::Virtual4K().Alloc(stackSize + 0x1000, 0x1000, true);
        t->stackLimit = t->stackAlloc + 0x1000;
        t->stackBase = t->stackLimit + stackSize;

        uint32_t pcr = t->pcr, kt = t->kthread;
        Store32(pcr + kPcrTlsPtr, t->tlsStatic);
        Store64(pcr + kPcrPcrPtr, pcr);
        Store8(pcr + kPcrCurrentIrql, 0);
        Store32(pcr + kPcrStackBase, t->stackBase);
        Store32(pcr + kPcrStackEnd, t->stackLimit);
        Store32(pcr + kPcrCurrentThread, kt);
        Store8(pcr + kPcrCurrentCpu, uint8_t(cpu));
        Store32(pcr + kPcrPrcb, pcr + 0x100);

        // KTHREAD starts with a dispatcher header (type 6), signalled on exit.
        Store8(kt + 0, kern::Thread);
        Store8(kt + 2, uint8_t(kThreadSize / 4));
        Store32(kt + 4, 0);
        Store32(kt + 8, kt + 8);
        Store32(kt + 12, kt + 8);
        Store32(kt + kThreadStackBase, t->stackBase);
        Store32(kt + kThreadStackLimit, t->stackLimit);
        Store32(kt + kThreadStackKernel, t->stackBase - 240);
        Store32(kt + kThreadTlsAddress, t->tlsSlots);
        Store8(kt + kThreadState, 1);
        Store8(kt + kThreadPriority, 8);
        Store8(kt + kThreadProcessType, 1); // title process
        Store8(kt + kThreadCurrentCpu, uint8_t(cpu));
        Store32(kt + kThreadStackAllocBase, t->stackBase);
        Store32(kt + kThreadId, t->id);
        Store32(kt + kThreadStartAddress, t->startAddress);
    }

    static void InitContext(GuestThread* t)
    {
        t->ctx = new PPCContext{};
        PPCContext& ctx = *t->ctx;
        ctx.r1.u64 = t->stackBase - 0x100; // room for the callee's LR/back-chain saves
        ctx.r13.u64 = t->pcr;
        ctx.fpscr.csr = ctx.fpscr.getcsr();
    }

    static unsigned __stdcall ThreadProc(void* param)
    {
        GuestThread* t = static_cast<GuestThread*>(param);
        t_current = t;
        {
            std::unique_lock lock(t->suspendMutex);
            t->suspendCv.wait(lock, [t] { return t->suspendCount == 0; });
        }
        if (!t->name.empty())
        {
            std::wstring w(t->name.begin(), t->name.end());
            SetThreadDescription(GetCurrentThread(), w.c_str());
        }

        PPCContext& ctx = *t->ctx;
        ctx.fpscr.csr = ctx.fpscr.getcsr();
        uint32_t target;
        if (t->xapiStartup)
        {
            ctx.r3.u64 = t->startAddress;
            ctx.r4.u64 = t->startContext;
            target = t->xapiStartup;
        }
        else
        {
            ctx.r3.u64 = t->startContext;
            target = t->startAddress;
        }

        try
        {
            CallGuest(ctx, target);
            t->exitCode = ctx.r3.u32;
        }
        catch (const ThreadExit& e)
        {
            t->exitCode = e.code;
        }

        LOGI("guest thread %u (%s) exited with %08X", t->id, t->name.c_str(), t->exitCode);
        kern::SignalObject(t->kthread);
        t_current = nullptr;
        return 0;
    }

    GuestThread* Create(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                        uint32_t xapiStartup, uint32_t creationFlags)
    {
        auto* t = new GuestThread();
        t->id = s_nextThreadId++;
        t->startAddress = startAddress;
        t->startContext = startContext;
        t->xapiStartup = xapiStartup;
        t->suspendCount = (creationFlags & 1) ? 1 : 0;
        char name[64];
        snprintf(name, sizeof(name), "guest %u (%08X)", t->id, startAddress);
        t->name = name;

        SetupGuestBlocks(t, stackSize ? stackSize : s_defaultStackSize, ChooseCpu(creationFlags));
        Store32(t->kthread + kThreadCreationFlags, creationFlags);
        InitContext(t);
        t->handle = kern::CreateHandle(kern::HandleKind::Thread, t->kthread);
        {
            std::lock_guard lock(s_threadsLock);
            s_threadsByKThread[t->kthread] = t;
        }

        t->hostHandle = reinterpret_cast<void*>(_beginthreadex(nullptr, kHostStackSize, ThreadProc, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
        LOGI("created guest thread %u: start %08X ctx %08X xapi %08X stack %08X-%08X%s", t->id, startAddress,
             startContext, xapiStartup, t->stackLimit, t->stackBase, t->suspendCount ? " (suspended)" : "");
        return t;
    }

    int Resume(GuestThread* t)
    {
        std::lock_guard lock(t->suspendMutex);
        int prev = t->suspendCount;
        if (t->suspendCount > 0 && --t->suspendCount == 0)
            t->suspendCv.notify_all();
        return prev;
    }

    void ExitCurrent(uint32_t exitCode)
    {
        throw ThreadExit{ exitCode };
    }

    uint32_t RunMain(uint32_t entryPoint)
    {
        GuestThread* t = Create(s_defaultStackSize, entryPoint, 0, 0, 1);
        t->name = "guest main";
        Resume(t);
        WaitForSingleObject(t->hostHandle, INFINITE);
        return t->exitCode;
    }

    static std::recursive_mutex s_cpuInterruptLocks[6];

    std::recursive_mutex& CpuInterruptLock(uint32_t cpu)
    {
        return s_cpuInterruptLocks[cpu % 6];
    }

    uint8_t GetIrql()
    {
        GuestThread* t = Current();
        return t ? Load8(t->pcr + kPcrCurrentIrql) : 0;
    }

    void SetIrql(uint8_t irql)
    {
        GuestThread* t = Current();
        if (!t)
            return;
        uint8_t old = Load8(t->pcr + kPcrCurrentIrql);
        if (old < 2 && irql >= 2 && t->irqLockCpu < 0)
        {
            t->irqLockCpu = Load8(t->pcr + kPcrCurrentCpu);
            CpuInterruptLock(t->irqLockCpu).lock();
        }
        else if (old >= 2 && irql < 2 && t->irqLockCpu >= 0)
        {
            CpuInterruptLock(t->irqLockCpu).unlock();
            t->irqLockCpu = -1;
        }
        Store8(t->pcr + kPcrCurrentIrql, irql);
    }

    std::vector<GuestThread*> All()
    {
        std::lock_guard lock(s_threadsLock);
        std::vector<GuestThread*> out;
        for (auto& [kt, t] : s_threadsByKThread)
            out.push_back(t);
        return out;
    }

    PPCContext& AttachHostThread(const char* name)
    {
        auto* t = new GuestThread();
        t->id = s_nextThreadId++;
        t->name = name;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->hostHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        SetupGuestBlocks(t, 0x10000, s_nextCpu++ % 6);
        InitContext(t);
        {
            std::lock_guard lock(s_threadsLock);
            s_threadsByKThread[t->kthread] = t;
        }
        t_current = t;
        return *t->ctx;
    }
}
