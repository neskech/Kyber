// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementText.h>

#include <Base/Log.h>
#include <Utilities/StringUtils.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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
std::unordered_map<uintptr_t, std::string> s_bindings;   // Entity address -> kit id.

struct CostBus
{
    uintptr_t bus = 0;
    uintptr_t parentBus = 0;
    std::string kit;
};

std::unordered_map<uintptr_t, CostBus> s_costByEntity; // Cost entity address -> bus + kit.

struct PendingCost
{
    uintptr_t entity = 0;
    uintptr_t bus = 0;
    uintptr_t parentBus = 0;
    std::string token;
    std::string kit;
    int index = -1;
    bool cardLike = false;
};

struct PendingWidget
{
    uintptr_t entity = 0;
    uintptr_t bus = 0;
    uintptr_t parentBus = 0;
    std::string typeName;
    std::string kit;
    int index = -1;
};

struct PendingText
{
    uintptr_t entity = 0;
    uintptr_t bus = 0;
    uintptr_t parentBus = 0;
};

std::vector<PendingCost> s_pendingCosts;     // Cost rows in create order (never dropped for empty GCR1).
std::vector<PendingWidget> s_pendingWidgets; // Wrapped widgets in create order.
std::vector<PendingText> s_pendingTexts;     // TextElement rows for count writes.
bool s_associateDirty = true;                // Pending rows or GCR1 changed since the last AssociateLocked.
int s_nextCostIndex = 0;                     // Card-like cost sequence.
int s_nextWidgetIndex = 0;                   // List-item widget sequence.
int s_dumpSeq = 0;                           // [GCDump] create sequence.
int s_dumpSelectSeq = 0;                     // [GCDump] select sequence.
bool s_dumpSawCost = false;                  // True after the first Battlepoint* dump.
std::unordered_map<std::string, uintptr_t> s_tokenToWidget; // Normalized token -> widget entity.
std::recursive_mutex s_writerMutex;                      // Guards s_writers (writers may re-enter).
std::map<uintptr_t, std::shared_ptr<std::function<void()>>> s_writers; // Entity address -> label writer.
constexpr size_t kMaxLocalizedIds = 512;                 // Cap on stored synthetic ids (oldest evicted first).
std::map<std::string, std::string> s_localizedText;      // Synthetic id -> text.
std::map<std::string, char*> s_localizedArena;           // Synthetic id -> arena copy handed to the game.
std::deque<std::string> s_localizedOrder;                // Insertion order of synthetic ids, for eviction.
std::unordered_map<int32_t, std::string> s_locHashToKit; // Loc id hash -> GCR1 kit for widget bind.

struct DiscoveryPollRow
{
    std::string dataType;
    std::function<std::string()> snapshot;
};

std::mutex s_pollMutex;                                          // Guards the poll map and clock.
std::unordered_map<uintptr_t, DiscoveryPollRow> s_pollRows;      // Entity address -> snapshot callback.
std::chrono::steady_clock::time_point s_pollStart{};             // First row or spawn-screen open.
int s_pollTicks = 0;                                             // Seconds already logged.
bool s_pollStarted = false;                                      // Clock is running.
constexpr int kPollSeconds = 30;                                 // How long to dump CString fields.

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

bool ReinforcementText::LabelHooksEnabled()
{
    static const bool enabled = EnvString("GC_TEXT_HOOKS") == "1" || DiscoveryEnabled();
    return enabled;
}

const std::string& ReinforcementText::TextField()
{
    static const std::string field = []() {
        const std::string value = EnvString("GC_TEXT_FIELD");
        return value.empty() ? std::string("Text") : value;
    }();
    return field;
}

