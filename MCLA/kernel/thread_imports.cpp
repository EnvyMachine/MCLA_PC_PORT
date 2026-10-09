// Thread, synchronisation, TLS, IRQL/spinlock and critical-section imports.
#include "imports.h"

#include <windows.h>
#include <immintrin.h>

#include <atomic>

namespace
{
    using thread::GetIrql;
    using thread::SetIrql;

    std::atomic_ref<uint32_t> Word(uint32_t addr)
    {
        return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(g_base + addr));
    }

    // Owner token for spinlocks/critical sections: the current KTHREAD.
    uint32_t Self()
    {
        uint32_t kt = thread::CurrentKThread();
        return kt ? kt : (0x80000000u | GetCurrentThreadId());
    }

    void SpinAcquire(uint32_t lock)
    {
        const uint32_t self = __builtin_bswap32(Self());
        for (uint32_t spins = 0;; spins++)
        {
            uint32_t expected = 0;
            if (Word(lock).compare_exchange_weak(expected, self, std::memory_order_acquire))
                return;
            if (spins < 64)
                _mm_pause();
            else
                SwitchToThread();
        }
    }

    bool SpinTryAcquire(uint32_t lock)
    {
        uint32_t expected = 0;
        return Word(lock).compare_exchange_strong(expected, __builtin_bswap32(Self()), std::memory_order_acquire);
    }

    void SpinRelease(uint32_t lock)
    {
        Word(lock).store(0, std::memory_order_release);
    }

    uint32_t StoreHandleResult(uint32_t pHandle, uint32_t handle)
    {
        if (pHandle)
            Store32(pHandle, handle);
        return xstatus::Success;
    }
}

// ---- threads -------------------------------------------------------------------

