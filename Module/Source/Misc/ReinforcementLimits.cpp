// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementLimits.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

namespace Kyber
{
namespace
{
constexpr size_t kMaxLineLength = 512;
constexpr const char kPrefix[] = "GCR1|";
constexpr auto kStaleAfter = std::chrono::seconds(90);

std::mutex s_mutex;
std::map<std::string, ReinforcementLimits::Entry> s_store;

// True when the entry was refreshed recently enough to trust.
bool IsFresh(const ReinforcementLimits::Entry& entry)
{
    if (entry.updatedAt.time_since_epoch().count() == 0)
    {
        return false;
    }
    return std::chrono::steady_clock::now() - entry.updatedAt <= kStaleAfter;
}

// Parses a non-negative integer made only of digits; false on empty/overflow/garbage.
bool ParseInt(const std::string& text, long long& out)
{
    if (text.empty() || text.size() > 9)
    {
        return false;
    }

    long long value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = value * 10 + (c - '0');
    }

    out = value;
    return true;
}

// Splits "<kit>:<cur>:<start>" from the right and validates it.
bool ParseEntry(const std::string& entry, ReinforcementLimits::Entry& out)
{
    size_t lastColon = entry.rfind(':');
    if (lastColon == std::string::npos || lastColon == 0)
    {
        return false;
    }

    size_t prevColon = entry.rfind(':', lastColon - 1);
    if (prevColon == std::string::npos || prevColon == 0)
    {
        return false;
    }

    long long cur = 0;
    long long start = 0;
    if (!ParseInt(entry.substr(prevColon + 1, lastColon - prevColon - 1), cur) || !ParseInt(entry.substr(lastColon + 1), start) ||
        start <= 0)
    {
        return false;
    }

    out.kit = entry.substr(0, prevColon);
    out.cur = static_cast<int>(cur);
    out.start = static_cast<int>(start);
    return true;
}
} // namespace

// Parses GCR1 admin chat and merges counts keyed by kit name.
bool ReinforcementLimits::ParseLine(const std::string& line)
{
    if (line.size() > kMaxLineLength || line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0)
    {
        return false;
    }

    std::vector<Entry> parsed;
    size_t pos = sizeof(kPrefix) - 1;
    while (pos <= line.size())
    {
        size_t end = line.find('|', pos);
        if (end == std::string::npos)
        {
            end = line.size();
        }

        Entry entry;
        if (end > pos && ParseEntry(line.substr(pos, end - pos), entry))
        {
            parsed.push_back(std::move(entry));
        }

        pos = end + 1;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s_mutex);
    for (Entry& entry : parsed)
    {
        entry.updatedAt = now;
        s_store[entry.kit] = entry;
    }
    return true;
}

// Drops every cached kit count (new online session).
void ReinforcementLimits::Clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_store.clear();
}

// True when no kit is stored (stale entries count as stored).
bool ReinforcementLimits::IsEmpty()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_store.empty();
}

// Returns fresh entries sorted by kit for overlay drawing.
std::vector<ReinforcementLimits::Entry> ReinforcementLimits::Snapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<Entry> out;
    out.reserve(s_store.size());
    for (const auto& [kit, entry] : s_store)
    {
        if (IsFresh(entry))
        {
            out.push_back(entry);
        }
    }
    std::sort(out.begin(), out.end(), [](const Entry& left, const Entry& right) { return left.kit < right.kit; });
    return out;
}

// True when the kit is tracked, fresh, and exhausted.
bool ReinforcementLimits::IsDepleted(const std::string& kit)
{
    Entry entry;
    return Find(kit, entry) && entry.cur == 0;
}

// Copies a fresh entry for one kit when it exists.
bool ReinforcementLimits::Find(const std::string& kit, Entry& out)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_store.find(kit);
    if (it == s_store.end() || !IsFresh(it->second))
    {
        return false;
    }

    out = it->second;
    return true;
}

// Returns the segment after the last slash in a kit path.
std::string ReinforcementLimits::ShortName(const std::string& kit)
{
    size_t slash = kit.rfind('/');
    return slash == std::string::npos ? kit : kit.substr(slash + 1);
}
} // namespace Kyber
