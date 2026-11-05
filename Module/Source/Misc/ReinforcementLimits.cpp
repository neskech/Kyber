// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementLimits.h>

#include <Base/Log.h>
#include <Misc/ReinforcementText.h>

#include <algorithm>
#include <cctype>
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
std::vector<std::string> s_order; // First-seen GCR1 kit order.

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
        start < 0)
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

    KYBER_LOG(Info, "[ReinforcementLimits] GCR1 line parsed, entries=" << parsed.size() << " first=" << (parsed.empty() ? std::string() : parsed.front().kit));
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (Entry& entry : parsed)
        {
            entry.updatedAt = now;
            if (s_store.find(entry.kit) == s_store.end())
            {
                s_order.push_back(entry.kit);
            }
            s_store[entry.kit] = entry;
        }
    }
    ReinforcementText::InvalidatePending();
    ReinforcementText::ResolvePending();
    return true;
}

// Drops every cached kit count (new online session).
void ReinforcementLimits::Clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_store.clear();
    s_order.clear();
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

// Returns fresh entries in the order the first GCR1 line named each kit.
std::vector<ReinforcementLimits::Entry> ReinforcementLimits::SnapshotOrdered()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<Entry> out;
    out.reserve(s_order.size());
    for (const std::string& kit : s_order)
    {
        auto it = s_store.find(kit);
        if (it != s_store.end() && IsFresh(it->second))
        {
            out.push_back(it->second);
        }
    }
    return out;
}

// Returns every stored entry regardless of age.
std::vector<ReinforcementLimits::Entry> ReinforcementLimits::SnapshotAll()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<Entry> out;
    out.reserve(s_store.size());
    for (const auto& [kit, entry] : s_store)
    {
        out.push_back(entry);
    }
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
    if (it != s_store.end() && IsFresh(it->second))
    {
        out = it->second;
        return true;
    }
    for (const auto& [stored, entry] : s_store)
    {
        if (IsFresh(entry) && Matches(stored, kit))
        {
            out = entry;
            return true;
        }
    }
    return false;
}

// Returns the segment after the last slash in a kit path.
std::string ReinforcementLimits::ShortName(const std::string& kit)
{
    size_t slash = kit.rfind('/');
    return slash == std::string::npos ? kit : kit.substr(slash + 1);
}

// True when two kit ids name the same asset (exact, trailing name, or suffix).
bool ReinforcementLimits::Matches(const std::string& left, const std::string& right)
{
    if (left.empty() || right.empty())
    {
        return false;
    }
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    if (lower(left) == lower(right))
    {
        return true;
    }
    const std::string leftName = lower(ShortName(left));
    const std::string rightName = lower(ShortName(right));
    if (leftName == rightName)
    {
        return true;
    }
    const auto endsWith = [](const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return leftName.size() >= 4 && rightName.size() >= 4 && (endsWith(leftName, rightName) || endsWith(rightName, leftName));
}
} // namespace Kyber
