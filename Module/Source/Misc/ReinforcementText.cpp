// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementText.h>

#include <Base/Log.h>
#include <Utilities/StringUtils.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Kyber
{
namespace
{
constexpr int kMaxLogLinesPerSecond = 200;
constexpr int kMaxLocLinesPerOpen = 300;
constexpr int kSpawnWindowSeconds = 10;

std::mutex s_mutex;                                      // Guards the maps below.
std::unordered_map<uintptr_t, std::string> s_bindings;   // Entity address -> kit short name.
std::recursive_mutex s_writerMutex;                      // Guards s_writers (writers may re-enter).
std::map<uintptr_t, std::shared_ptr<std::function<void()>>> s_writers; // Entity address -> label writer.
constexpr size_t kMaxLocalizedIds = 512;                 // Cap on stored synthetic ids (oldest evicted first).
std::map<std::string, std::string> s_localizedText;      // Synthetic id -> text.
std::map<std::string, char*> s_localizedArena;           // Synthetic id -> arena copy handed to the game.
std::deque<std::string> s_localizedOrder;                // Insertion order of synthetic ids, for eviction.

// True when VirtualQuery protection flags allow reading.
bool ProtectionReadable(DWORD protect)
{
    if ((protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
    {
        return false;
    }
    switch (protect & 0xff)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

std::mutex s_logMutex;                                   // Guards the rate limiter.
std::chrono::steady_clock::time_point s_logWindowStart{};
int s_logWindowCount = 0;

std::atomic<int64_t> s_spawnOpenedTicks{ 0 };            // steady_clock ticks of the last UI screen open (0 = never).
std::atomic<int> s_locLines{ 0 };                        // Localization lines logged since the last open.

// Reads an env var as a string, empty when unset.
std::string EnvString(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// Parses GC_TEXT_BIND ("substring=kit,substring=kit") once.
const std::vector<std::pair<std::string, std::string>>& BindRules()
{
    static const std::vector<std::pair<std::string, std::string>> rules = []() {
        std::vector<std::pair<std::string, std::string>> out;
        const std::string raw = EnvString("GC_TEXT_BIND");
        size_t pos = 0;
        while (pos <= raw.size())
        {
            size_t comma = raw.find(',', pos);
            if (comma == std::string::npos)
            {
                comma = raw.size();
            }
            const std::string item = raw.substr(pos, comma - pos);
            const size_t eq = item.find('=');
            if (eq != std::string::npos && eq > 0 && eq + 1 < item.size())
            {
                out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
            }
            pos = comma + 1;
        }
        return out;
    }();
    return rules;
}
} // namespace

std::string ReinforcementText::FormatLabel(const ReinforcementLimits::Entry& entry, const std::string& cost)
{
    const std::string counts = std::to_string(entry.cur) + "/" + std::to_string(entry.start);
    if (cost.empty())
    {
        return std::string(kLeadingSpaces, ' ') + counts;
    }
    return cost + std::string(kCostGapSpaces, ' ') + counts;
}

int32_t ReinforcementText::HashStringId(const std::string& id)
{
    int32_t result = static_cast<int32_t>(0xFFFFFFFF);
    for (size_t i = 0; i < id.length(); i++)
    {
        result = static_cast<int32_t>(id[i]) + 33 * result;
    }
    return result;
}

bool ReinforcementText::DiscoveryEnabled()
{
    static const bool enabled = EnvString("GC_TEXT_DISCOVERY") == "1";
    return enabled;
}

const std::string& ReinforcementText::TextField()
{
    static const std::string field = EnvString("GC_TEXT_FIELD");
    return field;
}

const std::string& ReinforcementText::SourceField()
{
    static const std::string field = []() {
        const std::string source = EnvString("GC_TEXT_SOURCE_FIELD");
        return source.empty() ? EnvString("GC_TEXT_FIELD") : source;
    }();
    return field;
}

bool ReinforcementText::LocalizedIdMode()
{
    static const bool localized = EnvString("GC_TEXT_MODE") == "localizedId";
    return localized;
}

bool ReinforcementText::WriterEnabled()
{
    return !TextField().empty();
}

void ReinforcementText::BindKit(uintptr_t entityAddress, const std::string& kitShortName)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_bindings[entityAddress] = kitShortName;
}

std::string ReinforcementText::kitFor(uintptr_t entityAddress)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_bindings.find(entityAddress);
    return it != s_bindings.end() ? it->second : std::string();
}

bool ReinforcementText::TryBindBySubstring(uintptr_t entityAddress, const std::string& currentText)
{
    if (currentText.empty())
    {
        return false;
    }
    for (const auto& rule : BindRules())
    {
        if (currentText.find(rule.first) != std::string::npos)
        {
            BindKit(entityAddress, rule.second);
            return true;
        }
    }
    return false;
}

bool ReinforcementText::ResolveEntry(const std::string& kitShortName, ReinforcementLimits::Entry& out)
{
    for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::Snapshot())
    {
        if (ReinforcementLimits::ShortName(entry.kit) == kitShortName)
        {
            out = entry;
            return true;
        }
    }
    return false;
}

void ReinforcementText::RegisterWriter(uintptr_t entityAddress, std::function<void()> writer)
{
    std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
    s_writers[entityAddress] = std::make_shared<std::function<void()>>(std::move(writer));
}

void ReinforcementText::UnregisterWriter(uintptr_t entityAddress)
{
    {
        std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
        s_writers.erase(entityAddress);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_bindings.erase(entityAddress);
}

void ReinforcementText::RefreshAll()
{
    if (!WriterEnabled())
    {
        return;
    }
    // Copy the writers under the lock, then call them unlocked, re-checking each is still registered.
    std::vector<std::pair<uintptr_t, std::shared_ptr<std::function<void()>>>> snapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
        snapshot.assign(s_writers.begin(), s_writers.end());
    }
    for (const auto& item : snapshot)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
            auto it = s_writers.find(item.first);
            if (it == s_writers.end() || it->second != item.second)
            {
                continue;
            }
        }
        if (item.second != nullptr && *item.second)
        {
            (*item.second)();
        }
    }
}