GUEST_IMPORT(ExCreateThread)
{
    TRACE("ExCreateThread");
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pThreadId = ctx.r5.u32;
    uint32_t xapiStartup = ctx.r6.u32, start = ctx.r7.u32, context = ctx.r8.u32, flags = ctx.r9.u32;
    GuestThread* t = thread::Create(stackSize, start, context, xapiStartup, flags);
    if (pHandle)
        Store32(pHandle, t->handle);
    if (pThreadId)
        Store32(pThreadId, t->id);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(ExTerminateThread)
{
    TRACE("ExTerminateThread");
    thread::ExitCurrent(ctx.r3.u32);
}

GUEST_IMPORT(NtResumeThread)
{
    TRACE("NtResumeThread");
    GuestThread* t = thread::FromKThread(kern::ObjectFromHandle(ctx.r3.u32));
    if (!t)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    int prev = thread::Resume(t);
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(KeResumeThread)
{
    TRACE("KeResumeThread");
    GuestThread* t = thread::FromKThread(ctx.r3.u32);
    ctx.r3.u64 = t ? uint32_t(thread::Resume(t)) : 0;
}

GUEST_IMPORT(KeSetAffinityThread)
{
    TRACE("KeSetAffinityThread");
    // Host scheduling decides where the thread really runs, but the title reads its
    // "current CPU" (PCR->prcb.current_cpu) for per-CPU data - e.g. the XAudio
    // workers rendezvous on flags indexed by it - so move the thread's nominal CPU
    // to the requested hardware thread.
    uint32_t kthread = ctx.r3.u32, affinity = ctx.r4.u32 & 0x3F;
    uint8_t current = Load8(kthread + 0xBF);
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, 1u << current);
    if (affinity && !(affinity & (1u << current)))
    {
        uint8_t cpu = uint8_t(31 - __builtin_clz(affinity)); // highest set bit, as in Xenia
        Store8(kthread + 0xBF, cpu);
        if (GuestThread* t = thread::FromKThread(kthread))
            Store8(t->pcr + 0x10C, cpu);
    }
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(KeSetBasePriorityThread)
{
    TRACE("KeSetBasePriorityThread");
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KeSetDisableBoostThread)
{
    TRACE("KeSetDisableBoostThread");
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KeDelayExecutionThread)
{
    TRACE("KeDelayExecutionThread");
    kern::Sleep(kern::ReadTimeout(ctx.r5.u32));
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(KeGetCurrentProcessType)
{
    TRACE("KeGetCurrentProcessType");
    ctx.r3.u64 = 1; // title process
}

GUEST_IMPORT(KeEnterCriticalRegion) { TRACE("KeEnterCriticalRegion"); }
GUEST_IMPORT(KeLeaveCriticalRegion) { TRACE("KeLeaveCriticalRegion"); }
GUEST_IMPORT(KiApcNormalRoutineNop) { TRACE("KiApcNormalRoutineNop"); }

GUEST_IMPORT(KeQueryPerformanceFrequency)
{
    TRACE("KeQueryPerformanceFrequency");
    ctx.r3.u64 = kern::TimebaseFrequency();
}

GUEST_IMPORT(KeQuerySystemTime)
{
    TRACE("KeQuerySystemTime");
    if (ctx.r3.u32)
        Store64(ctx.r3.u32, kern::SystemTime());
}

GUEST_IMPORT(KeBugCheck)
{
    LOGI("FATAL: KeBugCheck(%08X)", ctx.r3.u32);
    LogFlush();
    __debugbreak();
}

GUEST_IMPORT(KeBugCheckEx)
{
    LOGI("FATAL: KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    LogFlush();
    __debugbreak();
}

// ---- IRQL and spinlocks --------------------------------------------------------

GUEST_IMPORT(KeRaiseIrqlToDpcLevel)
{
    TRACE("KeRaiseIrqlToDpcLevel");
    uint8_t old = GetIrql();
    SetIrql(2);
    ctx.r3.u64 = old;
}

GUEST_IMPORT(KfLowerIrql)
{
    TRACE("KfLowerIrql");
    SetIrql(uint8_t(ctx.r3.u32));
}

GUEST_IMPORT(KfAcquireSpinLock)
{
    TRACE("KfAcquireSpinLock");
    uint8_t old = GetIrql();
    SetIrql(2);
    SpinAcquire(ctx.r3.u32);
    ctx.r3.u64 = old;
}

GUEST_IMPORT(KfReleaseSpinLock)
{
    TRACE("KfReleaseSpinLock");
    SpinRelease(ctx.r3.u32);
    SetIrql(uint8_t(ctx.r4.u32));
}

GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql)
{
    TRACE("KeAcquireSpinLockAtRaisedIrql");
    SpinAcquire(ctx.r3.u32);
}

GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql)
{
    TRACE("KeReleaseSpinLockFromRaisedIrql");
    SpinRelease(ctx.r3.u32);
}

GUEST_IMPORT(KeTryToAcquireSpinLockAtRaisedIrql)
{
    TRACE("KeTryToAcquireSpinLockAtRaisedIrql");
    ctx.r3.u64 = SpinTryAcquire(ctx.r3.u32) ? 1 : 0;
}

GUEST_IMPORT(KeLockL2) { TRACE("KeLockL2"); ctx.r3.u64 = 0; }
GUEST_IMPORT(KeUnlockL2) { TRACE("KeUnlockL2"); ctx.r3.u64 = 0; }

// ---- TLS -----------------------------------------------------------------------

GUEST_IMPORT(KeTlsAlloc)
{
    TRACE("KeTlsAlloc");
    ctx.r3.u64 = kern::TlsAlloc();
}

GUEST_IMPORT(KeTlsFree)
{
    TRACE("KeTlsFree");
    ctx.r3.u64 = kern::TlsFree(ctx.r3.u32) ? 1 : 0;
}

GUEST_IMPORT(KeTlsGetValue)
{
    GuestThread* t = thread::Current();
    uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = (t && slot < kern::TlsSlotCount()) ? Load32(t->tlsSlots + slot * 4) : 0;
}

GUEST_IMPORT(KeTlsSetValue)
{
    GuestThread* t = thread::Current();
    uint32_t slot = ctx.r3.u32;
    if (t && slot < kern::TlsSlotCount())
    {
        Store32(t->tlsSlots + slot * 4, ctx.r4.u32);
        ctx.r3.u64 = 1;
    }
    else
        ctx.r3.u64 = 0;
}

// ---- events, semaphores, mutants, timers ---------------------------------------

GUEST_IMPORT(NtCreateEvent)
{
    TRACE("NtCreateEvent");
    // event_type: 0 = notification (manual reset), 1 = synchronization (auto reset)
    uint32_t ev = kern::NewEvent(ctx.r5.u32 == 0, ctx.r6.u32 != 0);
    ctx.r3.u64 = StoreHandleResult(ctx.r3.u32, kern::CreateHandle(kern::HandleKind::Object, ev));
}

GUEST_IMPORT(NtSetEvent)
{
    TRACE("NtSetEvent");
    uint32_t ev = kern::ObjectFromHandle(ctx.r3.u32);
    if (!ev)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    int32_t prev = kern::SetEvent(ev);
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtClearEvent)
{
    TRACE("NtClearEvent");
    uint32_t ev = kern::ObjectFromHandle(ctx.r3.u32);
    if (!ev)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    kern::ResetEvent(ev);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(KeSetEvent)
{
    TRACE("KeSetEvent");
    ctx.r3.u64 = uint32_t(kern::SetEvent(ctx.r3.u32));
}

GUEST_IMPORT(KeResetEvent)
{
    TRACE("KeResetEvent");
    ctx.r3.u64 = uint32_t(kern::ResetEvent(ctx.r3.u32));
}

GUEST_IMPORT(NtCreateSemaphore)
{
    TRACE("NtCreateSemaphore");
    uint32_t sem = kern::NewSemaphore(int32_t(ctx.r5.u32), int32_t(ctx.r6.u32));
    ctx.r3.u64 = StoreHandleResult(ctx.r3.u32, kern::CreateHandle(kern::HandleKind::Object, sem));
}

GUEST_IMPORT(NtReleaseSemaphore)
{
    TRACE("NtReleaseSemaphore");
    uint32_t sem = kern::ObjectFromHandle(ctx.r3.u32);
    if (!sem)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    int32_t prev = 0;
    bool ok = kern::ReleaseSemaphore(sem, int32_t(ctx.r4.u32), &prev);
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, uint32_t(prev));
    ctx.r3.u64 = ok ? xstatus::Success : xstatus::SemaphoreLimitExceeded;
}

GUEST_IMPORT(KeInitializeSemaphore)
{
    TRACE("KeInitializeSemaphore");
    kern::InitSemaphore(ctx.r3.u32, int32_t(ctx.r4.u32), int32_t(ctx.r5.u32));
}

GUEST_IMPORT(KeReleaseSemaphore)
{
    TRACE("KeReleaseSemaphore");
    int32_t prev = 0;
    if (!kern::ReleaseSemaphore(ctx.r3.u32, int32_t(ctx.r5.u32), &prev))
        LOGI("KeReleaseSemaphore: limit exceeded on %08X", ctx.r3.u32);
    ctx.r3.u64 = uint32_t(prev);
}

GUEST_IMPORT(NtCreateMutant)
{
    TRACE("NtCreateMutant");
    uint32_t m = kern::NewMutant(ctx.r5.u32 != 0);
    ctx.r3.u64 = StoreHandleResult(ctx.r3.u32, kern::CreateHandle(kern::HandleKind::Object, m));
}

GUEST_IMPORT(NtReleaseMutant)
{
    TRACE("NtReleaseMutant");
    uint32_t m = kern::ObjectFromHandle(ctx.r3.u32);
    if (!m)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    int32_t prev = 0;
    bool ok = kern::ReleaseMutant(m, &prev);
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, uint32_t(prev));
    ctx.r3.u64 = ok ? xstatus::Success : xstatus::MutantNotOwned;
}

GUEST_IMPORT(NtCreateTimer)
{
    TRACE("NtCreateTimer");
    // timer_type: 0 = notification, 1 = synchronization
    uint32_t t = kern::NewTimer(ctx.r5.u32 == 0);
    ctx.r3.u64 = StoreHandleResult(ctx.r3.u32, kern::CreateHandle(kern::HandleKind::Object, t));
}

GUEST_IMPORT(NtSetTimerEx)
{
    TRACE("NtSetTimerEx");
    uint32_t timer = kern::ObjectFromHandle(ctx.r3.u32);
    if (!timer || !ctx.r4.u32)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    if (ctx.r5.u32)
        LOGI("NtSetTimerEx: APC routine %08X ignored", ctx.r5.u32);
    uint32_t period = Arg(ctx, 6);
    kern::SetTimer(timer, int64_t(Load64(ctx.r4.u32)), period);
    if (uint32_t pPrev = Arg(ctx, 7))
        Store32(pPrev, 0);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(NtCancelTimer)
{
    TRACE("NtCancelTimer");
    uint32_t timer = kern::ObjectFromHandle(ctx.r3.u32);
    if (timer)
        kern::CancelTimer(timer);
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = timer ? xstatus::Success : xstatus::InvalidHandle;
}

// ---- waits ---------------------------------------------------------------------

GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    TRACE("NtWaitForSingleObjectEx");
    uint32_t obj = kern::ObjectFromHandle(ctx.r3.u32);
    if (!obj)
    {
        LOGI("NtWaitForSingleObjectEx: invalid handle %08X", ctx.r3.u32);
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    ctx.r3.u64 = kern::Wait(&obj, 1, false, kern::ReadTimeout(ctx.r6.u32));
}

GUEST_IMPORT(NtWaitForMultipleObjectsEx)
{
    TRACE("NtWaitForMultipleObjectsEx");
    uint32_t count = ctx.r3.u32, handles = ctx.r4.u32, waitType = ctx.r5.u32;
    uint32_t objects[64];
    if (count == 0 || count > 64)
    {
        ctx.r3.u64 = xstatus::InvalidParameter;
        return;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        objects[i] = kern::ObjectFromHandle(Load32(handles + i * 4));
        if (!objects[i])
        {
            ctx.r3.u64 = xstatus::InvalidHandle;
            return;
        }
    }
    ctx.r3.u64 = kern::Wait(objects, count, waitType == 0, kern::ReadTimeout(Arg(ctx, 5)));
}

GUEST_IMPORT(KeWaitForSingleObject)
{
    TRACE("KeWaitForSingleObject");
    uint32_t obj = ctx.r3.u32;
    ctx.r3.u64 = kern::Wait(&obj, 1, false, kern::ReadTimeout(ctx.r7.u32));
}

GUEST_IMPORT(KeWaitForMultipleObjects)
{
    TRACE("KeWaitForMultipleObjects");
    uint32_t count = ctx.r3.u32, list = ctx.r4.u32, waitType = ctx.r5.u32;
    uint32_t objects[64];
    if (count == 0 || count > 64)
    {
        ctx.r3.u64 = xstatus::InvalidParameter;
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        objects[i] = Load32(list + i * 4);
    ctx.r3.u64 = kern::Wait(objects, count, waitType == 0, kern::ReadTimeout(Arg(ctx, 6)));
}

// ---- handles and object manager ------------------------------------------------

GUEST_IMPORT(NtClose)
{
    TRACE("NtClose");
    ctx.r3.u64 = kern::CloseHandle(ctx.r3.u32) ? xstatus::Success : xstatus::InvalidHandle;
}

GUEST_IMPORT(NtDuplicateObject)
{
    TRACE("NtDuplicateObject");
    uint32_t dup = kern::DuplicateHandle(ctx.r3.u32);
    if (!dup)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    if (ctx.r4.u32)
        Store32(ctx.r4.u32, dup);
    if (ctx.r5.u32 & 1) // DUPLICATE_CLOSE_SOURCE
        kern::CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(ObReferenceObjectByHandle)
{
    TRACE("ObReferenceObjectByHandle");
    uint32_t obj = kern::ObjectFromHandle(ctx.r3.u32);
    if (!obj)
    {
        ctx.r3.u64 = xstatus::InvalidHandle;
        return;
    }
    if (ctx.r5.u32)
        Store32(ctx.r5.u32, obj);
    ctx.r3.u64 = xstatus::Success;
}

GUEST_IMPORT(ObReferenceObject) { TRACE("ObReferenceObject"); }
GUEST_IMPORT(ObDereferenceObject) { TRACE("ObDereferenceObject"); }
GUEST_IMPORT(ObIsTitleObject) { TRACE("ObIsTitleObject"); ctx.r3.u64 = 1; }

// ---- critical sections ---------------------------------------------------------
// X_RTL_CRITICAL_SECTION: header (0x10), be32 lock_count (-1 = free),
// be32 recursion_count, be32 owning_thread.

GUEST_IMPORT(RtlInitializeCriticalSection)
{
    TRACE("RtlInitializeCriticalSection");
    uint32_t cs = ctx.r3.u32;
    memset(g_base + cs, 0, 0x1C);
    Store8(cs, kern::SynchronizationEvent);
    Store32(cs + 0x10, 0xFFFFFFFF);
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t self = Self();
    if (Load32(cs + 0x18) == self)
    {
        Store32(cs + 0x14, Load32(cs + 0x14) + 1);
        Word(cs + 0x10).store(__builtin_bswap32(Load32(cs + 0x10) + 1));
        return;
    }
    auto lockCount = Word(cs + 0x10);
    for (uint32_t spins = 0;; spins++)
    {
        uint32_t expected = 0xFFFFFFFF;
        if (lockCount.compare_exchange_weak(expected, 0, std::memory_order_acquire))
            break;
        if (spins < 256)
            _mm_pause();
        else
            WaitOnAddress(g_base + cs + 0x10, &expected, 4, 1);
    }
    Store32(cs + 0x18, self);
    Store32(cs + 0x14, 1);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t self = Self();
    if (Load32(cs + 0x18) == self)
    {
        Store32(cs + 0x14, Load32(cs + 0x14) + 1);
        Word(cs + 0x10).store(__builtin_bswap32(Load32(cs + 0x10) + 1));
        ctx.r3.u64 = 1;
        return;
    }
    uint32_t expected = 0xFFFFFFFF;
    if (Word(cs + 0x10).compare_exchange_strong(expected, 0, std::memory_order_acquire))
    {
        Store32(cs + 0x18, self);
        Store32(cs + 0x14, 1);
        ctx.r3.u64 = 1;
    }
    else
        ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t recursion = Load32(cs + 0x14);
    if (recursion > 1)
    {
        Store32(cs + 0x14, recursion - 1);
        Word(cs + 0x10).store(__builtin_bswap32(Load32(cs + 0x10) - 1));
        return;
    }
    Store32(cs + 0x14, 0);
    Store32(cs + 0x18, 0);
    Word(cs + 0x10).store(0xFFFFFFFF, std::memory_order_release);
    WakeByAddressAll(g_base + cs + 0x10);
}
