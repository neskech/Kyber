// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Walks a kit's weapon customization tables to list weapon and attachment unlock assets.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Core/Program.h>
#include <Utilities/StringUtils.h>
#include <Misc/AbilityWatch.h>
#include <Misc/ReflectDump.h>
#include <Misc/UnlockCatalog.h>
#include <SDK/Funcs.h>
#include <SDK/SDK.h>
#include <SDK/Fb/WS.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>

namespace Kyber
{
namespace UnlockCatalog
{
namespace
{
using namespace Reflect;

constexpr uint32_t kMaxItems = 64; // Elements walked per array.
constexpr int kMaxChain = 12;      // NextLevelUnlockAsset hops per unlock.

std::mutex s_mutex;
std::set<std::string> s_probed;

// True when GC_UNLOCK_PROBE=1.
bool ProbeEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_UNLOCK_PROBE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Valid asset pointer check: header readable and TypeInfo resolvable.
bool Valid(DataContainer* object)
{
    uint64_t head = 0;
    return object != nullptr && SafeRead(object, &head, sizeof(head)) && ReadableType(object->getType());
}

// Pointers stored in a List field of an object.
std::vector<DataContainer*> PointerList(DataContainer* object, const char* field)
{
    std::vector<DataContainer*> out;
    const FieldInfoData* info = FindField(object->getType(), field);
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    if (info == nullptr || !ReadArray(reinterpret_cast<const uint8_t*>(object) + info->fieldOffset, data, size))
    {
        return out;
    }
    for (uint32_t i = 0; i < size && i < kMaxItems; i++)
    {
        DataContainer* item = nullptr;
        if (SafeRead(data + i * sizeof(void*), &item, sizeof(item)) && Valid(item))
        {
            out.push_back(item);
        }
    }
    return out;
}

// Asset name or "?".
std::string AssetName(DataContainer* object)
{
    char* name = nullptr;
    if (!Valid(object) || !SafeRead(&static_cast<Asset*>(object)->Name, &name, sizeof(name)))
    {
        return "?";
    }
    return ReadCString(name);
}

// Instance GUID text or "".
std::string GuidText(DataContainer* object)
{
    uint16_t flags = 0;
    if (!SafeRead(&object->m_dcFlags, &flags, sizeof(flags)) || (flags & DataContainer::HasGuid) == 0)
    {
        return "";
    }
    Guid guid{};
    const Guid* where = object->GetInstanceGuid();
    return where != nullptr && SafeRead(where, &guid, sizeof(guid)) ? guid.ToString() : "";
}

// One log line for an unlock asset (and its next-level chain).
void LogUnlock(const std::string& kit, const std::string& label, DataContainer* unlock)
{
    DataContainer* current = unlock;
    for (int level = 0; level < kMaxChain && Valid(current); level++)
    {
        const TypeInfo* type = current->getType();
        uint32_t id = 0, score = 0, avail = 0, weaponId = 0, category = 0;
        bool autoAvail = false, hidden = false;
        ReadU32(current, type, "Identifier", id);
        ReadU32(current, type, "UnlockScore", score);
        ReadU32(current, type, "AvailableForPlayer", avail);
        ReadBool(current, type, "AutoAvailable", autoAvail);
        ReadBool(current, type, "HiddenInProgression", hidden);
        ReadU32(current, type, "WeaponIdentifier", weaponId);
        ReadU32(current, type, "Category", category);
        DataContainer* next = ReadPointerField(current, type, "NextLevelUnlockAsset");
        DataContainer* userData = ReadPointerField(current, type, "UnlockUserData");
        std::ostringstream line;
        line << "[GCUnlock] " << kit << " " << label << " L" << level << " " << TypeName(type) << " name=" << AssetName(current)
             << " guid=" << GuidText(current) << " id=0x" << std::hex << id << std::dec << " score=" << score << " avail=" << avail
             << " auto=" << autoAvail << " hidden=" << hidden << " weaponId=0x" << std::hex << weaponId << std::dec << " cat=" << category
             << " next=" << (Valid(next) ? AssetName(next) : "-");
        if (Valid(userData))
        {
            line << " userData=" << TypeName(userData->getType()) << " "
                 << DumpFields(reinterpret_cast<const uint8_t*>(userData), userData->getType(), 0);
        }
        KYBER_LOG(Info, line.str());
        if (next == current)
        {
            break;
        }
        current = next;
    }
}

// Logs every group and selectable unlock of one CustomizationTable.
void WalkTable(const std::string& kit, const std::string& label, DataContainer* table, int depth)
{
    if (!Valid(table))
    {
        return;
    }
    const std::vector<DataContainer*> groups = PointerList(table, "UnlockParts");
    for (size_t g = 0; g < groups.size(); g++)
    {
        const std::vector<DataContainer*> unlocks = PointerList(groups[g], "SelectableUnlocks");
        const FieldInfoData* category = FindField(groups[g]->getType(), "UICategorySid");
        const char* sid = nullptr;
        if (category != nullptr)
        {
            SafeRead(reinterpret_cast<const uint8_t*>(groups[g]) + category->fieldOffset, &sid, sizeof(sid));
        }
        KYBER_LOG(Info, "[GCUnlock] " << kit << " " << label << ".g" << g << " category=" << ReadCString(sid) << " unlocks=" << unlocks.size());
        for (size_t u = 0; u < unlocks.size(); u++)
        {
            const std::string name = label + ".g" + std::to_string(g) + ".u" + std::to_string(u);
            LogUnlock(kit, name, unlocks[u]);
            if (depth == 0 && g == 0 && u == 0)
            {
                DataContainer* blueprint = ReadPointerField(unlocks[u], unlocks[u]->getType(), "NonStreamedBlueprint");
                DataContainer* weaponData = Valid(blueprint) ? ReadPointerField(blueprint, blueprint->getType(), "Object") : nullptr;
                if (Valid(weaponData))
                {
                    std::string dump = DumpFields(reinterpret_cast<const uint8_t*>(weaponData), weaponData->getType(), 1);
                    size_t at = dump.find("WeaponModifierData=");
                    KYBER_LOG(Info, "[GCUnlock] weaponData " << kit << " " << (at == std::string::npos ? std::string("none") : dump.substr(at, 2500)));
                }
            }
            if (depth < 2)
            {
                // Weapons carry their attachment tables on Customization (directly or through a customization asset).
                DataContainer* nested = ReadPointerField(unlocks[u], unlocks[u]->getType(), "Customization");
                if (Valid(nested) && FindField(nested->getType(), "UnlockParts") == nullptr)
                {
                    KYBER_LOG(Info, "[GCUnlock] " << kit << " " << name << " customization=" << TypeName(nested->getType()) << " "
                                                  << AssetName(nested));
                    nested = ReadPointerField(nested, nested->getType(), "Customization");
                }
                WalkTable(kit, name + ".att", nested, depth + 1);
            }
        }
    }
}
} // namespace

void Probe(DataContainer* kit, const std::string& path)
{
    if (!ProbeEnabled() || !Valid(kit))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_probed.insert(path).second)
        {
            return;
        }
    }
    DataContainer* table = ReadPointerField(kit, kit->getType(), "WeaponTable");
    KYBER_LOG(Info, "[GCUnlock] kit " << path << " weaponTable=" << (Valid(table) ? TypeName(table->getType()) : "null"));
    WalkTable(path, "w", table, 0);
    WalkTable(path, "s", ReadPointerField(kit, kit->getType(), "SpecializationTable"), 2);
}

