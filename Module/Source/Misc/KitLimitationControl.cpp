// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Native kit-limitation bridge: maps GCR1 kits to kit Identifiers and locks depleted kits through the game's KitLimitation entities.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Core/DebugHooks.h>
#include <Core/Program.h>
#include <Misc/KitLimitationControl.h>
#include <Misc/ReflectDump.h>
#include <Misc/ReinforcementLimits.h>
#include <Misc/ReinforcementText.h>
#include <Misc/UnlockCatalog.h>
#include <SDK/Funcs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kyber
{
namespace
{
using namespace Reflect;

// Empty stand-in so the wrapper has a typed data pointer.
class KitLimitationEntityData : public DataContainer
{};

constexpr int kMaxChangeLogs = 600;   // Property/event lines per process.
constexpr int32_t kBrokenPrice = 50000; // Native Price at/above this is treated as unset (EraLock maps).

std::recursive_mutex s_mutex;
std::atomic<int> s_changeLogs{ 0 };

// "0" turns the bridge off; anything else (or unset) leaves it on.
bool Enabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_KITLIM");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// One KitLimitation row: the kit and the hashed input properties that drive it.
struct LimitationSlot
{
    uint32_t kitId = 0;
    int32_t maxHash = 0;
    int32_t priceHash = 0;
    int32_t stageHash = 0;
    int group = 0;
};

// Kit path -> candidate KitLimitation ids cache, filled from GCR1 entries.
struct KitIdCache
{
    std::unordered_map<std::string, std::vector<uint32_t>> ids;
    std::unordered_map<std::string, int> misses;
};
KitIdCache s_kitIds;

// Identifier of each depleted kit (cur == 0).
std::unordered_map<uint32_t, std::string> s_depleted;
// GCR1 entry of every tracked kit, keyed by Identifier.
std::unordered_map<uint32_t, ReinforcementLimits::Entry> s_tracked;

// Resolves a kit asset path to the ids a KitLimitation row may use (kit and Gameplay Identifier); empty while not loaded.
const std::vector<uint32_t>& ResolveKitIds(const std::string& path)
{
    static const std::vector<uint32_t> kNone;
    auto hit = s_kitIds.ids.find(path);
    if (hit != s_kitIds.ids.end())
    {
        return hit->second;
    }
    int& misses = s_kitIds.misses[path];
    if (misses > 40)
    {
        return kNone;
    }
    DataContainer* kit = ResourceManagerLookupDataContainer(path.c_str());
    uint32_t kitId = 0;
    if (kit == nullptr || !ReadU32(kit, kit->getType(), "Identifier", kitId))
    {
        if (misses++ == 0)
        {
            KYBER_LOG(Info, "[GCKitLim] kit " << path << " not resolved (asset=" << (kit != nullptr ? TypeName(kit->getType()) : "null") << ")");
        }
        return kNone;
    }
    UnlockCatalog::Probe(kit, path);
    std::vector<uint32_t> ids{ kitId };
    uint32_t gameplayId = 0;
    DataContainer* gameplay = ReadPointerField(kit, kit->getType(), "Gameplay");
    if (gameplay != nullptr && ReadU32(gameplay, gameplay->getType(), "Identifier", gameplayId) && gameplayId != 0)
    {
        ids.push_back(gameplayId);
    }
    KYBER_LOG(Info, "[GCKitLim] kit " << path << " type=" << TypeName(kit->getType()) << " Identifier=0x" << std::hex << kitId
                                      << " Gameplay=" << (gameplay != nullptr ? TypeName(gameplay->getType()) : "null") << " GameplayIdentifier=0x"
                                      << gameplayId);
    return s_kitIds.ids[path] = std::move(ids);
}

// Rebuilds the depleted-kit set from the GCR1 store.
void RefreshDepletion()
{
    UnlockCatalog::ProbeFromEnv();
    std::unordered_map<uint32_t, std::string> depleted;
    std::unordered_map<uint32_t, ReinforcementLimits::Entry> tracked;
    for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::SnapshotAll())
    {
        for (uint32_t id : ResolveKitIds(entry.kit))
        {
            tracked[id] = entry;
            if (entry.cur <= 0)
            {
                depleted[id] = entry.kit;
            }
        }
    }
    s_depleted.swap(depleted);
    s_tracked.swap(tracked);
}

// "1" makes every GCR1-tracked kit cost 0 battle points (dev test for lock vs. can't-afford).
bool FreeKits()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_KITLIM_FREE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// GC_KITLIM_PRICE=count shows the remaining count in the card's price badge.
bool PriceIsCount()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_KITLIM_PRICE");
        return value != nullptr && std::strcmp(value, "count") == 0;
    }();
    return enabled;
}

