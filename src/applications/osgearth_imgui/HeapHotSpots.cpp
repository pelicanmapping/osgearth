#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#include "HeapHotspots.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "Psapi.lib")

namespace
{
    // Start high. Lower this if the report doesn't explain enough memory.
    constexpr std::size_t kMinimumTrackedSize = 2;//2u * 1024u;
    constexpr USHORT kMaximumFrames = 32;

    using HeapAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
    using HeapReAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
    using HeapFreeFunction = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
    using RtlAllocateHeapFunction = PVOID(NTAPI*)(PVOID, ULONG, SIZE_T);
    using RtlReAllocateHeapFunction = PVOID(NTAPI*)(PVOID, ULONG, PVOID, SIZE_T);
    using RtlFreeHeapFunction = BOOLEAN(NTAPI*)(PVOID, ULONG, PVOID);
    using VirtualAllocFunction = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
    using VirtualFreeFunction = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
    using GlobalAllocFunction = HGLOBAL(WINAPI*)(UINT, SIZE_T);
    using GlobalReAllocFunction = HGLOBAL(WINAPI*)(HGLOBAL, SIZE_T, UINT);
    using GlobalFreeFunction = HGLOBAL(WINAPI*)(HGLOBAL);
    using LocalAllocFunction = HLOCAL(WINAPI*)(UINT, SIZE_T);
    using LocalReAllocFunction = HLOCAL(WINAPI*)(HLOCAL, SIZE_T, UINT);
    using LocalFreeFunction = HLOCAL(WINAPI*)(HLOCAL);
    using GetProcAddressFunction = FARPROC(WINAPI*)(HMODULE, LPCSTR);

    HeapAllocFunction g_heapAlloc = nullptr;
    HeapReAllocFunction g_heapReAlloc = nullptr;
    HeapFreeFunction g_heapFree = nullptr;
    RtlAllocateHeapFunction g_rtlAllocateHeap = nullptr;
    RtlReAllocateHeapFunction g_rtlReAllocateHeap = nullptr;
    RtlFreeHeapFunction g_rtlFreeHeap = nullptr;
    VirtualAllocFunction g_virtualAlloc = nullptr;
    VirtualFreeFunction g_virtualFree = nullptr;
    GlobalAllocFunction g_globalAlloc = nullptr;
    GlobalReAllocFunction g_globalReAlloc = nullptr;
    GlobalFreeFunction g_globalFree = nullptr;
    LocalAllocFunction g_localAlloc = nullptr;
    LocalReAllocFunction g_localReAlloc = nullptr;
    LocalFreeFunction g_localFree = nullptr;
    GetProcAddressFunction g_getProcAddress = nullptr;

    // Cached so the patching code never has to call GetModuleHandleW, which
    // takes the loader lock; the DLL-load notification callback already runs
    // while holding it.
    HMODULE g_ntdllModule = nullptr;
    HMODULE g_kernel32Module = nullptr;
    HMODULE g_kernelBaseModule = nullptr;

    bool g_hooksResolved = false;
    std::atomic<unsigned> g_patchedModuleCount{ 0 };

    thread_local bool g_insideTracker = false;
    thread_local unsigned g_heapApiCallDepth = 0;

    class ReentryGuard
    {
    public:
        ReentryGuard() noexcept
            : previous_(g_insideTracker)
        {
            g_insideTracker = true;
        }

        ~ReentryGuard()
        {
            g_insideTracker = previous_;
        }

        ReentryGuard(const ReentryGuard&) = delete;
        ReentryGuard& operator=(const ReentryGuard&) = delete;

    private:
        bool previous_;
    };

    class HeapApiCallGuard
    {
    public:
        HeapApiCallGuard() noexcept
            : shouldTrack_(!g_insideTracker && g_heapApiCallDepth == 0)
        {
            ++g_heapApiCallDepth;
        }

        ~HeapApiCallGuard()
        {
            --g_heapApiCallDepth;
        }

        bool shouldTrack() const noexcept { return shouldTrack_; }

    private:
        bool shouldTrack_;
    };

    enum class AllocationSource : std::uint8_t
    {
        Heap,
        Virtual
    };

    std::atomic<std::uint64_t> g_trackerStorageBytes{ 0 };
    std::atomic<std::uint64_t> g_trackerStorageBlocks{ 0 };

    // Count the profiler's own container storage. Calls made by this allocator
    // run under ReentryGuard, so they deliberately do not appear as application
    // allocations, but they still occupy the process heap and must be removed
    // from the HeapWalk totals in the report.
    template<typename T>
    class TrackerAllocator
    {
    public:
        using value_type = T;

        TrackerAllocator() noexcept = default;

        template<typename U>
        TrackerAllocator(const TrackerAllocator<U>&) noexcept
        {
        }

        T* allocate(std::size_t count)
        {
            T* result = std::allocator<T>{}.allocate(count);
            g_trackerStorageBytes.fetch_add(
                count * sizeof(T), std::memory_order_relaxed);
            g_trackerStorageBlocks.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        void deallocate(T* pointer, std::size_t count) noexcept
        {
            g_trackerStorageBytes.fetch_sub(
                count * sizeof(T), std::memory_order_relaxed);
            g_trackerStorageBlocks.fetch_sub(1, std::memory_order_relaxed);
            std::allocator<T>{}.deallocate(pointer, count);
        }

        template<typename U>
        bool operator==(const TrackerAllocator<U>&) const noexcept { return true; }

        template<typename U>
        bool operator!=(const TrackerAllocator<U>&) const noexcept { return false; }
    };

    struct StackTrace
    {
        USHORT frameCount = 0;
        void* frames[kMaximumFrames] = {};

        bool operator==(const StackTrace& rhs) const noexcept
        {
            return frameCount == rhs.frameCount &&
                std::memcmp(frames, rhs.frames, frameCount * sizeof(void*)) == 0;
        }
    };

    struct StackTraceHash
    {
        std::size_t operator()(const StackTrace& trace) const noexcept
        {
            std::size_t hash = static_cast<std::size_t>(1469598103934665603ull);
            for (USHORT i = 0; i < trace.frameCount; ++i)
            {
                hash ^= reinterpret_cast<std::size_t>(trace.frames[i]);
                hash *= static_cast<std::size_t>(1099511628211ull);
            }
            return hash;
        }
    };