// Logs one offline inventory settings group: Enabled, item count, and the first items.
static void ProbeInventory(const char* id)
{
    DataContainer* settings = g_program->GetSettingsObject<DataContainer>(id);
    if (!Valid(settings))
    {
        KYBER_LOG(Info, "[GCInv] " << id << " settings=null");
        return;
    }
    bool enabled = false;
    ReadBool(settings, settings->getType(), "Enabled", enabled);
    const FieldInfoData* info = FindField(settings->getType(), "InventoryItems");
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    bool hasArray = info != nullptr && ReadArray(reinterpret_cast<const uint8_t*>(settings) + info->fieldOffset, data, size);
    std::ostringstream items;
    for (uint32_t i = 0; hasArray && i < size && i < 40; ++i)
    {
        const char* item = nullptr;
        if (SafeRead(data + i * sizeof(char*), &item, sizeof(item)))
        {
            items << ReadCString(item) << ",";
        }
    }
    KYBER_LOG(Info, "[GCInv] " << id << " type=" << TypeName(settings->getType()) << " Enabled=" << enabled << " field=" << (info != nullptr)
                               << " count=" << size << " items=" << items.str());
}

// Asset name when p points at a PlayerAbilityAsset subtype, else empty; reads the header TypeInfo, never calls into the object.
static std::string AbilityName(uint64_t p)
{
    const TypeInfo* type = nullptr;
    if (p < 0x10000 || !SafeRead(reinterpret_cast<const uint8_t*>(p) + sizeof(void*), &type, sizeof(type)) || !ReadableType(type) || std::strstr(TypeName(type), "PlayerAbilityAsset") == nullptr)
    {
        return {};
    }
    const char* name = nullptr;
    SafeRead(reinterpret_cast<const uint8_t*>(p) + 0x18, &name, sizeof(name));
    return ReadCString(name);
}