// Lock mode from GC_KITLIM_MODE: "max", "stage", or "both" (default).
const std::string& LockMode()
{
    static const std::string mode = []() {
        const char* value = std::getenv("GC_KITLIM_MODE");
        return std::string(value != nullptr && *value != 0 ? value : "both");
    }();
    return mode;
}

// True when a lock writes MaxCount=0.
bool LockMax()
{
    return LockMode() != "stage";
}

// True when a lock writes EnabledAtStage=false.
bool LockStage()
{
    return LockMode() != "max";
}

// Stock battle-point cost used when native Price is broken (heroes / specials / vehicles).
int32_t StockPrice(int group)
{
    if (group == 0)
        return 4000;
    if (group == 1)
        return 1000;
    return 800;
}

// Stock MaxCount when the host map leaves an EraLock card at 0 (heroes / specials / vehicles).
int32_t StockMax(int group)
{
    if (group == 0)
        return 1;
    if (group == 1)
        return 6;
    return 4;
}

class GcKitLimitationEntity;
std::vector<GcKitLimitationEntity*> s_entities;

// Wraps one Client/ServerKitLimitationEntity: logs its table and overrides MaxCount for depleted kits.
class GcKitLimitationEntity : public KyberEntity<KitLimitationEntityData>
{
public:
    // Parses the limitation table, logs it, and registers for Apply.
    GcKitLimitationEntity(EntityManager* entityManager, NativeEntity* entity, KitLimitationEntityData* data)
        : KyberEntity<KitLimitationEntityData>(entity, data)
    {
        const TypeInfo* entityType = entity != nullptr ? entity->getType() : nullptr;
        m_entityType = TypeName(entityType);
        m_server = m_entityType.find("Server") != std::string::npos;
        ParseSlots();
        KYBER_LOG(Info, "[GCKitLim] created entity=" << std::hex << reinterpret_cast<uintptr_t>(entity) << std::dec << " type=" << m_entityType
                                                     << " slots=" << m_slots.size() << " data=" << DumpFields(reinterpret_cast<const uint8_t*>(data), data->getType(), 0));
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_entities.push_back(this);
    }

    // Unregisters before the native entity goes away.
    void OnDestroy() override
    {
        {
            std::lock_guard<std::recursive_mutex> lock(s_mutex);
            for (auto it = s_entities.begin(); it != s_entities.end(); ++it)
            {
                if (*it == this)
                {
                    s_entities.erase(it);
                    break;
                }
            }
        }
        KYBER_LOG(Info, "[GCKitLim] destroyed entity=" << std::hex << reinterpret_cast<uintptr_t>(m_nativeEntity) << std::dec << " type=" << m_entityType);
        KyberEntityBase::OnDestroy();
    }

