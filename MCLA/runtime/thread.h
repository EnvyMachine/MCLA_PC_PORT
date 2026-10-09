// Guest threads: each runs recompiled code on a host thread with its own
// PPCContext, guest stack, PCR (r13), KTHREAD and TLS block in guest memory.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct PPCContext;

struct GuestThread
{
    uint32_t block = 0;      // guest allocation: PCR + KTHREAD + TLS
    uint32_t pcr = 0;        // X_KPCR (r13)
    uint32_t kthread = 0;    // X_KTHREAD (also the dispatcher object waited on)
    uint32_t tlsStatic = 0;  // __declspec(thread) data (PCR->tls_ptr)
    uint32_t tlsSlots = 0;   // KeTlsGetValue slot array
    uint32_t stackAlloc = 0, stackBase = 0, stackLimit = 0;
    uint32_t id = 0;
    uint32_t handle = 0;

    uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
    uint32_t exitCode = 0;
    std::string name;

    PPCContext* ctx = nullptr;
    void* hostHandle = nullptr;
    const char* lastImport = "(none)";
    int irqLockCpu = -1; // CPU whose interrupts this thread masks (IRQL >= DISPATCH)

    std::mutex suspendMutex;
    std::condition_variable suspendCv;
    int suspendCount = 0;
};

namespace thread
{
    // TLS layout from the XEX TLS_INFO header.
    void SetTlsInfo(uint32_t slotCount, uint32_t rawAddress, uint32_t rawSize, uint32_t dataSize);
    uint32_t TlsSlotCount();
    void SetDefaultStackSize(uint32_t size);

    GuestThread* Current();
    uint32_t CurrentKThread();
    GuestThread* FromKThread(uint32_t kthread);

    // Creates a guest thread (and its handle). creationFlags bit 0 = suspended.
    GuestThread* Create(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                        uint32_t xapiStartup, uint32_t creationFlags);
    // Returns the previous suspend count.
    int Resume(GuestThread* t);

    // Unwinds the calling guest thread back to its entry (ExTerminateThread).
    [[noreturn]] void ExitCurrent(uint32_t exitCode);

    // Runs the title's entry point on a new guest thread and waits for it.
    uint32_t RunMain(uint32_t entryPoint);

    // Gives a runtime-owned host thread (vblank, audio) a guest context so it can
    // call guest callbacks. The returned context is owned by that host thread.
    PPCContext& AttachHostThread(const char* name);

    // Snapshot of all guest threads (for hang dumps).
    std::vector<GuestThread*> All();

    // IRQL. Raising to DISPATCH_LEVEL (2) or above masks interrupts on the
    // thread's CPU, as on the console: the D3D interrupt handler and the threads
    // on the same CPU share per-CPU data without other locking. Interrupt delivery
    // takes the same per-CPU lock (see gpu/command_processor.cpp).
    uint8_t GetIrql();
    void SetIrql(uint8_t irql);
    std::recursive_mutex& CpuInterruptLock(uint32_t cpu);
}