bool ReinforcementText::DiscoveryLogAllowed()
{
    std::lock_guard<std::mutex> lock(s_logMutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - s_logWindowStart >= std::chrono::seconds(1))
    {
        s_logWindowStart = now;
        s_logWindowCount = 0;
    }
    return ++s_logWindowCount <= kMaxLogLinesPerSecond;
}

void ReinforcementText::NoteSpawnScreenOpened()
{
    if (!DiscoveryEnabled())
    {
        return;
    }
    s_spawnOpenedTicks = std::chrono::steady_clock::now().time_since_epoch().count();
    s_locLines = 0;
}

void ReinforcementText::LogLocalization(const char* id, const char* result)
{
    if (!DiscoveryEnabled())
    {
        return;
    }
    const int64_t opened = s_spawnOpenedTicks;
    if (opened == 0)
    {
        return;
    }
    const auto openedAt = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(opened));
    if (std::chrono::steady_clock::now() - openedAt > std::chrono::seconds(kSpawnWindowSeconds))
    {
        return;
    }
    if (s_locLines.fetch_add(1) >= kMaxLocLinesPerOpen)
    {
        return;
    }
    KYBER_LOG(Info, "[GCTextDiscovery] loc id=" << (id != nullptr ? id : "(null)") << " -> " << (result != nullptr ? result : "(null)"));
}

void ReinforcementText::SetLocalizedText(const std::string& id, const std::string& text)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_localizedText.find(id);
    if (it != s_localizedText.end() && it->second == text)
    {
        return;
    }
    if (it == s_localizedText.end())
    {
        s_localizedOrder.push_back(id);
        while (s_localizedOrder.size() > kMaxLocalizedIds)
        {
            s_localizedText.erase(s_localizedOrder.front());
            s_localizedArena.erase(s_localizedOrder.front());
            s_localizedOrder.pop_front();
        }
    }
    s_localizedText[id] = text;
    s_localizedArena[id] = StringUtils::CopyWithArena(text); // Only copies when the text changes.
}

bool ReinforcementText::SafeReadMemory(const void* ptr, void* out, size_t size)
{
    if (size == 0)
    {
        return true;
    }
    if (ptr == nullptr || out == nullptr)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(ptr, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || !ProtectionReadable(info.Protect))
    {
        return false;
    }
    const uint8_t* regionStart = static_cast<const uint8_t*>(info.BaseAddress);
    const uint8_t* regionEnd = regionStart + info.RegionSize;
    const uint8_t* start = static_cast<const uint8_t*>(ptr);
    if (start < regionStart || start >= regionEnd || size > static_cast<size_t>(regionEnd - start))
    {
        return false;
    }
    std::memcpy(out, ptr, size);
    return true;
}

void ReinforcementText::LogComponentCreated(uintptr_t entityAddress, const char* entityTypeName)
{
    if (!DiscoveryEnabled() || entityTypeName == nullptr)
    {
        return;
    }
    static const char* const kInteresting[] = { "TextElement", "UIText", "Battlepoint", "LocalizedString" };
    bool match = false;
    for (const char* needle : kInteresting)
    {
        if (std::strstr(entityTypeName, needle) != nullptr)
        {
            match = true;
            break;
        }
    }
    if (!match || !DiscoveryLogAllowed())
    {
        return;
    }
    KYBER_LOG(Info, "[GCTextDiscovery] component created entity=" << std::hex << entityAddress << std::dec << " entityType=" << entityTypeName
                                                                  << " (data type and fields unavailable for components)");
}

const char* ReinforcementText::LookupLocalized(const char* id)
{
    if (id == nullptr || !WriterEnabled() || !LocalizedIdMode())
    {
        return nullptr;
    }
    if (std::strncmp(id, kLocalizedIdPrefix, std::strlen(kLocalizedIdPrefix)) != 0)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_localizedArena.find(id);
    return it != s_localizedArena.end() ? it->second : nullptr;
}
} // namespace Kyber