    // Forwards, then logs which slot property changed and its value.
    void PropertyChanged(PropertyModification* modification) override
    {
        KyberEntityBase::PropertyChanged(modification);
        if (s_changeLogs.fetch_add(1) >= kMaxChangeLogs)
        {
            return;
        }
        int32_t value = 0;
        const bool readable = modification->value != nullptr && SafeRead(modification->value, &value, sizeof(value));
        KYBER_LOG(Info, "[GCKitLim] changed type=" << m_entityType << " hash=0x" << std::hex << modification->nameHash << std::dec << " label="
                                                   << LabelFor(static_cast<int32_t>(modification->nameHash)) << " valueType="
                                                   << TypeName(modification->valueType) << " int=" << (readable ? std::to_string(value) : "?"));
    }

    // Forwards, then logs the event id.
    void Event(EntityEvent* event) override
    {
        KyberEntityBase::Event(event);
        if (event != nullptr && s_changeLogs.fetch_add(1) < kMaxChangeLogs)
        {
            KYBER_LOG(Info, "[GCKitLim] event type=" << m_entityType << " id=0x" << std::hex << event->eventId);
        }
    }

    // True for ServerKitLimitationEntity.
    bool IsServer() const
    {
        return m_server;
    }

    // Writes MaxCount=0 on depleted kits, force-enables the rest, and restores a refill.
    void Apply(const std::unordered_map<uint32_t, std::string>& depleted)
    {
        if (!m_valuesLogged && std::chrono::steady_clock::now() - m_created > std::chrono::seconds(5))
        {
            m_valuesLogged = true;
            LogValues();
        }
        for (const LimitationSlot& slot : m_slots)
        {
            auto tracked = s_tracked.find(slot.kitId);
            if (tracked != s_tracked.end() && (FreeKits() || PriceIsCount()))
            {
                const int32_t want = PriceIsCount() ? std::max(0, tracked->second.cur) : 0;
                int32_t price = 0;
                if (ReadInt(slot.priceHash, price) && price != want)
                {
                    const bool ok = WriteInt(slot.priceHash, want);
                    KYBER_LOG(Info, "[GCKitLim] price type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " " << price << "->"
                                                             << want << " write=" << ok);
                }
            }
            const bool wantLock = depleted.count(slot.kitId) != 0;
            auto locked = m_locked.find(slot.kitId);
            const bool isLocked = locked != m_locked.end();
            if (wantLock && isLocked)
            {
                Reassert(slot, locked->second);
            }
            else if (wantLock)
            {
                Lock(slot, depleted.at(slot.kitId));
            }
            else if (isLocked)
            {
                Unlock(slot, locked->second);
                m_locked.erase(locked);
            }
            if (!wantLock)
            {
                EnsureAvailable(slot);
            }
        }
    }

private:
    // Original inputs of a locked slot, restored on unlock.
    struct LockedInputs
    {
        int32_t max = -1;    // -1 when unread.
        int stage = -1;      // 0/1, or -1 when unread.
    };

    // Saves the slot's inputs and writes the lock values for the active mode.
    void Lock(const LimitationSlot& slot, const std::string& kit)
    {
        LockedInputs saved;
        int32_t max = 0;
        bool stage = false;
        if (ReadInt(slot.maxHash, max))
            saved.max = max;
        if (ReadBoolInput(slot.stageHash, stage))
            saved.stage = stage ? 1 : 0;
        const bool okMax = !LockMax() || WriteInt(slot.maxHash, 0);
        const bool okStage = !LockStage() || WriteBoolInput(slot.stageHash, false);
        m_locked[slot.kitId] = saved;
        KYBER_LOG(Info, "[GCKitLim] lock type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " (" << kit << ") max " << saved.max
                                                << "->0 write=" << okMax << " stage " << saved.stage << "->0 write=" << okStage);
    }