const std::string& ReinforcementText::SourceField()
{
    static const std::string field = []() {
        const std::string source = EnvString("GC_TEXT_SOURCE_FIELD");
        return source.empty() ? TextField() : source;
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
    return LabelHooksEnabled() && !TextField().empty();
}

bool ReinforcementText::HasWriters()
{
    std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
    return !s_writers.empty();
}

static std::string MatchTokenToKit(const std::string& token);
static bool IsNoiseToken(const std::string& token);
static bool EntityAlive(uintptr_t entity);
static void AssociateLocked();

void ReinforcementText::RegisterCostKit(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& kit)
{
    if (entityAddress == 0 || kit.empty())
    {
        return;
    }
    const std::string resolved = MatchTokenToKit(kit);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    s_costByEntity[entityAddress] = CostBus{ bus, parentBus, resolved.empty() ? kit : resolved };
    for (PendingCost& row : s_pendingCosts)
    {
        if (row.entity == entityAddress)
        {
            row.token = kit;
            row.bus = bus;
            row.parentBus = parentBus;
            if (!resolved.empty())
            {
                row.kit = resolved;
            }
            return;
        }
    }
    PendingCost row;
    row.entity = entityAddress;
    row.bus = bus;
    row.parentBus = parentBus;
    row.token = kit;
    row.kit = resolved;
    row.cardLike = !IsNoiseToken(kit) && kit.find("Kit_") == std::string::npos && kit.find("Gameplay/Kits") == std::string::npos;
    if (row.cardLike)
    {
        row.index = s_nextCostIndex++;
    }
    s_pendingCosts.push_back(row);
    KYBER_LOG(Info, "[GCBind] cost #" << row.index << " token=" << kit << " kit=" << resolved << " card=" << row.cardLike
                                      << std::hex << " entity=" << entityAddress << " bus=" << bus << " parent=" << parentBus
                                      << std::dec);
}

void ReinforcementText::UnregisterCostKit(uintptr_t entityAddress)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    s_costByEntity.erase(entityAddress);
    s_pendingCosts.erase(std::remove_if(s_pendingCosts.begin(), s_pendingCosts.end(),
                                        [entityAddress](const PendingCost& row) { return row.entity == entityAddress; }),
                         s_pendingCosts.end());
}

std::string ReinforcementText::KitForBus(uintptr_t bus)
{
    if (bus == 0)
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const auto& [entity, row] : s_costByEntity)
    {
        (void)entity;
        if (!row.kit.empty() && (row.bus == bus || (row.parentBus != 0 && row.parentBus == bus)))
        {
            return row.kit;
        }
    }
    return std::string();
}

// Alphanumeric uppercase form of a kit, settings id, or card label.
static std::string NormalizeId(const std::string& text)
{
    std::string raw = text;
    const size_t slash = raw.find_last_of("/\\");
    if (slash != std::string::npos)
    {
        raw = raw.substr(slash + 1);
    }
    std::string out;
    for (unsigned char c : raw)
    {
        if (std::isalnum(c))
        {
            out += static_cast<char>(std::toupper(c));
        }
    }
    const char* prefixes[] = { "KITSPECIAL", "KITHERO", "KITVEHICLE", "KIT" };
    for (const char* prefix : prefixes)
    {
        const size_t n = std::strlen(prefix);
        if (out.compare(0, n, prefix) == 0)
        {
            out = out.substr(n);
            break;
        }
    }
    return out;
}

// Drops Rebel/Imperial/Clone/etc. prefixes used on settings ids but not always on GCR1 short names.
static std::string StripFaction(const std::string& normalized)
{
    const char* prefixes[] = { "REBEL", "IMPERIAL", "EMPIRE", "CLONE", "FIRSTORDER", "RESISTENCE", "RESISTANCE", "REPUBLIC",
                               "SEPARATIST", "CIS" };
    for (const char* prefix : prefixes)
    {
        const size_t n = std::strlen(prefix);
        if (normalized.size() > n + 2 && normalized.compare(0, n, prefix) == 0)
        {
            return normalized.substr(n);
        }
    }
    return normalized;
}

// True when two identifiers name the same unit after path/Kit_/faction stripping.
static bool IdsMatch(const std::string& left, const std::string& right)
{
    const std::string a = NormalizeId(left);
    const std::string b = NormalizeId(right);
    if (a.size() < 3 || b.size() < 3)
    {
        return false;
    }
    if (a == b)
    {
        return true;
    }
    const std::string as = StripFaction(a);
    const std::string bs = StripFaction(b);
    return as == bs || a == bs || as == b;
}