// Hex of `count` bytes at p, or "?" when unreadable.
static std::string Hex(const uint8_t* p, int count)
{
    std::vector<uint8_t> bytes(count);
    if (!SafeRead(p, bytes.data(), count))
    {
        return "?";
    }
    std::ostringstream out;
    for (int i = 0; i < count; ++i)
    {
        out << std::hex << (bytes[i] < 16 ? "0" : "") << int(bytes[i]) << ((i % 8 == 7) ? " " : "");
    }
    return out.str();
}

// Logs every ability asset pointer within `size` bytes of base, and one pointer hop deeper.
static void ScanForAbilities(const char* where, const uint8_t* base, uint32_t size, uint32_t innerSize)
{
    for (uint32_t off = 0; off < size; off += 8)
    {
        uint64_t q = 0;
        if (!SafeRead(base + off, &q, sizeof(q)))
        {
            continue;
        }
        std::string name = AbilityName(q);
        if (!name.empty())
        {
            KYBER_LOG(Info, "[GCAbil] " << where << " off=0x" << std::hex << off << " " << name << " ctx=" << Hex(base + off - 0x10, 0x40));
            continue;
        }
        if (q < 0x10000 || q > 0x7FFFFFFFFFFF)
        {
            continue;
        }
        for (uint32_t inner = 0; inner < innerSize; inner += 8)
        {
            uint64_t r = 0;
            if (!SafeRead(reinterpret_cast<const uint8_t*>(q) + inner, &r, sizeof(r)))
            {
                break;
            }
            std::string innerName = AbilityName(r);
            if (!innerName.empty())
            {
                const uint8_t* at = reinterpret_cast<const uint8_t*>(q) + inner;
                KYBER_LOG(Info, "[GCAbil] " << where << " off=0x" << std::hex << off << "->0x" << inner << " " << innerName
                                            << " ctx=" << Hex(at - 0x10, 0x40));
            }
        }
    }
}

void DumpAbilities(ServerPlayer* player)
{
    if (player == nullptr)
    {
        return;
    }
    KYBER_LOG(Info, "[GCAbil] dump " << player->m_name << " player=0x" << std::hex << reinterpret_cast<uintptr_t>(player) << " abilityExtent=0x"
                                     << reinterpret_cast<uintptr_t>(player->GetWSServerPlayerAbilityExtent()) << " character=0x"
                                     << reinterpret_cast<uintptr_t>(player->GetCharacterEntity()));
    ScanForAbilities("extent", reinterpret_cast<const uint8_t*>(player->GetWSServerPlayerAbilityExtent()), 0x200, 0x400);
}

void ApplyOfflineInventoryFromEnv()
{
    const char* list = std::getenv("GC_OFFLINE_INVENTORY");
    if (list == nullptr)
    {
        return;
    }
    DataContainer* settings = g_program->GetSettingsObject<DataContainer>("WSOfflineInventory");
    const FieldInfoData* enabled = Valid(settings) ? FindField(settings->getType(), "Enabled") : nullptr;
    const FieldInfoData* items = Valid(settings) ? FindField(settings->getType(), "InventoryItems") : nullptr;
    if (enabled == nullptr || items == nullptr)
    {
        KYBER_LOG(Info, "[GCInv] apply failed: WSOfflineInventory fields not found");
        return;
    }
    std::vector<char*> names;
    std::stringstream stream(list);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        if (!name.empty())
        {
            names.push_back(StringUtils::CopyWithArena(name));
        }
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(settings);
    reinterpret_cast<FBArray<char*>*>(base + items->fieldOffset)->cloneFromVec(names);
    *reinterpret_cast<bool*>(base + enabled->fieldOffset) = true;
    KYBER_LOG(Info, "[GCInv] applied WSOfflineInventory Enabled=1 count=" << names.size());
}

void ProbeFromEnv()
{
    AbilityWatch::Poll();
    static std::once_flag inventoryOnce;
    if (ProbeEnabled())
    {
        std::call_once(inventoryOnce, []() {
            ProbeInventory("WSOfflineInventory");
            ProbeInventory("WSEventOfflineInventory");
        });
    }
    const char* list = std::getenv("GC_UNLOCK_PROBE_KITS");
    if (!ProbeEnabled() || list == nullptr)
    {
        return;
    }
    std::stringstream stream(list);
    std::string path;
    while (std::getline(stream, path, ','))
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (path.empty() || s_probed.count(path) != 0)
            {
                continue;
            }
        }
        if (DataContainer* kit = ResourceManagerLookupDataContainer(path.c_str()))
        {
            Probe(kit, path);
        }
    }
}
} // namespace UnlockCatalog
} // namespace Kyber