    // Writes the lock values again when the source entity overwrote them, keeping the source's latest value for Unlock.
    void Reassert(const LimitationSlot& slot, LockedInputs& saved)
    {
        int32_t max = 0;
        bool stage = false;
        if (LockMax() && ReadInt(slot.maxHash, max) && max != 0)
        {
            saved.max = max;
            KYBER_LOG(Info, "[GCKitLim] relock max type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " was=" << max
                                                          << " write=" << WriteInt(slot.maxHash, 0));
        }
        if (LockStage() && ReadBoolInput(slot.stageHash, stage) && stage)
        {
            saved.stage = 1;
            KYBER_LOG(Info, "[GCKitLim] relock stage type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec
                                                            << " write=" << WriteBoolInput(slot.stageHash, false));
        }
    }

    // Restores the inputs saved by Lock.
    void Unlock(const LimitationSlot& slot, const LockedInputs& saved)
    {
        const bool okMax = !LockMax() || (saved.max >= 0 && WriteInt(slot.maxHash, saved.max));
        const bool okStage = !LockStage() || (saved.stage >= 0 && WriteBoolInput(slot.stageHash, saved.stage == 1));
        KYBER_LOG(Info, "[GCKitLim] unlock type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " max=" << saved.max
                                                  << " write=" << okMax << " stage=" << saved.stage << " write=" << okStage);
    }

    // Turns on EraLock cards the host map still treats as the wrong era (stage, MaxCount, garbage Price).
    void EnsureAvailable(const LimitationSlot& slot)
    {
        if (LockStage())
        {
            bool stage = false;
            if (ReadBoolInput(slot.stageHash, stage) && !stage)
            {
                KYBER_LOG(Info, "[GCKitLim] enable type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec
                                                          << " write=" << WriteBoolInput(slot.stageHash, true));
            }
        }
        if (LockMax())
        {
            int32_t max = 0;
            if (ReadInt(slot.maxHash, max) && max <= 0)
            {
                int32_t want = StockMax(slot.group);
                auto tracked = s_tracked.find(slot.kitId);
                if (tracked != s_tracked.end())
                    want = (std::max)(1, tracked->second.start);
                KYBER_LOG(Info, "[GCKitLim] max type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " 0->" << want
                                                       << " write=" << WriteInt(slot.maxHash, want));
            }
        }
        if (FreeKits() || PriceIsCount())
        {
            return;
        }
        int32_t price = 0;
        if (!ReadInt(slot.priceHash, price) || price < kBrokenPrice)
        {
            return;
        }
        const int32_t want = StockPrice(slot.group);
        KYBER_LOG(Info, "[GCKitLim] clampPrice type=" << m_entityType << " kit=0x" << std::hex << slot.kitId << std::dec << " " << price << "->"
                                                      << want << " write=" << WriteInt(slot.priceHash, want));
    }

public:

private:
    // Reads LimitationGroups[].Limitations[] into m_slots.
    void ParseSlots()
    {
        const TypeInfo* type = m_data->getType();
        const FieldInfoData* groupsField = FindField(type, "LimitationGroups");
        if (groupsField == nullptr)
        {
            return;
        }
        const TypeInfo* groupType = reinterpret_cast<const ArrayTypeInfoData*>(groupsField->fieldTypePtr->typeInfoData)->elementType;
        const uint8_t* groups = nullptr;
        uint32_t groupCount = 0;
        if (groupType == nullptr || !ReadArray(reinterpret_cast<const uint8_t*>(m_data) + groupsField->fieldOffset, groups, groupCount))
        {
            return;
        }
        const FieldInfoData* limitsField = FindField(groupType, "Limitations");
        if (limitsField == nullptr)
        {
            return;
        }
        const TypeInfo* limitType = reinterpret_cast<const ArrayTypeInfoData*>(limitsField->fieldTypePtr->typeInfoData)->elementType;
        for (uint32_t g = 0; g < groupCount; g++)
        {
            const uint8_t* group = groups + g * groupType->typeInfoData->totalSize;
            uint32_t groupMax = 0;
            if (ReadU32(group, groupType, "MaxCountProperty", groupMax))
            {
                m_labels[static_cast<int32_t>(groupMax)] = "group" + std::to_string(g) + ".MaxCount";
            }
            const uint8_t* limits = nullptr;
            uint32_t limitCount = 0;
            if (limitType == nullptr || !ReadArray(group + limitsField->fieldOffset, limits, limitCount))
            {
                continue;
            }
            for (uint32_t i = 0; i < limitCount; i++)
            {
                const uint8_t* row = limits + i * limitType->typeInfoData->totalSize;
                LimitationSlot slot;
                slot.group = static_cast<int>(g);
                uint32_t v = 0;
                if (ReadU32(row, limitType, "KitId", v))
                    slot.kitId = v;
                if (ReadU32(row, limitType, "MaxCountProperty", v))
                    slot.maxHash = static_cast<int32_t>(v);
                if (ReadU32(row, limitType, "PriceProperty", v))
                    slot.priceHash = static_cast<int32_t>(v);
                if (ReadU32(row, limitType, "EnabledAtStageProperty", v))
                    slot.stageHash = static_cast<int32_t>(v);
                std::ostringstream id;
                id << "kit0x" << std::hex << slot.kitId;
                m_labels[slot.maxHash] = id.str() + ".MaxCount";
                m_labels[slot.priceHash] = id.str() + ".Price";
                m_labels[slot.stageHash] = id.str() + ".EnabledAtStage";
                m_slots.push_back(slot);
            }
        }
    }

