// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Per-team star card levels through a hook on the game's ability-level provider.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Hook/HookManager.h>
#include <Misc/ReflectDump.h>
#include <Misc/StarCardLevels.h>
#include <SDK/Funcs.h>
#include <SDK/SDK.h>

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace Kyber
{
namespace StarCardLevels
{
namespace
{
using namespace Reflect;

// Global ability-level provider; vtable slot 1 returns a 0-based PlayerAbilityLevel for (player, ability asset, flag).
constexpr uint64_t kLevelProviderGlobal = 0x143D9AD20;
constexpr const char* kCatalogPath = "Persistence/Catalog/Catalog";
constexpr const char* kLinePrefix = "GCS1|";
constexpr int kMaxLevel = 3;
constexpr uint64_t kCatalogRetryMs = 2000;
constexpr int kOverrideLogBudget = 8;

using GuidKey = std::pair<uint64_t, uint64_t>;

std::atomic<int> s_team1Level{ -1 };
std::atomic<int> s_team2Level{ -1 };
std::atomic<int> s_logBudget{ 0 };
std::atomic<bool> s_hooked{ false };
std::atomic<bool> s_catalogReady{ false };
std::mutex s_catalogMutex;
std::set<GuidKey> s_starCardUnlocks;
uint64_t s_catalogRetryAt = 0;

// TypeInfo stored in an object's header, or nullptr.
const TypeInfo* ObjectType(const void* object)
{
    const TypeInfo* type = nullptr;
    if (object == nullptr || !SafeRead(static_cast<const uint8_t*>(object) + sizeof(void*), &type, sizeof(type)))
    {
        return nullptr;
    }
    return ReadableType(type) ? type : nullptr;
}

// True for Catalog aliases of star cards: trooper, hero, reinforcement, and vehicle cards all start with "A_".
bool IsStarCardAlias(const char* alias)
{
    return alias != nullptr && alias[0] == 'A' && alias[1] == '_';
}

// Adds the star card unlock guids of one Catalog array field; returns how many entries it held.
uint32_t LoadCatalogArray(const uint8_t* catalog, const FieldInfoData* field, uint32_t& added)
{
    const TypeInfo* arrayType = field->fieldTypePtr;
    if (arrayType == nullptr || arrayType->typeInfoData == nullptr || arrayType->getBasicType() != kTypeCode_Array)
    {
        return 0;
    }
    const TypeInfo* element = reinterpret_cast<const ArrayTypeInfoData*>(arrayType->typeInfoData)->elementType;
    const uint8_t* data = nullptr;
    uint32_t count = 0;
    if (element == nullptr || element->typeInfoData == nullptr || !ReadArray(catalog + field->fieldOffset, data, count))
    {
        return 0;
    }
    const bool isClass = element->getBasicType() == kTypeCode_Class;
    if (!isClass && element->getBasicType() != kTypeCode_ValueType)
    {
        return 0;
    }
    const size_t stride = isClass ? sizeof(void*) : element->typeInfoData->totalSize;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* entry = data + i * stride;
        if (isClass && !SafeRead(data + i * stride, &entry, sizeof(entry)))
        {
            continue;
        }
        const TypeInfo* entryType = isClass ? ObjectType(entry) : element;
        const FieldInfoData* guidField = entryType != nullptr ? FindField(entryType, "AssetGuid") : nullptr;
        const FieldInfoData* aliasField = entryType != nullptr ? FindField(entryType, "ItemDefinitionAlias") : nullptr;
        if (guidField == nullptr || aliasField == nullptr)
        {
            return 0;
        }
        const char* alias = nullptr;
        GuidKey guid{};
        if (SafeRead(entry + aliasField->fieldOffset, &alias, sizeof(alias)) && IsStarCardAlias(alias) &&
            SafeRead(entry + guidField->fieldOffset, &guid, sizeof(guid)) && s_starCardUnlocks.insert(guid).second)
        {
            added++;
        }
    }
    return count;
}

// Loads star card unlock guids from every Catalog array of { AssetGuid, ItemDefinitionAlias } entries; caller holds the mutex.
bool LoadCatalog()
{
    s_starCardUnlocks.clear();
    const DataContainer* catalog = ResourceManagerLookupDataContainer(kCatalogPath);
    const TypeInfo* type = ObjectType(catalog);
    if (type == nullptr)
    {
        return false;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(catalog);
    for (const FieldInfoData* field : AllFields(type))
    {
        uint32_t added = 0;
        const uint32_t entries = LoadCatalogArray(base, field, added);
        if (entries > 0)
        {
            KYBER_LOG(Info, "[GCStar] catalog " << (field->name != nullptr ? field->name : "?") << " entries=" << entries << " starCardUnlocks=" << added);
        }
    }
    return !s_starCardUnlocks.empty();
}

// Loads the Catalog once from Poll; the hook only reads s_catalogReady and never waits on this.
void EnsureCatalog()
{
    if (s_catalogReady.load(std::memory_order_acquire))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_catalogMutex);
    if (s_catalogReady.load(std::memory_order_relaxed))
    {
        return;
    }
    const uint64_t now = GetTickCount64();
    if (now < s_catalogRetryAt)
    {
        return;
    }
    if (LoadCatalog())
    {
        s_catalogReady.store(true, std::memory_order_release);
        return;
    }
    s_catalogRetryAt = now + kCatalogRetryMs;
    KYBER_LOG(Warning, "[GCStar] catalog not resolved yet");
}

// Unlock field offset for one ability type; cached per thread because this runs on the tick.
int32_t UnlockOffset(const TypeInfo* type)
{
    static thread_local const TypeInfo* cachedType = nullptr;
    static thread_local int32_t cachedOffset = -1;
    if (type == cachedType)
    {
        return cachedOffset;
    }
    const FieldInfoData* field = type != nullptr ? FindField(type, "Unlock") : nullptr;
    cachedType = type;
    cachedOffset = field != nullptr ? static_cast<int32_t>(field->fieldOffset) : -1;
    return cachedOffset;
}

// True when the ability asset's Unlock is a Catalog star card. No syscalls; the set is immutable after load.
bool IsStarCard(const DataContainer* asset)
{
    const int32_t offset = UnlockOffset(asset->getType());
    if (offset < 0)
    {
        return false;
    }
    DataContainer* unlock = *reinterpret_cast<DataContainer* const*>(reinterpret_cast<const uint8_t*>(asset) + offset);
    if (unlock == nullptr)
    {
        return false;
    }
    const Guid* guid = unlock->GetInstanceGuid();
    if (guid == nullptr)
    {
        return false;
    }
    GuidKey key{};
    std::memcpy(&key, guid, sizeof(key));
    return s_starCardUnlocks.count(key) != 0;
}

// Asset Name for logs, or "?".
std::string AssetName(const DataContainer* asset)
{
    const TypeInfo* type = ObjectType(asset);
    const FieldInfoData* field = type != nullptr ? FindField(type, "Name") : nullptr;
    const char* name = nullptr;
    if (field == nullptr || !SafeRead(reinterpret_cast<const uint8_t*>(asset) + field->fieldOffset, &name, sizeof(name)))
    {
        return "?";
    }
    return ReadCString(name);
}

// Answers star card level queries with the querying player's team level; other queries pass through.
int32_t LevelProviderHk(void* self, void* player, DataContainer* asset, int32_t flag)
{
    static const auto trampoline = HookManager::Call(LevelProviderHk);
    const int32_t level = trampoline(self, player, asset, flag);
    if (player == nullptr || asset == nullptr || !s_catalogReady.load(std::memory_order_acquire))
    {
        return level;
    }
    // ServerPlayer (server queries) and ClientPlayer (client queries) both keep m_teamId at 0x58.
    const int32_t team = static_cast<const ServerPlayer*>(player)->m_teamId;
    const int forced = team == 1 ? s_team1Level.load(std::memory_order_relaxed) : team == 2 ? s_team2Level.load(std::memory_order_relaxed) : -1;
    if (forced < 0 || !IsStarCard(asset))
    {
        return level;
    }
    if (s_logBudget.load(std::memory_order_relaxed) > 0 && s_logBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
    {
        KYBER_LOG(Info, "[GCStar] " << AssetName(asset) << " team=" << team << " flag=" << flag << " level " << level << " -> " << forced);
    }
    return forced;
}

// Clamps a level to the PlayerAbilityLevel range, keeping -1 as "game default".
int ClampLevel(int level)
{
    return level < 0 ? -1 : (level > kMaxLevel ? kMaxLevel : level);
}
} // namespace

void SetTeamLevels(int team1, int team2)
{
    team1 = ClampLevel(team1);
    team2 = ClampLevel(team2);
    const bool changed1 = s_team1Level.exchange(team1) != team1;
    const bool changed2 = s_team2Level.exchange(team2) != team2;
    if (changed1 || changed2)
    {
        s_logBudget.store(kOverrideLogBudget, std::memory_order_relaxed);
        KYBER_LOG(Info, "[GCStar] team levels 1=" << team1 << " 2=" << team2);
    }
}

bool ParseLine(const std::string& line)
{
    if (line.rfind(kLinePrefix, 0) != 0)
    {
        return false;
    }
    int team1 = -1, team2 = -1;
    if (std::sscanf(line.c_str() + std::strlen(kLinePrefix), "%d:%d", &team1, &team2) != 2)
    {
        KYBER_LOG(Warning, "[GCStar] bad line " << line);
        return true;
    }
    SetTeamLevels(team1, team2);
    return true;
}

void Clear()
{
    SetTeamLevels(-1, -1);
}

void Poll()
{
    if (!s_hooked.load(std::memory_order_relaxed))
    {
        uint64_t provider = 0, vtable = 0, slot = 0;
        if (SafeRead(reinterpret_cast<const void*>(kLevelProviderGlobal), &provider, sizeof(provider)) && provider != 0 &&
            SafeRead(reinterpret_cast<const void*>(provider), &vtable, sizeof(vtable)) && vtable != 0 &&
            SafeRead(reinterpret_cast<const void*>(vtable + sizeof(void*)), &slot, sizeof(slot)) && slot != 0 && !s_hooked.exchange(true))
        {
            HookManager::CreateHook(reinterpret_cast<Hook::Address>(slot), LevelProviderHk);
            Hook::ApplyQueuedActions();
            KYBER_LOG(Info, "[GCStar] level provider hooked at 0x" << std::hex << slot);
        }
    }
    EnsureCatalog();
}
} // namespace StarCardLevels
} // namespace Kyber
