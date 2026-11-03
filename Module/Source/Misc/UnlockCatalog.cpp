// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Walks a kit's weapon customization tables to list weapon and attachment unlock assets.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Misc/ReflectDump.h>
#include <Misc/UnlockCatalog.h>
#include <SDK/Funcs.h>

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

void ProbeFromEnv()
{
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