    // Name of a known slot hash, or "?".
    std::string LabelFor(int32_t hash) const
    {
        auto it = m_labels.find(hash);
        return it != m_labels.end() ? it->second : std::string("?");
    }

    // Reads the current int value bound to a property hash on this entity.
    bool ReadInt(int32_t hash, int32_t& out) const
    {
        DataContext dc;
        DataContext_ctor(m_nativeEntity->m_entityBus, &dc);
        PropertyReaderBase reader;
        PropertyReaderBase_set(&reader, &dc, m_data, hash, nullptr, nullptr);
        if (reader.m_cache == nullptr)
        {
            return false;
        }
        void* value = nullptr;
        return SafeRead(&reader.m_cache->value, &value, sizeof(value)) && value != nullptr && SafeRead(value, &out, sizeof(out));
    }

    // Cache feeding an input property hash on this entity, or nullptr when unconnected.
    CacheData* InputCache(int32_t hash) const
    {
        DataContext dc;
        DataContext_ctor(m_nativeEntity->m_entityBus, &dc);
        PropertyReaderBase reader;
        PropertyReaderBase_set(&reader, &dc, m_data, hash, nullptr, nullptr);
        return reader.m_cache;
    }

    // Reads the current Boolean value bound to an input property hash.
    bool ReadBoolInput(int32_t hash, bool& out) const
    {
        CacheData* cache = InputCache(hash);
        void* value = nullptr;
        uint8_t byte = 0;
        if (cache == nullptr || !SafeRead(&cache->value, &value, sizeof(value)) || value == nullptr || !SafeRead(value, &byte, 1))
        {
            return false;
        }
        out = byte != 0;
        return true;
    }

    // Writes a Boolean into the cache feeding an input property hash and fires its listeners.
    bool WriteBoolInput(int32_t hash, bool value)
    {
        CacheData* cache = InputCache(hash);
        void* packed = nullptr;
        if (cache == nullptr || !SafeRead(&cache->value, &packed, sizeof(packed)) || packed == nullptr)
        {
            return false;
        }
        PropertyWriterBase writer;
        writer.m_cache = cache;
        PropertyWriterBase_set(&writer, &value, true);
        bool check = !value;
        return ReadBoolInput(hash, check) && check == value;
    }

    // Writes an Int32 into the cache feeding an input property hash and fires its listeners.
    bool WriteInt(int32_t hash, int32_t value)
    {
        CacheData* cache = InputCache(hash);
        void* packed = nullptr;
        if (cache == nullptr || !SafeRead(&cache->value, &packed, sizeof(packed)) || packed == nullptr)
        {
            return false;
        }
        PropertyWriterBase writer;
        writer.m_cache = cache;
        PropertyWriterBase_set(&writer, &value, true);
        int32_t check = -1;
        return ReadInt(hash, check) && check == value;
    }

