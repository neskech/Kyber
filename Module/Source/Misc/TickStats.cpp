// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Diagnostic frame timing for the server and client pre-frame hooks.

#define _WINSOCKAPI_
#include <Misc/TickStats.h>

#include <Base/Log.h>

#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace Kyber
{
namespace TickStats
{
namespace
{
const char* const kSectionNames[Section_Count] = { "game", "entities", "listeners", "heartbeat", "executor", "events", "api", "lua", "generic" };

// One side's window: frame count, longest gap, and per-section totals.
struct Side
{
    uint64_t last = 0;
    uint64_t frames = 0;
    uint64_t maxGap = 0;
    std::atomic<uint64_t> sections[Section_Count] = {};
};

Side s_sides[2];
uint64_t s_windowStart = 0;
std::mutex s_mutex;

// Milliseconds in a QPC tick count.
double Ms(uint64_t ticks)
{
    static const double perMs = []() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(freq.QuadPart) / 1e3;
    }();
    return static_cast<double>(ticks) / perMs;
}

// One side's summary line fragment, resetting its window.
std::string Summary(const char* name, Side& side, double seconds)
{
    std::stringstream out;
    out << name << " fps=" << side.frames / seconds << " maxGapMs=" << Ms(side.maxGap);
    for (int i = 0; i < Section_Count; i++)
    {
        out << " " << kSectionNames[i] << "Ms=" << Ms(side.sections[i].exchange(0)) / (std::max)(side.frames, uint64_t{ 1 });
    }
    side.frames = 0;
    side.maxGap = 0;
    return out.str();
}

constexpr int kStackDepth = 24;
constexpr int kSamples = 40;
using Stack = std::vector<uint64_t>;

// Kernel plus user CPU time of one thread in 100 ns units, or 0.
uint64_t ThreadCpu(HANDLE thread)
{
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
    {
        return 0;
    }
    return (static_cast<uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) + (static_cast<uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}

// Ids of this process's threads, excluding the caller.
std::vector<DWORD> OwnThreads()
{
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return ids;
    }
    THREADENTRY32 entry{ sizeof(entry) };
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
        {
            ids.push_back(entry.th32ThreadID);
        }
    }
    CloseHandle(snapshot);
    return ids;
}

// Thread with the most CPU over `window`, with its CPU milliseconds.
std::pair<DWORD, double> HottestThread(std::chrono::milliseconds window)
{
    std::unordered_map<DWORD, std::pair<HANDLE, uint64_t>> before;
    for (DWORD id : OwnThreads())
    {
        if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id))
        {
            before[id] = { thread, ThreadCpu(thread) };
        }
    }
    std::this_thread::sleep_for(window);
    std::pair<DWORD, double> best{ 0, 0.0 };
    for (auto& [id, entry] : before)
    {
        const double ms = static_cast<double>(ThreadCpu(entry.first) - entry.second) / 1e4;
        if (ms > best.second)
        {
            best = { id, ms };
        }
        CloseHandle(entry.first);
    }
    return best;
}

// Captures one stack of a thread by suspending it; no allocation while it is suspended.
int CaptureStack(HANDLE thread, uint64_t (&frames)[kStackDepth])
{
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
    {
        return 0;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    int count = 0;
    if (GetThreadContext(thread, &context))
    {
        while (count < kStackDepth && context.Rip != 0)
        {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function == nullptr)
            {
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += sizeof(DWORD64);
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        }
    }
    ResumeThread(thread);
    return count;
}

// "module!symbol+0x.." or "module+0x.." for one address.
std::string Symbolize(uint64_t address)
{
    char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module))
    {
        GetModuleFileNameA(module, path, MAX_PATH);
    }
    const char* name = std::strrchr(path, '\\') != nullptr ? std::strrchr(path, '\\') + 1 : path;
    std::stringstream out;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
    {
        out << name << "!" << symbol->Name << "+0x" << std::hex << displacement;
    }
    else
    {
        out << name << "+0x" << std::hex << (address - reinterpret_cast<uint64_t>(module));
    }
    return out.str();
}

// Every 10 s: finds the hottest thread, samples its stack, and logs the most common stacks.
void SamplerLoop()
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    const bool symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    KYBER_LOG(Info, "[GCTick] sampler started symbols=" << symbols);
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        const auto [id, cpuMs] = HottestThread(std::chrono::milliseconds(2000));
        HANDLE thread = id != 0 ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id) : nullptr;
        if (thread == nullptr)
        {
            continue;
        }
        std::map<Stack, int> stacks;
        std::map<uint64_t, int> tops;
        for (int i = 0; i < kSamples; i++)
        {
            uint64_t frames[kStackDepth] = {};
            const int count = CaptureStack(thread, frames);
            if (count > 0)
            {
                stacks[Stack(frames, frames + count)]++;
                tops[frames[0]]++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CloseHandle(thread);
        std::vector<std::pair<int, Stack>> sorted;
        for (const auto& [stack, hits] : stacks)
        {
            sorted.push_back({ hits, stack });
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        KYBER_LOG(Info, "[GCTick] hot tid=" << id << " cpuMsPer2s=" << cpuMs << " distinctStacks=" << sorted.size());
        for (size_t i = 0; i < sorted.size() && i < 3; i++)
        {
            std::stringstream line;
            for (uint64_t frame : sorted[i].second)
            {
                line << " < " << Symbolize(frame);
            }
            KYBER_LOG(Info, "[GCTick] stack " << sorted[i].first << "/" << kSamples << line.str());
        }
        std::vector<std::pair<int, uint64_t>> topSorted;
        for (const auto& [frame, hits] : tops)
        {
            topSorted.push_back({ hits, frame });
        }
        std::sort(topSorted.begin(), topSorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::stringstream line;
        for (size_t i = 0; i < topSorted.size() && i < 6; i++)
        {
            line << " " << topSorted[i].first << "x " << Symbolize(topSorted[i].second);
        }
        KYBER_LOG(Info, "[GCTick] top frames" << line.str());
    }
}
} // namespace

bool Enabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_TICK_STATS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

uint64_t Now()
{
    if (!Enabled())
    {
        return 0;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

uint64_t Add(bool server, Section section, uint64_t start)
{
    if (!Enabled())
    {
        return 0;
    }
    const uint64_t now = Now();
    s_sides[server ? 0 : 1].sections[section].fetch_add(now - start, std::memory_order_relaxed);
    return now;
}

void Frame(bool server)
{
    if (!Enabled())
    {
        return;
    }
    static std::once_flag samplerOnce;
    std::call_once(samplerOnce, []() { std::thread(SamplerLoop).detach(); });
    std::lock_guard<std::mutex> lock(s_mutex);
    const uint64_t now = Now();
    Side& side = s_sides[server ? 0 : 1];
    if (side.last != 0)
    {
        side.frames++;
        side.maxGap = (std::max)(side.maxGap, now - side.last);
    }
    side.last = now;
    if (s_windowStart == 0)
    {
        s_windowStart = now;
        return;
    }
    if (Ms(now - s_windowStart) < 10000.0)
    {
        return;
    }
    const double seconds = Ms(now - s_windowStart) / 1e3;
    KYBER_LOG(Info, "[GCTick] " << Summary("server", s_sides[0], seconds) << " | " << Summary("client", s_sides[1], seconds));
    s_windowStart = now;
}
} // namespace TickStats
} // namespace Kyber
