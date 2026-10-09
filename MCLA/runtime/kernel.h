// Kernel object model.
//
// Dispatcher objects (events, semaphores, mutants, timers, threads) live in guest
// memory as X_DISPATCHER_HEADER-prefixed structs, exactly as on the console, so
// the game can pass them to Ke* functions directly. Handles map to the guest
// address of the object (plus optional host data, e.g. an open file).
// All signalling goes through one dispatcher lock + condition variable.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct PPCContext;

namespace kern
{
    // X_DISPATCHER_HEADER.type values
    enum ObjectType : uint8_t
    {
        NotificationEvent = 0,
        SynchronizationEvent = 1,
        Mutant = 2,
        Semaphore = 5,
        Thread = 6,
        NotificationTimer = 8,
        SynchronizationTimer = 9,
    };

    enum class HandleKind { Object, File, Thread, Enumerator, Notification, Other };

    struct HostObject
    {
        virtual ~HostObject() = default;
    };

    struct HandleEntry
    {
        HandleKind kind = HandleKind::Other;
        uint32_t object = 0; // guest address of the dispatcher object (0 if none)
        std::shared_ptr<HostObject> host;
    };

    constexpr uint32_t CurrentThreadHandle = 0xFFFFFFFE;
    constexpr uint32_t CurrentProcessHandle = 0xFFFFFFFF;

    void Init();

    uint32_t CreateHandle(HandleKind kind, uint32_t object, std::shared_ptr<HostObject> host = {});
    bool LookupHandle(uint32_t handle, HandleEntry& out);
    bool CloseHandle(uint32_t handle);
    uint32_t DuplicateHandle(uint32_t handle);
    // Guest object address for a handle (handles pseudo-handles); 0 if invalid.
    uint32_t ObjectFromHandle(uint32_t handle);

    // Allocates and initialises dispatcher objects in guest memory.
    uint32_t NewEvent(bool manualReset, bool initialState);
    uint32_t NewSemaphore(int32_t count, int32_t limit);
    uint32_t NewMutant(bool initialOwner);
    uint32_t NewTimer(bool manualReset);
    void InitSemaphore(uint32_t sem, int32_t count, int32_t limit);

    // Signal operations (return previous state where the kernel API does).
    int32_t SetEvent(uint32_t ev);
    int32_t ResetEvent(uint32_t ev);
    bool ReleaseSemaphore(uint32_t sem, int32_t adjustment, int32_t* previous);
    bool ReleaseMutant(uint32_t mutant, int32_t* previous);
    void SignalObject(uint32_t object); // threads (on exit), timers

    // Timers: due time in 100 ns units (negative = relative, positive = absolute
    // system time); period in ms (0 = one-shot).
    void SetTimer(uint32_t timer, int64_t dueTime, uint32_t periodMs);
    void CancelTimer(uint32_t timer);

    // Waits. timeout: nullopt = infinite; value in 100 ns units as above.
    // Returns STATUS_WAIT_0 + index, STATUS_ABANDONED_WAIT_0 + index, or STATUS_TIMEOUT.
    uint32_t Wait(const uint32_t* objects, uint32_t count, bool waitAll, std::optional<int64_t> timeout);
    std::optional<int64_t> ReadTimeout(uint32_t timeoutPtr);
    void Sleep(std::optional<int64_t> interval);

    // Thread-local storage slots (KeTlsAlloc & co.).
    uint32_t TlsAlloc();
    bool TlsFree(uint32_t slot);
    uint32_t TlsSlotCount();

    // Time.
    uint64_t SystemTime();       // 100 ns since 1601 (FILETIME)
    constexpr uint64_t kTimebaseFrequency = 49875000;
    uint64_t TimebaseFrequency(); // frequency of mftb (calibrates the TSC scale on first call)
    void StartTimeStampThread(uint32_t timeStampBundle);
}