    // Logs the live MaxCount/Price/EnabledAtStage value of every slot.
    void LogValues() const
    {
        std::ostringstream out;
        for (const LimitationSlot& slot : m_slots)
        {
            int32_t max = 0;
            int32_t price = 0;
            bool stage = false;
            const bool hm = ReadInt(slot.maxHash, max);
            const bool hp = ReadInt(slot.priceHash, price);
            const bool hs = ReadBoolInput(slot.stageHash, stage);
            out << " [g" << slot.group << " kit=0x" << std::hex << slot.kitId << " cache=0x" << reinterpret_cast<uintptr_t>(InputCache(slot.maxHash))
                << std::dec << " max=" << (hm ? std::to_string(max) : "?")
                << " price=" << (hp ? std::to_string(price) : "?") << " stage=" << (hs ? std::to_string(stage) : "?") << "]";
        }
        KYBER_LOG(Info, "[GCKitLim] values type=" << m_entityType << out.str());
    }

    std::string m_entityType;
    bool m_server = false;
    bool m_valuesLogged = false;
    std::chrono::steady_clock::time_point m_created = std::chrono::steady_clock::now();
    std::vector<LimitationSlot> m_slots;
    std::unordered_map<int32_t, std::string> m_labels;
    std::unordered_map<uint32_t, LockedInputs> m_locked; // kitId -> inputs before the lock.
};

// Runs Apply for the entities of one realm on that realm's thread.
class KitLimitationUpdater : public GenericUpdateListener
{
public:
    explicit KitLimitationUpdater(bool server)
        : m_server(server)
    {}

    // Refreshes depletion and applies it to this realm's limitation entities.
    void Update(UpdateType type, const UpdateParameters& params) override
    {
        (void)type;
        (void)params;
        if (!Enabled())
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastApply < std::chrono::milliseconds(250))
        {
            return;
        }
        m_lastApply = now;
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        if (s_entities.empty())
        {
            return;
        }
        RefreshDepletion();
        for (GcKitLimitationEntity* entity : s_entities)
        {
            if (entity->IsServer() == m_server)
            {
                entity->Apply(s_depleted);
            }
        }
    }

private:
    bool m_server;
    std::chrono::steady_clock::time_point m_lastApply{};
};

KitLimitationUpdater s_clientUpdater(false);
KitLimitationUpdater s_serverUpdater(true);
KB_REGISTER_GENERIC_UPDATE_LISTENER(s_clientUpdater, UpdateType_Client_PostFrame);
KB_REGISTER_GENERIC_UPDATE_LISTENER(s_serverUpdater, UpdateType_Server_PreFrame);

// Creator matching EntityStaticRegistrar's signature.
KyberEntityBase* CreateKitLimitation(EntityManager* entityManager, NativeEntity* entity, DataContainer* data)
{
    return new GcKitLimitationEntity(entityManager, entity, reinterpret_cast<KitLimitationEntityData*>(data));
}

// Registers the wrapper unless GC_KITLIM=0.
bool RegisterKitLimitation()
{
    if (!Enabled())
    {
        return false;
    }
    static EntityStaticRegistrar registrar("KitLimitationEntityData", CreateKitLimitation, false);
    return true;
}

[[maybe_unused]] const bool s_kitLimitationRegistered = RegisterKitLimitation();
} // namespace

bool KitLimitationControl::CountFor(uint32_t kitId, int& cur, int& start)
{
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    auto it = s_tracked.find(kitId);
    if (it == s_tracked.end())
    {
        return false;
    }
    cur = it->second.cur;
    start = it->second.start;
    return true;
}
} // namespace Kyber