    struct StackTotals
    {
        std::uint64_t heapBytes = 0;
        std::uint64_t heapCount = 0;
        std::uint64_t virtualBytes = 0;
        std::uint64_t virtualCount = 0;
        std::uint64_t largestAllocation = 0;
        std::size_t dumpIndex = static_cast<std::size_t>(-1);
    };

    struct Allocation
    {
        std::size_t requestedSize = 0;
        AllocationSource source = AllocationSource::Heap;
        StackTotals* totals = nullptr;
    };

    struct TrackerState
    {
        using LiveValue = std::pair<void* const, Allocation>;
        using StackValue = std::pair<const StackTrace, StackTotals>;
        using LiveMap = std::unordered_map<
            void*, Allocation, std::hash<void*>, std::equal_to<void*>,
            TrackerAllocator<LiveValue>>;
        using StackMap = std::unordered_map<
            StackTrace, StackTotals, StackTraceHash, std::equal_to<StackTrace>,
            TrackerAllocator<StackValue>>;

        std::mutex mutex;
        LiveMap live;
        StackMap stacks;
    };

    void addToTotals(
        StackTotals& totals, std::size_t size, AllocationSource source) noexcept
    {
        if (source == AllocationSource::Virtual)
        {
            totals.virtualBytes += size;
            ++totals.virtualCount;
        }
        else
        {
            totals.heapBytes += size;
            ++totals.heapCount;
        }
    }

    void removeFromTotals(
        StackTotals& totals, std::size_t size, AllocationSource source) noexcept
    {
        if (source == AllocationSource::Virtual)
        {
            totals.virtualBytes -= size;
            --totals.virtualCount;
        }
        else
        {
            totals.heapBytes -= size;
            --totals.heapCount;
        }
        // largestAllocation is recomputed from the live records during dump;
        // maintaining a per-stack size tree on every allocation would cost
        // more memory than the value is worth.
    }

    TrackerState& trackerState()
    {
        // Intentionally never destroyed. This avoids static-destruction-order
        // problems caused by operator delete running during shutdown.
        static TrackerState* state = []() -> TrackerState*
        {
            ReentryGuard guard;

            void* storage = ::HeapAlloc(
                ::GetProcessHeap(),
                HEAP_ZERO_MEMORY,
                sizeof(TrackerState));

            if (!storage)
                std::abort();

            g_trackerStorageBytes.fetch_add(
                sizeof(TrackerState), std::memory_order_relaxed);
            g_trackerStorageBlocks.fetch_add(1, std::memory_order_relaxed);
            return ::new (storage) TrackerState();
        }();

        return *state;
    }

    void trackAllocation(void* pointer, std::size_t size, AllocationSource source) noexcept
    {
        if (!pointer ||
            size < kMinimumTrackedSize ||
            g_insideTracker)
        {
            return;
        }

        ReentryGuard guard;

        StackTrace trace;

        // Skip the heap hook and tracker frames. The exact ideal value may
        // vary slightly depending on optimization and inlining.
        trace.frameCount = ::CaptureStackBackTrace(
            2,
            kMaximumFrames,
            trace.frames,
            nullptr);

        try
        {
            TrackerState& state = trackerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            auto stackResult = state.stacks.try_emplace(trace);

            Allocation allocation;
            allocation.requestedSize = size;
            allocation.source = source;
            allocation.totals = &stackResult.first->second;

            // If a free was missed through an unpatched path, an address can
            // be reused. Remove the stale record from its aggregate first.
            auto existing = state.live.find(pointer);
            if (existing != state.live.end())
            {
                removeFromTotals(
                    *existing->second.totals,
                    existing->second.requestedSize,
                    existing->second.source);
                existing->second = allocation;
            }
            else
            {
                state.live.emplace(pointer, allocation);
            }

            addToTotals(stackResult.first->second, size, source);
        }
        catch (...)
        {
            // A diagnostic tracker must never break the application.
        }
    }

    void trackFree(void* pointer) noexcept
    {
        if (!pointer || g_insideTracker)
            return;

        ReentryGuard guard;

        try
        {
            TrackerState& state = trackerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            auto existing = state.live.find(pointer);
            if (existing != state.live.end())
            {
                removeFromTotals(
                    *existing->second.totals,
                    existing->second.requestedSize,
                    existing->second.source);
                state.live.erase(existing);
            }
        }
        catch (...)
        {
        }
    }

    LPVOID WINAPI heapAllocHook(HANDLE heap, DWORD flags, SIZE_T size)
    {
        LPVOID pointer;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            pointer = g_heapAlloc(heap, flags, size);
        }
        if (shouldTrack)
            trackAllocation(pointer, size, AllocationSource::Heap);
        return pointer;
    }