// Compact lowercase letters/digits from a kit or card label.
static std::string CompactTokens(const std::string& text)
{
    std::string out = NormalizeId(text);
    for (char& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Settings ids that are game rules, not spawn cards.
static bool IsNoiseToken(const std::string& token)
{
    const std::string n = NormalizeId(token);
    const char* noise[] = { "TIMEOFDAY", "SQUADSPAWN", "SPAWNWAVE", "FLAGSPAWN", "GLOBALCAPTURE", "GLOBALOBJECTIVE",
                            "GLOBALBOMB", "INTERACTTIME", "DETONATETIME", "BOMBTIMER", "HACKINGINTERACT",
                            "PLANETARYMISSIONS", "MODE1", "MODE5", "MODE9", "MODEDEFEND", "SPACEBATTLE", "HEROESVERSUS",
                            "SQUADSPAWNENABLED", "SPAWNWAVEENABLED", "FLAGSPAWNENABLED" };
    for (const char* item : noise)
    {
        if (n.find(item) != std::string::npos)
        {
            return true;
        }
    }
    return n == "GLOBAL" || n == "BLAST";
}

// True when the entity pointer still looks like a live vtable object.
static bool EntityAlive(uintptr_t entity)
{
    if (entity == 0)
    {
        return false;
    }
    void* vptr = nullptr;
    return ReinforcementText::SafeReadMemory(reinterpret_cast<const void*>(entity), &vptr, sizeof(vptr)) && vptr != nullptr;
}

// GCR1 kit whose normalized id uniquely matches token (settings id or card label).
static std::string MatchTokenToKit(const std::string& token)
{
    if (NormalizeId(token).size() < 3)
    {
        return std::string();
    }
    std::string exact;
    std::string unique;
    int hits = 0;
    for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::Snapshot())
    {
        if (IdsMatch(token, entry.kit))
        {
            hits++;
            unique = entry.kit;
            if (NormalizeId(token) == NormalizeId(entry.kit) || StripFaction(NormalizeId(token)) == StripFaction(NormalizeId(entry.kit)))
            {
                exact = entry.kit;
            }
        }
    }
    if (!exact.empty())
    {
        return exact;
    }
    return hits == 1 ? unique : std::string();
}

bool ReinforcementText::TryBindByKitTokens(uintptr_t entityAddress, const std::string& currentText)
{
    const std::string kit = MatchTokenToKit(currentText);
    if (kit.empty())
    {
        return false;
    }
    BindKit(entityAddress, kit);
    return true;
}

void ReinforcementText::ClearUi()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_costByEntity.clear();
        s_bindings.clear();
        s_locHashToKit.clear();
        s_pendingCosts.clear();
        s_pendingWidgets.clear();
        s_pendingTexts.clear();
        s_tokenToWidget.clear();
        s_associateDirty = true;
        s_nextCostIndex = 0;
        s_nextWidgetIndex = 0;
        s_dumpSeq = 0;
        s_dumpSelectSeq = 0;
        s_dumpSawCost = false;
    }
    std::lock_guard<std::recursive_mutex> lock(s_writerMutex);
    s_writers.clear();
}

// True when this create dump is a cost, list item, or spawn-screen identifier row.
static bool DumpWorthKeeping(const std::string& typeName, const std::string& fields)
{
    if (typeName.find("Battlepoint") != std::string::npos)
    {
        return true;
    }
    if (typeName.find("FBUIListItem") != std::string::npos)
    {
        return true;
    }
    if (fields.find("ID_") != std::string::npos || fields.find("Kit_") != std::string::npos ||
        fields.find("Gameplay/") != std::string::npos)
    {
        return true;
    }
    return s_dumpSawCost;
}

