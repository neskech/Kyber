// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Lock-free ring of recent entity-create type names, so a crash dump can name the last creates without logging each one.

#include <Misc/EntityCreateTrace.h>

#include <Base/Log.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Kyber
{
namespace EntityCreateTrace
{
namespace
{
constexpr uint32_t kRingSize = 64;

// Type names are static TypeInfo strings, so storing the pointer is enough.
std::atomic<const char*> s_ring[kRingSize] = {};
std::atomic<uint32_t> s_next{ 0 };

// True when GC_ENTITY_CREATE_LOG=1 asks for one log line per create.
bool LogEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_ENTITY_CREATE_LOG");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
} // namespace

void Record(const char* typeName)
{
    const uint32_t slot = s_next.fetch_add(1, std::memory_order_relaxed) % kRingSize;
    s_ring[slot].store(typeName, std::memory_order_relaxed);
    if (LogEnabled())
    {
        KYBER_LOG(Info, "[EntityCreate] " << (typeName != nullptr ? typeName : "?"));
    }
}

void LogRecent()
{
    const uint32_t next = s_next.load(std::memory_order_relaxed);
    const uint32_t count = next < kRingSize ? next : kRingSize;
    for (uint32_t i = 0; i < count; i++)
    {
        const char* name = s_ring[(next - count + i) % kRingSize].load(std::memory_order_relaxed);
        KYBER_LOG(Info, "[EntityCreate] recent " << (count - i) << ": " << (name != nullptr ? name : "?"));
    }
}
} // namespace EntityCreateTrace
} // namespace Kyber