    LPVOID WINAPI heapReAllocHook(
        HANDLE heap,
        DWORD flags,
        LPVOID pointer,
        SIZE_T size)
    {
        LPVOID reallocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            reallocated = g_heapReAlloc(heap, flags, pointer, size);
        }
        if (shouldTrack && reallocated)
        {
            trackFree(pointer);
            trackAllocation(reallocated, size, AllocationSource::Heap);
        }
        return reallocated;
    }

    BOOL WINAPI heapFreeHook(HANDLE heap, DWORD flags, LPVOID pointer)
    {
        BOOL freed;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            freed = g_heapFree(heap, flags, pointer);
        }
        if (shouldTrack && freed)
            trackFree(pointer);
        return freed;
    }

    PVOID NTAPI rtlAllocateHeapHook(PVOID heap, ULONG flags, SIZE_T size)
    {
        PVOID allocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            allocated = g_rtlAllocateHeap(heap, flags, size);
        }

        if (shouldTrack)
            trackAllocation(allocated, size, AllocationSource::Heap);

        return allocated;
    }

    PVOID NTAPI rtlReAllocateHeapHook(
        PVOID heap,
        ULONG flags,
        PVOID pointer,
        SIZE_T size)
    {
        PVOID reallocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            reallocated = g_rtlReAllocateHeap(heap, flags, pointer, size);
        }

        if (shouldTrack && reallocated)
        {
            trackFree(pointer);
            trackAllocation(reallocated, size, AllocationSource::Heap);
        }

        return reallocated;
    }

    BOOLEAN NTAPI rtlFreeHeapHook(PVOID heap, ULONG flags, PVOID pointer)
    {
        BOOLEAN freed;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            freed = g_rtlFreeHeap(heap, flags, pointer);
        }

        if (shouldTrack && freed)
            trackFree(pointer);

        return freed;
    }

    LPVOID WINAPI virtualAllocHook(
        LPVOID address,
        SIZE_T size,
        DWORD allocationType,
        DWORD protect)
    {
        LPVOID allocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            allocated = g_virtualAlloc(address, size, allocationType, protect);
        }

        if (shouldTrack && (allocationType & MEM_COMMIT) != 0)
            trackAllocation(allocated, size, AllocationSource::Virtual);

        return allocated;
    }

    BOOL WINAPI virtualFreeHook(
        LPVOID address,
        SIZE_T size,
        DWORD freeType)
    {
        BOOL freed;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            freed = g_virtualFree(address, size, freeType);
        }

        if (shouldTrack && freed)
            trackFree(address);

        return freed;
    }

    HGLOBAL WINAPI globalAllocHook(UINT flags, SIZE_T size)
    {
        HGLOBAL allocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            allocated = g_globalAlloc(flags, size);
        }
        if (shouldTrack)
            trackAllocation(allocated, size, AllocationSource::Heap);
        return allocated;
    }

    HGLOBAL WINAPI globalReAllocHook(HGLOBAL memory, SIZE_T size, UINT flags)
    {
        HGLOBAL reallocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            reallocated = g_globalReAlloc(memory, size, flags);
        }
        // GMEM_MODIFY changes attributes only; the size argument is ignored.
        if (shouldTrack && reallocated && (flags & GMEM_MODIFY) == 0)
        {
            trackFree(memory);
            trackAllocation(reallocated, size, AllocationSource::Heap);
        }
        return reallocated;
    }

    HGLOBAL WINAPI globalFreeHook(HGLOBAL memory)
    {
        HGLOBAL result;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            result = g_globalFree(memory);
        }
        // GlobalFree returns NULL on success.
        if (shouldTrack && result == nullptr)
            trackFree(memory);
        return result;
    }

    HLOCAL WINAPI localAllocHook(UINT flags, SIZE_T size)
    {
        HLOCAL allocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            allocated = g_localAlloc(flags, size);
        }
        if (shouldTrack)
            trackAllocation(allocated, size, AllocationSource::Heap);
        return allocated;
    }

    HLOCAL WINAPI localReAllocHook(HLOCAL memory, SIZE_T size, UINT flags)
    {
        HLOCAL reallocated;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            reallocated = g_localReAlloc(memory, size, flags);
        }
        if (shouldTrack && reallocated && (flags & LMEM_MODIFY) == 0)
        {
            trackFree(memory);
            trackAllocation(reallocated, size, AllocationSource::Heap);
        }
        return reallocated;
    }

    HLOCAL WINAPI localFreeHook(HLOCAL memory)
    {
        HLOCAL result;
        bool shouldTrack;
        {
            HeapApiCallGuard guard;
            shouldTrack = guard.shouldTrack();
            result = g_localFree(memory);
        }
        // LocalFree returns NULL on success.
        if (shouldTrack && result == nullptr)
            trackFree(memory);
        return result;
    }

    FARPROC WINAPI getProcAddressHook(HMODULE module, LPCSTR name);

    void* replacementForImport(const char* name)
    {
        struct HookEntry
        {
            const char* name;
            void* replacement;
        };

        static const HookEntry entries[] =
        {
            { "HeapAlloc",         reinterpret_cast<void*>(&heapAllocHook) },
            { "HeapReAlloc",       reinterpret_cast<void*>(&heapReAllocHook) },
            { "HeapFree",          reinterpret_cast<void*>(&heapFreeHook) },
            { "RtlAllocateHeap",   reinterpret_cast<void*>(&rtlAllocateHeapHook) },
            { "RtlReAllocateHeap", reinterpret_cast<void*>(&rtlReAllocateHeapHook) },
            { "RtlFreeHeap",       reinterpret_cast<void*>(&rtlFreeHeapHook) },
            { "VirtualAlloc",      reinterpret_cast<void*>(&virtualAllocHook) },
            { "VirtualFree",       reinterpret_cast<void*>(&virtualFreeHook) },
            { "GlobalAlloc",       reinterpret_cast<void*>(&globalAllocHook) },
            { "GlobalReAlloc",     reinterpret_cast<void*>(&globalReAllocHook) },
            { "GlobalFree",        reinterpret_cast<void*>(&globalFreeHook) },
            { "LocalAlloc",        reinterpret_cast<void*>(&localAllocHook) },
            { "LocalReAlloc",      reinterpret_cast<void*>(&localReAllocHook) },
            { "LocalFree",         reinterpret_cast<void*>(&localFreeHook) },
            { "GetProcAddress",    reinterpret_cast<void*>(&getProcAddressHook) },
        };

        for (const HookEntry& entry : entries)
        {
            if (std::strcmp(name, entry.name) == 0)
                return entry.replacement;
        }

        return nullptr;
    }

    // Modules that resolve allocation functions at runtime (the OpenGL
    // driver, plugin loaders, anything using LoadLibrary/GetProcAddress)
    // bypass IAT patches entirely. Intercepting GetProcAddress closes that
    // hole: when one of the hooked names is requested from the system
    // modules that actually export it, hand back our hook instead.
    FARPROC WINAPI getProcAddressHook(HMODULE module, LPCSTR name)
    {
        FARPROC result = g_getProcAddress(module, name);

        // Ordinal lookups have no name to match.
        if (!result || !name || HIWORD(name) == 0)
            return result;

        // Only substitute for the modules that genuinely export the heap
        // APIs. A same-named export from an unrelated DLL must resolve
        // normally. API-set handles (api-ms-win-core-heap-*) resolve to
        // their host module, so kernelbase is covered by this check.
        if (module != g_ntdllModule &&
            module != g_kernel32Module &&
            module != g_kernelBaseModule)
        {
            return result;
        }

        void* replacement = replacementForImport(name);
        return replacement ? reinterpret_cast<FARPROC>(replacement) : result;
    }

    bool patchThunk(IMAGE_THUNK_DATA* thunk, void* replacement)
    {
        if (thunk->u1.Function == reinterpret_cast<ULONG_PTR>(replacement))
            return false;

        DWORD previousProtection;
        if (!::VirtualProtect(
                &thunk->u1.Function,
                sizeof(thunk->u1.Function),
                PAGE_READWRITE,
                &previousProtection))
        {
            return false;
        }

        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

        DWORD ignored;
        ::VirtualProtect(
            &thunk->u1.Function,
            sizeof(thunk->u1.Function),
            previousProtection,
            &ignored);

        return true;
    }

    // Returns true if any thunk was newly patched.
    bool patchImportAddressTable(HMODULE module)
    {
        // Do not modify the loader's own import tables. Patching these core
        // modules can affect loader/runtime initialization and is unnecessary:
        // application and CRT modules call the heap APIs through their own IATs.
        if (module == g_ntdllModule ||
            module == g_kernel32Module ||
            module == g_kernelBaseModule)
        {
            return false;
        }

        bool patchedAny = false;

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const IMAGE_DATA_DIRECTORY& importDirectory =
            ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

        if (importDirectory.VirtualAddress)
        {
            auto* importDescriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                base + importDirectory.VirtualAddress);

            for (; importDescriptor->Name; ++importDescriptor)
            {
                // Imports without an original thunk table cannot be identified
                // by name after the loader has resolved them.
                if (!importDescriptor->OriginalFirstThunk)
                    continue;

                auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + importDescriptor->OriginalFirstThunk);
                auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + importDescriptor->FirstThunk);

                for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal))
                        continue;

                    auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        base + originalThunk->u1.AddressOfData);

                    void* replacement = replacementForImport(
                        reinterpret_cast<const char*>(importByName->Name));

                    if (replacement)
                        patchedAny |= patchThunk(firstThunk, replacement);
                }
            }
        }

        // Delay-load imports resolve through GetProcAddress at first call,
        // which would bypass the static IAT hooks entirely. Patching the
        // delay-load address table up front short-circuits that: the entry
        // jumps straight to our hook instead of the resolver stub.
        const IMAGE_DATA_DIRECTORY& delayDirectory =
            ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];

        if (delayDirectory.VirtualAddress)
        {
            auto* delayDescriptor = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(
                base + delayDirectory.VirtualAddress);

            for (; delayDescriptor->DllNameRVA; ++delayDescriptor)
            {
                // Only the modern RVA-based descriptor form is supported.
                if ((delayDescriptor->Attributes.AllAttributes & 1) == 0 ||
                    !delayDescriptor->ImportAddressTableRVA ||
                    !delayDescriptor->ImportNameTableRVA)
                {
                    continue;
                }

                auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + delayDescriptor->ImportNameTableRVA);
                auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + delayDescriptor->ImportAddressTableRVA);

                for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
                        continue;

                    auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        base + nameThunk->u1.AddressOfData);

                    void* replacement = replacementForImport(
                        reinterpret_cast<const char*>(importByName->Name));

                    if (replacement)
                        patchedAny |= patchThunk(addressThunk, replacement);
                }
            }
        }

        return patchedAny;
    }

    std::mutex& patchMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    void patchModule(HMODULE module)
    {
        if (!g_hooksResolved || !module)
            return;

        bool patchedAny;
        {
            std::lock_guard<std::mutex> lock(patchMutex());
            patchedAny = patchImportAddressTable(module);
        }

        if (patchedAny)
            ++g_patchedModuleCount;
    }

    void patchAllLoadedModules()
    {
        if (!g_hooksResolved)
            return;

        // Enumerate before taking patchMutex: EnumProcessModules acquires the
        // loader lock, and the DLL-load notification callback runs while
        // holding it, so nesting the two locks in the opposite order here
        // would deadlock.
        DWORD requiredBytes = 0;
        ::EnumProcessModules(::GetCurrentProcess(), nullptr, 0, &requiredBytes);
        if (!requiredBytes)
            return;

        std::vector<HMODULE> modules;
        do
        {
            modules.resize(requiredBytes / sizeof(HMODULE));
            if (!::EnumProcessModules(
                    ::GetCurrentProcess(),
                    modules.data(),
                    static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                    &requiredBytes))
            {
                return;
            }
        } while (requiredBytes > modules.size() * sizeof(HMODULE));

        modules.resize(requiredBytes / sizeof(HMODULE));
        for (HMODULE module : modules)
            patchModule(module);
    }

    // The loader notification fires whenever a DLL is mapped - after its
    // imports are resolved but before its initializers run - which is exactly
    // when the OpenGL driver, osgDB plugins, GDAL format drivers, and anything
    // else loaded after install() need their import tables patched.
    struct LdrDllLoadedNotificationData
    {
        ULONG flags;
        const void* fullDllName;
        const void* baseDllName;
        void* dllBase;
        ULONG sizeOfImage;
    };

    constexpr ULONG kLdrDllNotificationReasonLoaded = 1;

    using LdrDllNotificationFunction =
        VOID(CALLBACK*)(ULONG, const void*, void*);
    using LdrRegisterDllNotificationFunction =
        LONG(NTAPI*)(ULONG, LdrDllNotificationFunction, void*, void**);

    VOID CALLBACK dllLoadNotification(ULONG reason, const void* data, void*)
    {
        if (reason != kLdrDllNotificationReasonLoaded || !data)
            return;

        // The callback runs under the loader lock; keep it minimal and make
        // sure nothing it allocates gets tracked.
        ReentryGuard guard;

        auto* loaded = static_cast<const LdrDllLoadedNotificationData*>(data);
        patchModule(static_cast<HMODULE>(loaded->dllBase));
    }

    void installHeapHooks()
    {
        g_ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        g_kernel32Module = ::GetModuleHandleW(L"kernel32.dll");
        g_kernelBaseModule = ::GetModuleHandleW(L"kernelbase.dll");
        if (!g_kernel32Module || !g_ntdllModule)
            return;

        g_heapAlloc = reinterpret_cast<HeapAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "HeapAlloc"));
        g_heapReAlloc = reinterpret_cast<HeapReAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "HeapReAlloc"));
        g_heapFree = reinterpret_cast<HeapFreeFunction>(
            ::GetProcAddress(g_kernel32Module, "HeapFree"));
        g_rtlAllocateHeap = reinterpret_cast<RtlAllocateHeapFunction>(
            ::GetProcAddress(g_ntdllModule, "RtlAllocateHeap"));
        g_rtlReAllocateHeap = reinterpret_cast<RtlReAllocateHeapFunction>(
            ::GetProcAddress(g_ntdllModule, "RtlReAllocateHeap"));
        g_rtlFreeHeap = reinterpret_cast<RtlFreeHeapFunction>(
            ::GetProcAddress(g_ntdllModule, "RtlFreeHeap"));
        g_virtualAlloc = reinterpret_cast<VirtualAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "VirtualAlloc"));
        g_virtualFree = reinterpret_cast<VirtualFreeFunction>(
            ::GetProcAddress(g_kernel32Module, "VirtualFree"));
        g_globalAlloc = reinterpret_cast<GlobalAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "GlobalAlloc"));
        g_globalReAlloc = reinterpret_cast<GlobalReAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "GlobalReAlloc"));
        g_globalFree = reinterpret_cast<GlobalFreeFunction>(
            ::GetProcAddress(g_kernel32Module, "GlobalFree"));
        g_localAlloc = reinterpret_cast<LocalAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "LocalAlloc"));
        g_localReAlloc = reinterpret_cast<LocalReAllocFunction>(
            ::GetProcAddress(g_kernel32Module, "LocalReAlloc"));
        g_localFree = reinterpret_cast<LocalFreeFunction>(
            ::GetProcAddress(g_kernel32Module, "LocalFree"));

        // Resolved dynamically, not with &::GetProcAddress: taking the
        // address of a dllimport function reads this module's IAT slot,
        // which the patcher is about to point at the hook - the hook would
        // call itself. This call runs before any patching, so it returns
        // the real kernel32 export.
        g_getProcAddress = reinterpret_cast<GetProcAddressFunction>(
            ::GetProcAddress(g_kernel32Module, "GetProcAddress"));

        if (!g_heapAlloc || !g_heapReAlloc || !g_heapFree ||
            !g_rtlAllocateHeap || !g_rtlReAllocateHeap || !g_rtlFreeHeap ||
            !g_virtualAlloc || !g_virtualFree ||
            !g_globalAlloc || !g_globalReAlloc || !g_globalFree ||
            !g_localAlloc || !g_localReAlloc || !g_localFree ||
            !g_getProcAddress)
            return;

        g_hooksResolved = true;

        // Register for future module loads before patching what is already
        // here, so a module loaded between the two steps cannot slip through.
        auto ldrRegisterDllNotification =
            reinterpret_cast<LdrRegisterDllNotificationFunction>(
                ::GetProcAddress(g_ntdllModule, "LdrRegisterDllNotification"));

        if (ldrRegisterDllNotification)
        {
            static void* cookie = nullptr;
            ldrRegisterDllNotification(0, &dllLoadNotification, nullptr, &cookie);
        }

        patchAllLoadedModules();
    }

    struct ResolvedFrame
    {
        std::string file;
        std::string function;
        DWORD line = 0;
        std::uintptr_t address = 0;
        bool hasLine = false;
    };

    std::string lowerCase(std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });

        return value;
    }

    bool isLibrarySource(const ResolvedFrame& frame)
    {
        const std::string file = lowerCase(frame.file);
        const std::string function = lowerCase(frame.function);

        return
            file.find("\\vc\\tools\\msvc\\") != std::string::npos ||
            file.find("/vc/tools/msvc/") != std::string::npos ||
            file.find("\\windows kits\\") != std::string::npos ||
            file.find("/windows kits/") != std::string::npos ||
            file.find("heaphotspots.cpp") != std::string::npos ||
            function.find("operator new") != std::string::npos ||
            function.find("trackallocation") != std::string::npos;
    }

    ResolvedFrame resolveFrame(HANDLE symbolHandle, void* frameAddress)
    {
        ResolvedFrame result;
        result.address =
            reinterpret_cast<std::uintptr_t>(frameAddress);

        const DWORD64 address =
            static_cast<DWORD64>(result.address);

        alignas(SYMBOL_INFO)
        unsigned char symbolStorage[
            sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};

        SYMBOL_INFO* symbol =
            reinterpret_cast<SYMBOL_INFO*>(symbolStorage);

        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 symbolDisplacement = 0;

        if (::SymFromAddr(
                symbolHandle,
                address,
                &symbolDisplacement,
                symbol))
        {
            result.function.assign(
                symbol->Name,
                symbol->NameLen);
        }

        IMAGEHLP_LINE64 lineInfo = {};
        lineInfo.SizeOfStruct = sizeof(lineInfo);

        DWORD lineDisplacement = 0;

        if (::SymGetLineFromAddr64(
                symbolHandle,
                address,
                &lineDisplacement,
                &lineInfo))
        {
            if (lineInfo.FileName)
                result.file = lineInfo.FileName;

            result.line = lineInfo.LineNumber;
            result.hasLine = true;
        }

        return result;
    }

    ResolvedFrame findUsefulFrame(
        HANDLE symbolHandle,
        const StackTrace& trace,
        std::vector<ResolvedFrame>* callStack = nullptr)
    {
        ResolvedFrame fallback;
        ResolvedFrame useful;

        if (callStack)
            callStack->reserve(trace.frameCount);

        for (USHORT i = 0; i < trace.frameCount; ++i)
        {
            ResolvedFrame frame =
                resolveFrame(symbolHandle, trace.frames[i]);

            if (callStack)
                callStack->push_back(frame);

            if (fallback.address == 0 &&
                (!frame.function.empty() || frame.hasLine))
            {
                fallback = frame;
            }

            if (useful.address == 0 &&
                frame.hasLine && !isLibrarySource(frame))
            {
                useful = frame;
            }
        }

        return useful.address != 0 ? useful : fallback;
    }

    struct Aggregate
    {
        std::string file;
        std::string function;
        DWORD line = 0;

        std::uint64_t liveBytes = 0;
        std::uint64_t liveAllocations = 0;
        std::uint64_t largestAllocation = 0;
        std::vector<ResolvedFrame> callStack;
    };

    class SymbolSession
    {
    public:
        SymbolSession()
        {
            HANDLE currentProcess = ::GetCurrentProcess();

            if (!::DuplicateHandle(
                    currentProcess,
                    currentProcess,
                    currentProcess,
                    &handle_,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS))
            {
                return;
            }

            ::SymSetOptions(
                ::SymGetOptions() |
                SYMOPT_LOAD_LINES |
                SYMOPT_UNDNAME |
                SYMOPT_DEFERRED_LOADS);

            initialized_ =
                ::SymInitialize(handle_, nullptr, TRUE) != FALSE;
        }

        ~SymbolSession()
        {
            if (initialized_)
                ::SymCleanup(handle_);

            if (handle_)
                ::CloseHandle(handle_);
        }

        bool valid() const noexcept
        {
            return initialized_;
        }

        HANDLE handle() const noexcept
        {
            return handle_;
        }

    private:
        HANDLE handle_ = nullptr;
        bool initialized_ = false;
    };

    std::mutex& symbolMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    struct ProcessHeapTotals
    {
        std::uint64_t busyBytes = 0;
        std::uint64_t busyBlocks = 0;
        std::uint64_t overheadBytes = 0;
        std::uint64_t committedBytes = 0;
        std::uint64_t summaryAllocatedBytes = 0;
        std::uint64_t summaryCommittedBytes = 0;
        std::uint64_t summaryReservedBytes = 0;
        std::size_t heapCount = 0;
        bool complete = true;
        bool haveSummary = false;
    };

    // The SDK declares HEAP_SUMMARY in heapapi.h, but only under newer
    // _WIN32_WINNT values; declare our own and resolve the export at runtime.
    struct HeapSummaryData
    {
        DWORD cb;
        SIZE_T cbAllocated;
        SIZE_T cbCommitted;
        SIZE_T cbReserved;
        SIZE_T cbMaxReserve;
    };

    using HeapSummaryFunction = BOOL(WINAPI*)(HANDLE, DWORD, HeapSummaryData*);

    // Walks every heap in the process and totals what the heap manager itself
    // says is in use. This is the honest denominator for tracker coverage:
    // comparing tracked (requested) bytes against process private bytes mixes
    // in driver memory, thread stacks, and DLL data sections that no heap
    // tracker can ever see.
    //
    // Two sources are combined because each lies in a different way:
    //  - HeapWalk gives per-block busy totals, but its PROCESS_HEAP_REGION
    //    committed sizes cover only heap segments. Allocations >= ~508 KiB
    //    are "virtual blocks" committed outside any segment and are invisible
    //    to the region totals (verified experimentally: 400 x 1 MiB
    //    allocations moved region-committed by zero bytes).
    //  - HeapSummary reads the heap's own counters, which do include virtual
    //    blocks, but gives no per-block detail.
    ProcessHeapTotals measureProcessHeaps()
    {
        ProcessHeapTotals totals;

        static const HeapSummaryFunction heapSummary =
            reinterpret_cast<HeapSummaryFunction>(::GetProcAddress(
                ::GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));

        std::vector<HANDLE> heaps(64);
        for (;;)
        {
            DWORD count = ::GetProcessHeaps(
                static_cast<DWORD>(heaps.size()), heaps.data());

            if (count == 0)
            {
                totals.complete = false;
                return totals;
            }

            if (count <= heaps.size())
            {
                heaps.resize(count);
                break;
            }

            heaps.resize(count);
        }

        totals.heapCount = heaps.size();
        totals.haveSummary = heapSummary != nullptr && !heaps.empty();

        for (HANDLE heap : heaps)
        {
            if (heapSummary)
            {
                HeapSummaryData summary = {};
                summary.cb = sizeof(summary);

                if (heapSummary(heap, 0, &summary))
                {
                    totals.summaryAllocatedBytes += summary.cbAllocated;
                    totals.summaryCommittedBytes += summary.cbCommitted;
                    totals.summaryReservedBytes += summary.cbReserved;
                }
                else
                {
                    totals.haveSummary = false;
                }
            }

            if (!::HeapLock(heap))
            {
                totals.complete = false;
                continue;
            }

            // No allocations inside this loop: this thread holds the heap
            // lock, and although critical sections are reentrant, allocating
            // while walking corrupts the iteration state.
            PROCESS_HEAP_ENTRY entry = {};
            while (::HeapWalk(heap, &entry))
            {
                if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                {
                    totals.busyBytes += entry.cbData;
                    totals.overheadBytes += entry.cbOverhead;
                    ++totals.busyBlocks;
                }
                else if (entry.wFlags & PROCESS_HEAP_REGION)
                {
                    totals.committedBytes += entry.Region.dwCommittedSize;
                }
            }

            if (::GetLastError() != ERROR_NO_MORE_ITEMS)
                totals.complete = false;

            ::HeapUnlock(heap);
        }

        return totals;
    }
}