int ReinforcementText::DumpCreate(const char* kind, uintptr_t entity, uintptr_t bus, uintptr_t parentBus,
                                  const std::string& typeName, const std::string& fields)
{
    if (kind == nullptr || entity == 0)
    {
        return -1;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    const bool select = std::strcmp(kind, "select") == 0;
    if (!select && !DumpWorthKeeping(typeName, fields))
    {
        return -1;
    }
    if (!select && typeName.find("Battlepoint") != std::string::npos)
    {
        s_dumpSawCost = true;
    }
    constexpr int kCreateCap = 400;
    constexpr int kSelectCap = 40;
    if (select)
    {
        if (s_dumpSelectSeq >= kSelectCap)
        {
            return -1;
        }
        const int seq = s_dumpSelectSeq++;
        KYBER_LOG(Info, "[GCDump] select #" << seq << " type=" << typeName << std::hex << " entity=" << entity << " bus=" << bus
                                            << " parent=" << parentBus << std::dec << fields);
        return seq;
    }
    if (s_dumpSeq >= kCreateCap)
    {
        return -1;
    }
    const int seq = s_dumpSeq++;
    KYBER_LOG(Info, "[GCDump] create #" << seq << " type=" << typeName << std::hex << " entity=" << entity << " bus=" << bus
                                        << " parent=" << parentBus << std::dec << fields);
    return seq;
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
    if (ReinforcementLimits::Find(kitShortName, out))
    {
        return true;
    }
    for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::Snapshot())
    {
        if (ReinforcementLimits::Matches(entry.kit, kitShortName))
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

// Pairs GCR1-resolved costs to list-item widgets by create index and shared/parent bus.
static void AssociateLocked()
{
    std::vector<PendingCost*> costs;
    for (PendingCost& row : s_pendingCosts)
    {
        if (!row.cardLike || row.index < 0 || !EntityAlive(row.entity))
        {
            continue;
        }
        if (row.kit.empty())
        {
            row.kit = MatchTokenToKit(row.token);
        }
        if (!row.kit.empty())
        {
            costs.push_back(&row);
            s_costByEntity[row.entity] = CostBus{ row.bus, row.parentBus, row.kit };
            s_bindings[row.entity] = row.kit;
        }
    }
    std::sort(costs.begin(), costs.end(), [](const PendingCost* a, const PendingCost* b) { return a->index < b->index; });

    std::unordered_map<uintptr_t, int> busCount;
    for (PendingCost* row : costs)
    {
        busCount[row->bus]++;
    }
    uintptr_t listBus = 0;
    int listN = 0;
    for (const auto& [bus, n] : busCount)
    {
        if (n > listN)
        {
            listBus = bus;
            listN = n;
        }
    }
    std::vector<PendingCost*> busCosts;
    for (PendingCost* row : costs)
    {
        if (row->bus == listBus)
        {
            busCosts.push_back(row);
        }
    }
    std::sort(busCosts.begin(), busCosts.end(), [](const PendingCost* a, const PendingCost* b) { return a->index < b->index; });
    const std::vector<ReinforcementLimits::Entry> gcr = ReinforcementLimits::SnapshotOrdered();
    const size_t nIdx = busCosts.size() < gcr.size() ? busCosts.size() : gcr.size();
    static int s_indexLogs = 0;
    if (s_indexLogs < 8)
    {
        s_indexLogs++;
        KYBER_LOG(Info, "[GCBind] index bus=" << std::hex << listBus << std::dec << " costs=" << busCosts.size()
                                              << " gcr=" << gcr.size() << " pair=" << nIdx);
    }
    for (size_t i = 0; i < nIdx; i++)
    {
        if (s_indexLogs < 40)
        {
            s_indexLogs++;
            KYBER_LOG(Info, "[GCBind] index #" << i << " seq=" << busCosts[i]->index << " token=" << busCosts[i]->token
                                               << " gcr=" << gcr[i].kit << " tokenKit=" << busCosts[i]->kit);
        }
    }

    std::vector<PendingWidget*> listItems;
    std::vector<PendingWidget*> all;
    for (PendingWidget& widget : s_pendingWidgets)
    {
        if (!EntityAlive(widget.entity))
        {
            continue;
        }
        all.push_back(&widget);
        if (widget.typeName.find("ListItem") != std::string::npos || widget.typeName.find("FBUI") != std::string::npos)
        {
            listItems.push_back(&widget);
        }
    }
    std::sort(listItems.begin(), listItems.end(), [](const PendingWidget* a, const PendingWidget* b) { return a->index < b->index; });

    static int s_orderLogs = 0;
    if (s_orderLogs < 8)
    {
        s_orderLogs++;
        KYBER_LOG(Info, "[GCBind] order costs=" << costs.size() << " listItems=" << listItems.size()
                                                << " widgets=" << all.size() << " pendingCosts=" << s_pendingCosts.size());
    }

    std::vector<const PendingText*> liveTexts;
    for (const PendingText& text : s_pendingTexts)
    {
        if (EntityAlive(text.entity))
        {
            liveTexts.push_back(&text);
        }
    }

    auto pairCostWidget = [&liveTexts](PendingCost& cost, PendingWidget& widget) {
        widget.kit = cost.kit;
        s_bindings[widget.entity] = cost.kit;
        s_tokenToWidget[NormalizeId(cost.token)] = widget.entity;
        for (const PendingText* textRow : liveTexts)
        {
            const PendingText& text = *textRow;
            const bool sameBus = text.bus == widget.bus || text.bus == widget.parentBus || text.parentBus == widget.bus;
            if (sameBus)
            {
                s_bindings[text.entity] = cost.kit;
            }
        }
        static int s_pairLogs = 0;
        if (s_pairLogs < 40)
        {
            s_pairLogs++;
            KYBER_LOG(Info, "[GCBind] pair cost#" << cost.index << " " << cost.token << " -> widget#" << widget.index
                                                  << std::hex << " widget=" << widget.entity << " wbus=" << widget.bus
                                                  << " costBus=" << cost.bus << " sameBus=" << (cost.bus == widget.bus)
                                                  << std::dec << " kit=" << cost.kit);
        }
    };

    const size_t n = costs.size() < listItems.size() ? costs.size() : listItems.size();
    if (n > 0 && (listItems.size() + 2 >= costs.size() || costs.size() + 2 >= listItems.size() || n >= 3))
    {
        for (size_t i = 0; i < n; i++)
        {
            pairCostWidget(*costs[i], *listItems[i]);
        }
    }

    uintptr_t costBus = costs.empty() ? 0 : costs.front()->bus;
    static int s_busLogs = 0;
    for (const PendingText& text : s_pendingTexts)
    {
        const bool onCostBus = costBus != 0 && (text.bus == costBus || text.parentBus == costBus);
        if (onCostBus && s_busLogs < 12)
        {
            s_busLogs++;
            KYBER_LOG(Info, "[GCBind] text-on-cost-bus entity=" << std::hex << text.entity << " bus=" << text.bus
                                                                << " parent=" << text.parentBus << " costBus=" << costBus
                                                                << std::dec);
        }
    }
}

void ReinforcementText::ResolvePending()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_associateDirty)
    {
        return;
    }
    s_associateDirty = false;
    AssociateLocked();
}

void ReinforcementText::InvalidatePending()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
}

void ReinforcementText::RegisterCardWidget(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& typeName)
{
    if (entityAddress == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    for (PendingWidget& widget : s_pendingWidgets)
    {
        if (widget.entity == entityAddress)
        {
            widget.bus = bus;
            widget.parentBus = parentBus;
            widget.typeName = typeName;
            return;
        }
    }
    PendingWidget widget;
    widget.entity = entityAddress;
    widget.bus = bus;
    widget.parentBus = parentBus;
    widget.typeName = typeName;
    widget.index = s_nextWidgetIndex++;
    s_pendingWidgets.push_back(widget);
    KYBER_LOG(Info, "[GCBind] widget #" << widget.index << " type=" << typeName << std::hex << " entity=" << entityAddress
                                        << " bus=" << bus << " parent=" << parentBus << std::dec);
}

void ReinforcementText::UnregisterCardWidget(uintptr_t entityAddress)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    s_pendingWidgets.erase(std::remove_if(s_pendingWidgets.begin(), s_pendingWidgets.end(),
                                          [entityAddress](const PendingWidget& row) { return row.entity == entityAddress; }),
                           s_pendingWidgets.end());
}

void ReinforcementText::RegisterTextSlot(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus)
{
    if (entityAddress == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    for (PendingText& text : s_pendingTexts)
    {
        if (text.entity == entityAddress)
        {
            text.bus = bus;
            text.parentBus = parentBus;
            return;
        }
    }
    s_pendingTexts.push_back(PendingText{ entityAddress, bus, parentBus });
}

void ReinforcementText::NoteTextSelection(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& text)
{
    if (entityAddress == 0 || text.empty() || text == "|")
    {
        return;
    }
    const std::string kit = MatchTokenToKit(text);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_associateDirty = true;
    if (!kit.empty())
    {
        s_bindings[entityAddress] = kit;
    }
    uintptr_t widgetEntity = 0;
    for (PendingWidget& widget : s_pendingWidgets)
    {
        if (widget.bus == bus || widget.bus == parentBus || widget.parentBus == bus)
        {
            if (!kit.empty())
            {
                widget.kit = kit;
                s_bindings[widget.entity] = kit;
            }
            widgetEntity = widget.entity;
        }
    }
    for (PendingCost& cost : s_pendingCosts)
    {
        if (cost.cardLike && (IdsMatch(cost.token, text) || (!kit.empty() && IdsMatch(cost.token, kit))))
        {
            if (cost.kit.empty())
            {
                cost.kit = kit;
            }
            if (widgetEntity != 0)
            {
                s_tokenToWidget[NormalizeId(cost.token)] = widgetEntity;
            }
        }
    }
    static int s_selLogs = 0;
    if (s_selLogs < 40)
    {
        s_selLogs++;
        KYBER_LOG(Info, "[GCBind] select text=\"" << text << "\" kit=" << kit << std::hex << " textEnt=" << entityAddress
                                                  << " bus=" << bus << " parent=" << parentBus << " widget=" << widgetEntity
                                                  << std::dec);
    }
}

void ReinforcementText::RefreshAll()
{
    ResolvePending();
    if (!WriterEnabled() && !HasWriters())
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
    std::lock_guard<std::mutex> lock(s_pollMutex);
    s_pollStarted = true;
    s_pollStart = std::chrono::steady_clock::now();
    s_pollTicks = 0;
}

// Starts the 30 s clock the first time a cost/text/widget row is created.
void StartPollClockLocked()
{
    if (!s_pollStarted)
    {
        s_pollStarted = true;
        s_pollStart = std::chrono::steady_clock::now();
        s_pollTicks = 0;
    }
}

void ReinforcementText::RegisterDiscoveryPoll(uintptr_t entityAddress, const std::string& dataType, std::function<std::string()> snapshot)
{
    if (!DiscoveryEnabled() || entityAddress == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_pollMutex);
    s_pollRows[entityAddress] = DiscoveryPollRow{ dataType, std::move(snapshot) };
    StartPollClockLocked();
}

void ReinforcementText::UnregisterDiscoveryPoll(uintptr_t entityAddress)
{
    std::lock_guard<std::mutex> lock(s_pollMutex);
    s_pollRows.erase(entityAddress);
}

void ReinforcementText::PollDiscovery()
{
    if (!DiscoveryEnabled())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_pollMutex);
    if (!s_pollStarted || s_pollTicks >= kPollSeconds)
    {
        return;
    }
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s_pollStart).count());
    if (elapsed <= s_pollTicks)
    {
        return;
    }
    s_pollTicks = elapsed > kPollSeconds ? kPollSeconds : elapsed;
    KYBER_LOG(Info, "[GCTextDiscovery] poll t=" << s_pollTicks << "s rows=" << s_pollRows.size());
    for (const auto& [addr, row] : s_pollRows)
    {
        std::string snap = "<none>";
        if (row.snapshot)
        {
            snap = row.snapshot();
        }
        KYBER_LOG(Info, "[GCTextDiscovery] poll entity=" << std::hex << addr << std::dec << " data=" << row.dataType << snap);
    }
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