namespace HeapHotspots
{
    void install()
    {
        static std::once_flag once;
        std::call_once(once, []()
            {
                ReentryGuard guard;
                try
                {
                    installHeapHooks();
                }
                catch (...)
                {
                    // Heap diagnostics must never prevent the application
                    // from starting.
                }
            });
    }

    Report capture()
    {
        Report report;

        if (g_insideTracker)
        {
            report.error = "Heap capture cannot run recursively";
            return report;
        }

        try
        {
        ReentryGuard guard;

        // Catch any module that was loaded without a notification (or that
        // arrived if registration failed). Patching is idempotent, so this is
        // a cheap safety net.
        patchAllLoadedModules();

        // Take process measurements before creating any dump scratch storage.
        // The old implementation first copied every fat allocation record and
        // consequently counted hundreds of MiB of its own snapshot as
        // unexplained application heap.
        const ProcessHeapTotals heapTotals = measureProcessHeaps();
        const std::uint64_t trackerStorageBytes =
            g_trackerStorageBytes.load(std::memory_order_relaxed);
        const std::uint64_t trackerStorageBlocks =
            g_trackerStorageBlocks.load(std::memory_order_relaxed);

        PROCESS_MEMORY_COUNTERS_EX memoryCounters = {};
        DWORD memoryCountersSize = sizeof(memoryCounters);
        const bool haveMemoryUsage =
            ::GetProcessMemoryInfo(
                ::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounters),
                memoryCountersSize) != FALSE;
        const std::uint64_t processPrivateBytes =
            haveMemoryUsage ? static_cast<std::uint64_t>(memoryCounters.PrivateUsage) : 0;

        struct StackSnapshot
        {
            StackTrace trace;
            StackTotals totals;
        };

        std::vector<StackSnapshot> snapshot;

        {
            TrackerState& state = trackerState();
            std::lock_guard<std::mutex> lock(state.mutex);

            snapshot.reserve(state.stacks.size());

            for (auto& entry : state.stacks)
            {
                StackTotals& totals = entry.second;
                if (totals.heapCount == 0 && totals.virtualCount == 0)
                    continue;

                totals.dumpIndex = snapshot.size();
                snapshot.push_back({ entry.first, totals });
                snapshot.back().totals.largestAllocation = 0;
            }

            // The per-stack maximum is inexpensive to reconstruct here and
            // lets the hot path avoid a size multiset for every call stack.
            for (const auto& entry : state.live)
            {
                const Allocation& allocation = entry.second;
                StackTotals& totals = snapshot[allocation.totals->dumpIndex].totals;
                totals.largestAllocation = (std::max)(
                    totals.largestAllocation,
                    static_cast<std::uint64_t>(allocation.requestedSize));
            }
        }

        std::uint64_t trackedHeapBytes = 0;
        std::uint64_t trackedHeapCount = 0;
        std::uint64_t trackedVirtualBytes = 0;
        std::uint64_t trackedVirtualCount = 0;

        for (const StackSnapshot& stack : snapshot)
        {
            trackedHeapBytes += stack.totals.heapBytes;
            trackedHeapCount += stack.totals.heapCount;
            trackedVirtualBytes += stack.totals.virtualBytes;
            trackedVirtualCount += stack.totals.virtualCount;
        }

        /*
         * DbgHelp calls are serialized because the library is not safe for
         * concurrent use.
         */
        std::lock_guard<std::mutex> symbolsLock(symbolMutex());

        SymbolSession symbols;

        if (!symbols.valid())
        {
            report.error = "Unable to initialize DbgHelp symbol resolution";
            return report;
        }

        std::unordered_map<std::string, Aggregate> grouped;

        for (const StackSnapshot& stack : snapshot)
        {
            std::vector<ResolvedFrame> callStack;
            ResolvedFrame frame =
                findUsefulFrame(symbols.handle(), stack.trace, &callStack);

            std::string key;

            if (frame.hasLine)
            {
                key =
                    frame.file + ":" +
                    std::to_string(frame.line);
            }
            else if (!frame.function.empty())
            {
                key = frame.function;
            }
            else
            {
                key =
                    "0x" +
                    std::to_string(frame.address);
            }

            Aggregate& aggregate = grouped[key];

            if (aggregate.file.empty())
            {
                aggregate.file = std::move(frame.file);
                aggregate.function = std::move(frame.function);
                aggregate.line = frame.line;
                aggregate.callStack = std::move(callStack);
            }

            aggregate.liveBytes +=
                stack.totals.heapBytes + stack.totals.virtualBytes;
            aggregate.liveAllocations +=
                stack.totals.heapCount + stack.totals.virtualCount;

            aggregate.largestAllocation =
                (std::max)(
                    aggregate.largestAllocation,
                    stack.totals.largestAllocation);
        }

        std::vector<Aggregate> results;
        results.reserve(grouped.size());

        for (auto& entry : grouped)
            results.push_back(std::move(entry.second));

        std::sort(
            results.begin(),
            results.end(),
            [](const Aggregate& left, const Aggregate& right)
            {
                return left.liveBytes > right.liveBytes;
            });

        const std::uint64_t applicationHeapBytes =
            heapTotals.busyBytes > trackerStorageBytes
                ? heapTotals.busyBytes - trackerStorageBytes
                : 0;
        const std::uint64_t applicationHeapBlocks =
            heapTotals.busyBlocks > trackerStorageBlocks
                ? heapTotals.busyBlocks - trackerStorageBlocks
                : 0;
        const std::uint64_t untrackedApplicationHeapBytes =
            applicationHeapBytes > trackedHeapBytes
                ? applicationHeapBytes - trackedHeapBytes
                : 0;
        const double heapCoverage =
            applicationHeapBytes > 0
                ? 100.0 * static_cast<double>(trackedHeapBytes) /
                    static_cast<double>(applicationHeapBytes)
                : 0.0;

        // HeapSummary committed is the true heap footprint (segments plus
        // large virtual blocks). If HeapSummary is unavailable, busy+overhead
        // is a better floor than region-committed, which misses every
        // allocation >= ~508 KiB.
        const std::uint64_t heapCommittedBytes =
            heapTotals.haveSummary
                ? heapTotals.summaryCommittedBytes
                : (std::max)(
                    heapTotals.committedBytes,
                    heapTotals.busyBytes + heapTotals.overheadBytes);

        // This is a heap backing-layout detail, not another allocation API.
        // Large HeapAlloc requests may be represented here and still be part
        // of both busyBytes and trackedHeapBytes.
        const std::uint64_t largeBlockBytes =
            heapCommittedBytes > heapTotals.committedBytes
                ? heapCommittedBytes - heapTotals.committedBytes
                : 0;

        // Heap committed + tracked VirtualAlloc regions is the portion of
        // private memory this report can account for. The remainder is
        // memory nothing user-mode can attribute per call site: thread
        // stacks, DLL data sections, and driver allocations made through
        // unhookable paths.
        const std::uint64_t explainedBytes =
            heapCommittedBytes + trackedVirtualBytes;
        const double explainedPercentage =
            processPrivateBytes > 0
                ? 100.0 * static_cast<double>(explainedBytes) /
                    static_cast<double>(processPrivateBytes)
                : 0.0;

        report.valid = true;
        report.allocationThreshold = kMinimumTrackedSize;
        report.patchedModules = g_patchedModuleCount.load();
        report.distinctCallStacks = snapshot.size();
        report.trackedHeapBytes = trackedHeapBytes;
        report.trackedHeapAllocations = trackedHeapCount;
        report.trackedVirtualBytes = trackedVirtualBytes;
        report.trackedVirtualRegions = trackedVirtualCount;
        report.processHeapCount = heapTotals.heapCount;
        report.processHeapWalkComplete = heapTotals.complete;
        report.rawHeapBytes = heapTotals.busyBytes;
        report.rawHeapBlocks = heapTotals.busyBlocks;
        report.heapBlockOverheadBytes = heapTotals.overheadBytes;
        report.profilerBytes = trackerStorageBytes;
        report.profilerBlocks = trackerStorageBlocks;
        report.applicationHeapBytes = applicationHeapBytes;
        report.applicationHeapBlocks = applicationHeapBlocks;
        report.untrackedApplicationHeapBytes = untrackedApplicationHeapBytes;
        report.heapCoveragePercent = heapCoverage;
        report.heapCommittedBytes = heapCommittedBytes;
        report.heapCommittedIsEstimate = !heapTotals.haveSummary;
        report.heapSegmentBackingBytes = heapTotals.committedBytes;
        report.standaloneLargeBlockBackingBytes = largeBlockBytes;
        report.processPrivateBytesAvailable = haveMemoryUsage;
        report.processPrivateBytes = processPrivateBytes;
        report.knownMemoryCategoriesPercent = explainedPercentage;
        report.sites.reserve(results.size());

        for (Aggregate& result : results)
        {
            osgEarth::HeapHotspotSite site;
            site.file = std::move(result.file);
            site.function = std::move(result.function);
            site.line = result.line;
            site.liveBytes = result.liveBytes;
            site.liveAllocations = result.liveAllocations;
            site.largestAllocation = result.largestAllocation;
            site.callStack.reserve(result.callStack.size());
            for (ResolvedFrame& frame : result.callStack)
            {
                osgEarth::HeapHotspotFrame publicFrame;
                publicFrame.file = std::move(frame.file);
                publicFrame.function = std::move(frame.function);
                publicFrame.line = frame.line;
                publicFrame.address = frame.address;
                site.callStack.push_back(std::move(publicFrame));
            }
            report.sites.push_back(std::move(site));
        }

        return report;
        }
        catch (const std::exception& e)
        {
            report = Report();
            report.error = e.what();
            return report;
        }
        catch (...)
        {
            report = Report();
            report.error = "Unknown error while capturing heap snapshot";
            return report;
        }
    }

    bool write(
        const Report& report,
        const char* filename,
        std::size_t maxResults)
    {
        if (!report.valid || !filename)
            return false;

        FILE* output = nullptr;
        if (::fopen_s(&output, filename, "w") != 0 || !output)
            return false;

        const auto toMiB = [](std::uint64_t bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        };

        std::fprintf(
            output,
            "Allocation size threshold: %.2f KiB\n"
            "Patched modules: %u\n"
            "\n"
            "Tracked live allocations: %zu (%zu distinct call stacks)\n"
            "  heap:    %.2f MiB in %llu allocations\n"
            "  virtual: %.2f MiB in %llu regions\n"
            "\n"
            "Process heaps: %zu%s\n"
            "  raw in use: %.2f MiB in %llu blocks (+ %.2f MiB block overhead)\n"
            "  profiler bookkeeping: %.2f MiB requested in %llu heap blocks\n"
            "  application in use:   %.2f MiB in about %llu blocks\n"
            "  untracked application heap: %.2f MiB\n"
            "  committed: %.2f MiB%s\n"
            "    segment backing:              %.2f MiB\n"
            "    standalone large-block backing: %.2f MiB\n"
            "    (backing layout only; not a separate allocation source)\n"
            "Heap coverage (tracked / application heap in-use): %.1f%%\n",
            static_cast<double>(report.allocationThreshold) / 1024.0,
            report.patchedModules,
            static_cast<std::size_t>(report.trackedAllocations()),
            report.distinctCallStacks,
            toMiB(report.trackedHeapBytes),
            static_cast<unsigned long long>(report.trackedHeapAllocations),
            toMiB(report.trackedVirtualBytes),
            static_cast<unsigned long long>(report.trackedVirtualRegions),
            report.processHeapCount,
            report.processHeapWalkComplete ? "" : " (walk incomplete)",
            toMiB(report.rawHeapBytes),
            static_cast<unsigned long long>(report.rawHeapBlocks),
            toMiB(report.heapBlockOverheadBytes),
            toMiB(report.profilerBytes),
            static_cast<unsigned long long>(report.profilerBlocks),
            toMiB(report.applicationHeapBytes),
            static_cast<unsigned long long>(report.applicationHeapBlocks),
            toMiB(report.untrackedApplicationHeapBytes),
            toMiB(report.heapCommittedBytes),
            report.heapCommittedIsEstimate
                ? " (HeapSummary unavailable; estimated from busy blocks)"
                : "",
            toMiB(report.heapSegmentBackingBytes),
            toMiB(report.standaloneLargeBlockBackingBytes),
            report.heapCoveragePercent);

        if (report.processPrivateBytesAvailable)
        {
            std::fprintf(
                output,
                "\n"
                "Process private bytes: %.2f MiB\n"
                "Known memory categories ((heap committed + tracked virtual) / private): %.1f%%\n",
                toMiB(report.processPrivateBytes),
                report.knownMemoryCategoriesPercent);
        }
        else
        {
            std::fprintf(output, "\nProcess private bytes: unavailable\n");
        }

        std::fprintf(
            output,
            "\n"
            "Profiler bookkeeping is excluded from application heap coverage.\n"
            "Requested bytes are used for that subtraction; heap alignment and\n"
            "allocator metadata remain in the untracked figure.\n"
            "\n"
            "Not tracked by design: allocations made before install() (static\n"
            "initializers, CRT/loader startup) and heap calls made from inside\n"
            "ntdll/kernel32/kernelbase on behalf of other APIs.\n\n");

        const std::size_t resultCount =
            maxResults == 0
                ? report.sites.size()
                : (std::min)(maxResults, report.sites.size());

        for (std::size_t i = 0; i < resultCount; ++i)
        {
            const osgEarth::HeapHotspotSite& result = report.sites[i];

            std::fprintf(
                output,
                "%zu. %.2f MiB live in %llu allocations"
                " (largest %.2f MiB)\n",
                i + 1,
                static_cast<double>(result.liveBytes) /
                    (1024.0 * 1024.0),
                static_cast<unsigned long long>(
                    result.liveAllocations),
                static_cast<double>(result.largestAllocation) /
                    (1024.0 * 1024.0));

            if (!result.file.empty())
            {
                std::fprintf(
                    output,
                    "   %s:%lu\n",
                    result.file.c_str(),
                    static_cast<unsigned long>(result.line));
            }

            if (!result.function.empty())
            {
                std::fprintf(
                    output,
                    "   %s\n",
                    result.function.c_str());
            }

            if (!result.callStack.empty())
            {
                std::fprintf(output, "   Call stack:\n");
                const std::size_t frameCount =
                    (std::min)(result.callStack.size(), std::size_t(24));
                for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
                {
                    const osgEarth::HeapHotspotFrame& stackFrame = result.callStack[frameIndex];
                    std::fprintf(output, "      #%zu ", frameIndex);

                    if (!stackFrame.function.empty())
                        std::fprintf(output, "%s", stackFrame.function.c_str());
                    else
                        std::fprintf(output, "0x%llx", static_cast<unsigned long long>(stackFrame.address));

                    if (!stackFrame.file.empty())
                    {
                        std::fprintf(
                            output,
                            " (%s:%lu)",
                            stackFrame.file.c_str(),
                            static_cast<unsigned long>(stackFrame.line));
                    }

                    std::fprintf(output, "\n");
                }
            }

            std::fprintf(output, "\n");
        }

        std::fclose(output);
        return true;
    }

    void dump(const char* filename, std::size_t maxResults)
    {
        write(capture(), filename, maxResults);
    }
}