const char* ReinforcementText::RewriteLocalized(const char* id, const char* result)
{
    if (!LabelHooksEnabled() || id == nullptr || id[0] == '\0' || result == nullptr || result[0] == '\0' || ReinforcementLimits::IsEmpty())
    {
        return nullptr;
    }
    const std::string compact = CompactTokens(result);
    if (compact.size() < 3)
    {
        return nullptr;
    }
    ReinforcementLimits::Entry best;
    size_t bestLen = 0;
    for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::Snapshot())
    {
        std::string lower = CompactTokens(ReinforcementLimits::ShortName(entry.kit));
        const char* prefixes[] = { "kit_special_", "kit_hero_", "kit_vehicle_", "kit_" };
        for (const char* prefix : prefixes)
        {
            const size_t n = std::strlen(prefix);
            if (lower.compare(0, n, prefix) == 0)
            {
                lower = lower.substr(n);
                break;
            }
        }
        if (lower.size() < 3)
        {
            continue;
        }
        const bool hit = compact.find(lower) != std::string::npos || lower.find(compact) != std::string::npos;
        if (hit && lower.size() > bestLen)
        {
            best = entry;
            bestLen = lower.size();
        }
    }
    if (bestLen == 0)
    {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_locHashToKit[HashStringId(id)] = best.kit;
    }
    const std::string text = FormatLabel(best, result);
    SetLocalizedText(id, text);
    static int s_locLogs = 0;
    if (s_locLogs < 40)
    {
        s_locLogs++;
        KYBER_LOG(Info, "[GCText] loc rewrite id=" << id << " kit=" << best.kit << " text=\"" << text << "\"");
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_localizedArena.find(id);
    return it != s_localizedArena.end() ? it->second : nullptr;
}

std::string ReinforcementText::KitForLocalizedHash(int32_t hash)
{
    if (hash == 0)
    {
        return std::string();
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_locHashToKit.find(hash);
    return it != s_locHashToKit.end() ? it->second : std::string();
}

const char* ReinforcementText::LookupLocalized(const char* id)
{
    if (!LabelHooksEnabled() || id == nullptr || id[0] == '\0')
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_localizedArena.find(id);
    return it != s_localizedArena.end() ? it->second : nullptr;
}
} // namespace Kyber
