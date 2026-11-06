// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Native-entity wrappers: bind cost rows to spawn cards, write cur/start, and disable depleted widgets.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Core/DebugHooks.h>
#include <Core/Program.h>
#include <Misc/ReinforcementLimits.h>
#include <Misc/ReinforcementText.h>
#include <Misc/ReinforcementWidgets.h>
#include <SDK/Fb/UIIncubatorShared.h>
#include <Utilities/StringUtils.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Kyber
{
namespace
{
// Native data types have no SDK structs here; empty stand-ins give the wrappers a typed data pointer.
class TextElementData : public DataContainer
{};
class BattlepointUnitCostEntityData : public DataContainer
{};
class BattlepointHeroCostEntityData : public DataContainer
{};
class LocalizedStringCacheEntityData : public DataContainer
{};
class WSUIWidgetEntityData : public DataContainer
{};
class FBUIListItemWidgetEntityData : public DataContainer
{};

constexpr size_t kMaxStringProbe = 256;      // Max chars read when probing a possible string.
constexpr int kMaxFieldCount = 512;          // Cap on fields listed per data type.
constexpr size_t kLocalizedHashOffset = 0x18; // LocalizedStringId::StringHash offset (SDK/TypeInfo.h).
constexpr int kMaxSuperDepth = 16;           // Guard against cyclic superclass chains.

// One field of a data type.
struct FieldDesc
{
    std::string name;
    std::string typeName;
    uint16_t offset = 0;
};

// Copies size bytes only when the whole range lies in committed readable memory.
bool SafeRead(const void* ptr, void* out, size_t size)
{
    return ReinforcementText::SafeReadMemory(ptr, out, size);
}

// Reads a NUL-terminated printable string at ptr, or a marker when unreadable or binary.
std::string ReadCString(const void* ptr)
{
    if (ptr == nullptr)
    {
        return "<null>";
    }
    std::string text;
    for (size_t i = 0; i < kMaxStringProbe; i++)
    {
        char c = 0;
        if (!SafeRead(static_cast<const char*>(ptr) + i, &c, 1))
        {
            return text.empty() ? "<unreadable>" : text + "...";
        }
        if (c == 0)
        {
            return text;
        }
        if (static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t')
        {
            return "<binary>";
        }
        text += c;
    }
    return text + "...";
}

// Lists the fields of a data type and its superclasses.
std::vector<FieldDesc> CollectFields(const DataContainer* data)
{
    std::vector<FieldDesc> out;
    if (data == nullptr || data->m_dcType == nullptr)
    {
        return out;
    }
    const ClassInfoData* info = reinterpret_cast<const ClassInfoData*>(data->m_dcType->typeInfoData);
    for (int depth = 0; info != nullptr && depth < kMaxSuperDepth; depth++)
    {
        const FieldInfoData* fields = info->fields;
        const int declared = static_cast<int>(info->fieldCount);
        const int fieldCount = fields == nullptr ? 0 : (declared < kMaxFieldCount ? declared : kMaxFieldCount);
        for (int i = 0; i < fieldCount && static_cast<int>(out.size()) < kMaxFieldCount; i++)
        {
            const FieldInfoData& field = fields[i];
            uint8_t probe[sizeof(FieldInfoData)];
            if (!SafeRead(&field, probe, sizeof(probe)))
            {
                break;
            }
            FieldDesc desc;
            desc.name = field.name != nullptr ? field.name : "?";
            desc.typeName = field.fieldTypePtr != nullptr ? field.fieldTypePtr->getName() : "?";
            desc.offset = field.fieldOffset;
            out.push_back(std::move(desc));
        }
        const ClassInfo* super = info->superClass;
        info = (super != nullptr) ? super->typeInfoData : nullptr;
    }
    return out;
}

// Describes a property value, probing the possible layouts without assuming one.
std::string DescribeValue(const PropertyModification* modification)
{
    std::ostringstream out;
    const TypeInfo* type = modification->valueType;
    if (type == nullptr)
    {
        return "valueType=null";
    }
    const std::string typeName = type->getName();
    const int code = static_cast<int>(type->getBasicType());
    out << "valueType=" << typeName << " code=" << code;

    const void* value = modification->value;
    void* viaPtr = nullptr;
    const bool derefOk = SafeRead(value, &viaPtr, sizeof(viaPtr));

    if (code == kTypeCode_CString)
    {
        out << " asChars=\"" << ReadCString(value) << "\" asPtr=\"" << (derefOk ? ReadCString(viaPtr) : std::string("<unreadable>")) << "\"";
    }
    else if (typeName == "LocalizedStringId")
    {
        int32_t direct = 0;
        int32_t indirect = 0;
        const bool d1 = value != nullptr && SafeRead(static_cast<const uint8_t*>(value) + kLocalizedHashOffset, &direct, sizeof(direct));
        const bool d2 = derefOk && viaPtr != nullptr && SafeRead(static_cast<const uint8_t*>(viaPtr) + kLocalizedHashOffset, &indirect, sizeof(indirect));
        out << std::hex;
        out << " hashDirect=" << (d1 ? std::to_string(direct) : std::string("<unreadable>"))
            << " hashViaPtr=" << (d2 ? std::to_string(indirect) : std::string("<unreadable>"));
    }
    else
    {
        uint64_t raw = 0;
        if (SafeRead(value, &raw, sizeof(raw)))
        {
            out << " raw8=0x" << std::hex << raw;
        }
    }
    return out.str();
}

// Type name and Asset.Name of a DataContainer-like pointer, empty when unreadable.
std::string DescribeObject(const void* object)
{
    if (object == nullptr)
    {
        return "<null>";
    }
    std::ostringstream out;
    out << std::hex << "obj=" << reinterpret_cast<uintptr_t>(object) << std::dec;
    TypeInfo* type = nullptr;
    decltype(type->typeInfoData) info = nullptr;
    decltype(info->name) typeName = nullptr;
    if (SafeRead(reinterpret_cast<const uint8_t*>(object) + sizeof(void*), &type, sizeof(type)) && type != nullptr &&
        SafeRead(&type->typeInfoData, &info, sizeof(info)) && info != nullptr &&
        SafeRead(&info->name, &typeName, sizeof(typeName)) && typeName != nullptr)
    {
        const std::string name = ReadCString(typeName);
        if (!name.empty() && name[0] != '<')
        {
            out << " type=" << name;
        }
    }
    void* namePtr = nullptr;
    if (SafeRead(reinterpret_cast<const uint8_t*>(object) + 0x18, &namePtr, sizeof(namePtr)) && namePtr != nullptr)
    {
        const std::string name = ReadCString(namePtr);
        if (!name.empty() && name[0] != '<')
        {
            out << " name=" << name;
        }
    }
    return out.str();
}

// Reads one CString field as stored bytes and as a pointer, the same two probes used for property changes.
std::string ReadStoredString(const DataContainer* data, uint16_t offset)
{
    if (data == nullptr)
    {
        return "<null>";
    }
    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + offset;
    void* viaPtr = nullptr;
    const bool derefOk = SafeRead(slot, &viaPtr, sizeof(viaPtr));
    std::string direct = ReadCString(slot);
    std::string indirect = derefOk ? ReadCString(viaPtr) : std::string("<unreadable>");
    return direct + "|" + indirect;
}

// Picks the real CString from ReadStoredString's "direct|indirect" pair.
std::string PreferredString(const std::string& stored)
{
    const size_t pipe = stored.find('|');
    const std::string direct = pipe == std::string::npos ? stored : stored.substr(0, pipe);
    const std::string indirect = pipe == std::string::npos ? std::string() : stored.substr(pipe + 1);
    const auto clean = [](const std::string& text) {
        return text.empty() || text[0] == '<' ? std::string() : text;
    };
    const std::string viaPtr = clean(indirect);
    return viaPtr.empty() ? clean(direct) : viaPtr;
}

bool VtableReadable(const void* object);

// Reads bus and parent-bus addresses from an entity.
void EntityBuses(const NativeEntity* entity, uintptr_t& bus, uintptr_t& parentBus)
{
    bus = 0;
    parentBus = 0;
    if (entity == nullptr || !VtableReadable(entity))
    {
        return;
    }
    EntityBus* entityBus = entity->GetEntityBus();
    if (entityBus == nullptr || !VtableReadable(entityBus))
    {
        return;
    }
    bus = reinterpret_cast<uintptr_t>(entityBus);
    EntityBus* parent = nullptr;
    if (SafeRead(&entityBus->m_parentBus, &parent, sizeof(parent)) && parent != nullptr)
    {
        parentBus = reinterpret_cast<uintptr_t>(parent);
    }
}

// Kit already registered on this bus or its parent.
std::string KitOnBuses(uintptr_t bus, uintptr_t parentBus)
{
    std::string kit = ReinforcementText::KitForBus(bus);
    return kit.empty() ? ReinforcementText::KitForBus(parentBus) : kit;
}

// True when the object's vtable slot is readable, so a virtual call is unlikely to jump into garbage.
bool VtableReadable(const void* object)
{
    void* vptr = nullptr;
    void* slot = nullptr;
    return object != nullptr && SafeRead(object, &vptr, sizeof(vptr)) && vptr != nullptr && SafeRead(vptr, &slot, sizeof(slot)) && slot != nullptr;
}

// Data type name of an entity's data, or "<unknown>" when the chain is unreadable.
std::string EntityDataName(const EntityBase* entity)
{
    if (!VtableReadable(entity))
    {
        return "<unknown>";
    }
    const GameObjectData* data = entity->GetData();
    if (!VtableReadable(data))
    {
        return "<unknown>";
    }
    const TypeInfo* type = data->getType();
    if (type == nullptr || type->typeInfoData == nullptr || type->typeInfoData->name == nullptr)
    {
        return "<unknown>";
    }
    return type->typeInfoData->name;
}

// Appends bus, owner entity, and parent owner so a cost row can be matched to its label.
std::string DescribeOwners(const NativeEntity* entity)
{
    if (entity == nullptr || !VtableReadable(entity))
    {
        return " bus=0";
    }
    EntityBus* bus = entity->GetEntityBus();
    if (bus == nullptr || !VtableReadable(bus))
    {
        return " bus=0";
    }
    std::ostringstream out;
    out << std::hex << " bus=" << reinterpret_cast<uintptr_t>(bus);
    EntityOwner* owner = nullptr;
    EntityBus* parentBus = nullptr;
    SafeRead(&bus->m_owner, &owner, sizeof(owner));
    SafeRead(&bus->m_parentBus, &parentBus, sizeof(parentBus));
    NativeEntity* ownerEntity = nullptr;
    if (owner != nullptr && SafeRead(&owner->m_owner, &ownerEntity, sizeof(ownerEntity)) && VtableReadable(ownerEntity))
    {
        out << " ownerEnt=" << reinterpret_cast<uintptr_t>(ownerEntity) << std::dec << " ownerData=" << EntityDataName(ownerEntity) << std::hex;
    }
    else
    {
        out << " ownerEnt=0";
    }
    if (parentBus != nullptr && VtableReadable(parentBus))
    {
        out << " parentBus=" << reinterpret_cast<uintptr_t>(parentBus);
        EntityOwner* parentOwner = nullptr;
        NativeEntity* parentEntity = nullptr;
        if (SafeRead(&parentBus->m_owner, &parentOwner, sizeof(parentOwner)) && parentOwner != nullptr
            && SafeRead(&parentOwner->m_owner, &parentEntity, sizeof(parentEntity)) && VtableReadable(parentEntity))
        {
            out << " parentEnt=" << reinterpret_cast<uintptr_t>(parentEntity) << std::dec << " parentData=" << EntityDataName(parentEntity) << std::hex;
        }
    }
    out << std::dec;
    return out.str();
}

// Base wrapper: logs creation and property changes when GC_TEXT_DISCOVERY=1; always forwards to the original entity.
template<class D>
class GcTextEntityBase : public KyberEntity<D>
{
public:
    // Captures the field list and logs creation.
    GcTextEntityBase(NativeEntity* entity, D* data, bool logChanges)
        : KyberEntity<D>(entity, data)
        , m_logChanges(logChanges)
    {
        m_fields = CollectFields(data);
        for (const FieldDesc& field : m_fields)
        {
            m_names[StringUtils::HashQuick(field.name.c_str())] = field.name;
        }
        LogCreated();
        LogGcDump("create");
        ReinforcementText::RegisterDiscoveryPoll(Address(), DataTypeName(), [this]() { return SnapshotCStrings(); });
    }

    // Drops the poll row; derived classes that override must call this.
    void OnDestroy() override
    {
        ReinforcementText::UnregisterDiscoveryPoll(Address());
        KyberEntityBase::OnDestroy();
    }

    // Forwards to the original entity, then logs the change when enabled.
    void PropertyChanged(PropertyModification* modification) override
    {
        KyberEntityBase::PropertyChanged(modification);
        if (m_logChanges)
        {
            LogChange(modification);
        }
    }

protected:
    // Address of the wrapped native entity as an integer key.
    uintptr_t Address() const
    {
        return reinterpret_cast<uintptr_t>(this->m_nativeEntity);
    }

    // True when the data type has a field with this name.
    bool HasField(const std::string& name) const
    {
        for (const FieldDesc& field : m_fields)
        {
            if (field.name == name)
            {
                return true;
            }
        }
        return false;
    }

    // First matching field name from the list, empty when none exist.
    std::string FirstField(std::initializer_list<const char*> names) const
    {
        for (const char* name : names)
        {
            if (name != nullptr && HasField(name))
            {
                return name;
            }
        }
        return std::string();
    }

    // LocalizedStringId::StringHash on this entity, 0 when missing.
    int32_t ReadLocalizedHash() const
    {
        for (const FieldDesc& field : m_fields)
        {
            if (field.name != "LocalizedStringId")
            {
                continue;
            }
            void* ptr = nullptr;
            if (!SafeRead(reinterpret_cast<const uint8_t*>(this->m_data) + field.offset, &ptr, sizeof(ptr)) || ptr == nullptr)
            {
                return 0;
            }
            int32_t hash = 0;
            if (!SafeRead(static_cast<const uint8_t*>(ptr) + kLocalizedHashOffset, &hash, sizeof(hash)))
            {
                return 0;
            }
            return hash;
        }
        return 0;
    }

    // CString stored on a named field.
    std::string FieldString(const std::string& name) const
    {
        for (const FieldDesc& field : m_fields)
        {
            if (field.name == name)
            {
                return PreferredString(ReadStoredString(this->m_data, field.offset));
            }
        }
        return std::string();
    }

    // All CString fields plus Character/Hero/Text/name so poll and create share one dump.
    std::string SnapshotCStrings() const
    {
        std::ostringstream values;
        values << DescribeOwners(this->m_nativeEntity);
        for (const FieldDesc& field : m_fields)
        {
            const bool named = field.name == "Character" || field.name == "Hero" || field.name == "Text" ||
                               field.name == "InstanceName" || field.name == "Name";
            if (field.typeName == "CString" || named)
            {
                values << " " << field.name << "(" << field.typeName << ")=\"" << ReadStoredString(this->m_data, field.offset) << "\"";
            }
        }
        return values.str();
    }

    // True when a field may hold a settings id, loc id, asset name, or other per-card key.
    static bool IsIdentifierField(const FieldDesc& field)
    {
        const std::string& n = field.name;
        const std::string& t = field.typeName;
        if (t == "CString" || t == "LocalizedStringId" || t == "ResourceRef")
        {
            return true;
        }
        if (t.find("String") != std::string::npos || t.find("Asset") != std::string::npos || t.find("Ref") != std::string::npos)
        {
            return true;
        }
        return n == "Text" || n == "LabelId" || n == "TextKey" || n == "Title" || n == "Name" || n == "Icon" || n == "Asset" ||
               n == "Data" || n == "Character" || n == "Hero" || n == "InstanceName" || n == "LocalizedStringId";
    }

    // Identifier-like fields for [GCDump] (CString, loc id, ResourceRef, named keys).
    std::string SnapshotDump() const
    {
        std::ostringstream values;
        for (const FieldDesc& field : m_fields)
        {
            if (!IsIdentifierField(field))
            {
                continue;
            }
            const uint8_t* slot = reinterpret_cast<const uint8_t*>(this->m_data) + field.offset;
            values << " " << field.name << "(" << field.typeName << ")=";
            if (field.typeName == "CString" || field.name == "Text" || field.name == "Character" || field.name == "Hero" ||
                field.name == "InstanceName" || field.name == "Name" || field.name == "LabelId" || field.name == "TextKey" ||
                field.name == "Title")
            {
                values << "\"" << ReadStoredString(this->m_data, field.offset) << "\"";
                continue;
            }
            if (field.typeName == "LocalizedStringId" || field.name == "LocalizedStringId")
            {
                void* ptr = nullptr;
                SafeRead(slot, &ptr, sizeof(ptr));
                int32_t hash = 0;
                if (ptr != nullptr)
                {
                    SafeRead(static_cast<const uint8_t*>(ptr) + kLocalizedHashOffset, &hash, sizeof(hash));
                }
                values << "hash=" << hash << " " << DescribeObject(ptr);
                continue;
            }
            void* ptr = nullptr;
            uint64_t raw = 0;
            SafeRead(slot, &raw, sizeof(raw));
            SafeRead(slot, &ptr, sizeof(ptr));
            values << "0x" << std::hex << raw << std::dec << " " << DescribeObject(ptr);
        }
        return values.str();
    }

    // Writes one [GCDump] line for this entity.
    void LogGcDump(const char* kind) const
    {
        if (!ReinforcementText::DiscoveryEnabled())
        {
            return;
        }
        uintptr_t bus = 0;
        uintptr_t parentBus = 0;
        EntityBuses(this->m_nativeEntity, bus, parentBus);
        ReinforcementText::DumpCreate(kind, Address(), bus, parentBus, DataTypeName(), SnapshotDump());
    }

    // Registers Character/Hero on this cost row's bus.
    void RegisterCostField(const char* name)
    {
        const std::string kit = FieldString(name);
        uintptr_t bus = 0;
        uintptr_t parentBus = 0;
        EntityBuses(this->m_nativeEntity, bus, parentBus);
        ReinforcementText::RegisterCostKit(Address(), bus, parentBus, kit);
        if (!kit.empty())
        {
            ReinforcementText::BindKit(Address(), kit);
        }
    }

private:
    // Logs the entity, its owner, every CString, and the field list.
    void LogCreated()
    {
        if (!ReinforcementText::DiscoveryEnabled() || !ReinforcementText::DiscoveryLogAllowed())
        {
            return;
        }
        std::ostringstream fields;
        for (const FieldDesc& field : m_fields)
        {
            fields << field.name << ":" << field.typeName << " ";
        }
        KYBER_LOG(Info, "[GCTextDiscovery] created entity=" << std::hex << Address() << std::dec << " data=" << DataTypeName()
                                                            << " isComponent=" << IsComponentText() << SnapshotCStrings()
                                                            << " fields=[ " << fields.str() << "]");
    }

    // Data type name, or "<unknown>" when any link in the chain is null.
    std::string DataTypeName() const
    {
        if (this->m_data == nullptr)
        {
            return "<unknown>";
        }
        const TypeInfo* type = this->m_data->getType();
        if (type == nullptr || type->typeInfoData == nullptr || type->typeInfoData->name == nullptr)
        {
            return "<unknown>";
        }
        return type->typeInfoData->name;
    }

    // "1"/"0" for the component flag, "?" when the native entity is null.
    const char* IsComponentText() const
    {
        if (this->m_nativeEntity == nullptr)
        {
            return "?";
        }
        return this->m_nativeEntity->IsComponent() ? "1" : "0";
    }

    // Logs one property change with its resolved name and value.
    void LogChange(const PropertyModification* modification)
    {
        if (!ReinforcementText::DiscoveryEnabled() || !ReinforcementText::DiscoveryLogAllowed())
        {
            return;
        }
        auto it = m_names.find(modification->nameHash);
        const std::string name = it != m_names.end() ? it->second : std::string("<unknown hash>");
        KYBER_LOG(Info, "[GCTextDiscovery] changed entity=" << std::hex << Address() << std::dec << " data=" << DataTypeName()
                                                            << " property=" << name << " hash=" << std::hex << modification->nameHash << std::dec
                                                            << " " << DescribeValue(modification));
    }

    bool m_logChanges;
    std::vector<FieldDesc> m_fields;
    std::unordered_map<uint32_t, std::string> m_names;
};

// True when text ends with "<digits>/<digits>", i.e. it already carries one of our formatted counts.
bool EndsWithCount(const std::string& text)
{
    size_t end = text.size();
    size_t digits2 = 0;
    while (end > 0 && text[end - 1] >= '0' && text[end - 1] <= '9')
    {
        end--;
        digits2++;
    }
    if (digits2 == 0 || end == 0 || text[end - 1] != '/')
    {
        return false;
    }
    end--;
    size_t digits1 = 0;
    while (end > 0 && text[end - 1] >= '0' && text[end - 1] <= '9')
    {
        end--;
        digits1++;
    }
    return digits1 > 0;
}

// TextElementData wrapper: writes cur/start on the same bus as a cost row.
class GcTextElementEntity : public GcTextEntityBase<TextElementData>
{
public:
    // Binds this label to a cost-row kit on the same bus and starts the cur/start writer.
    GcTextElementEntity(EntityManager* entityManager, NativeEntity* entity, TextElementData* data)
        : GcTextEntityBase<TextElementData>(entity, data, true)
    {
        const std::string& field = ReinforcementText::TextField();
        if (HasField(field))
        {
            m_active = true;
            uintptr_t bus = 0;
            uintptr_t parentBus = 0;
            EntityBuses(this->m_nativeEntity, bus, parentBus);
            ReinforcementText::RegisterTextSlot(Address(), bus, parentBus);
            TryBindFromBus();
            Refresh(true);
        }
        else if (ReinforcementText::DiscoveryEnabled() && ReinforcementText::DiscoveryLogAllowed())
        {
            KYBER_LOG(Warning, "[GCTextDiscovery] field '" << field << "' is not on TextElementData; writer disabled");
        }
    }

    // Drops the per-frame writer registration.
    ~GcTextElementEntity() override
    {
        if (m_registered)
        {
            ReinforcementText::UnregisterWriter(Address());
        }
    }

    // Forwards and logs, then re-reads the label text and rewrites it when the watched field changes.
    void PropertyChanged(PropertyModification* modification) override
    {
        GcTextEntityBase<TextElementData>::PropertyChanged(modification);
        if (!m_active || m_writing)
        {
            return;
        }
        TryBindFromBus();
        const bool isSource = modification->Is(ReinforcementText::SourceField().c_str());
        const bool isField = modification->Is(ReinforcementText::TextField().c_str());
        if (isSource || isField)
        {
            uintptr_t bus = 0;
            uintptr_t parentBus = 0;
            EntityBuses(this->m_nativeEntity, bus, parentBus);
            ReinforcementText::NoteTextSelection(Address(), bus, parentBus, ReadSourceText());
            LogGcDump("select");
            Refresh(isSource);
        }
    }

private:
    // Binds this label when a cost row is already on the same bus or parent bus.
    void TryBindFromBus()
    {
        if (!ReinforcementText::kitFor(Address()).empty())
        {
            return;
        }
        uintptr_t bus = 0;
        uintptr_t parentBus = 0;
        EntityBuses(this->m_nativeEntity, bus, parentBus);
        std::string kit = KitOnBuses(bus, parentBus);
        if (kit.empty())
        {
            kit = ReinforcementText::KitForLocalizedHash(ReadLocalizedHash());
        }
        if (!kit.empty())
        {
            ReinforcementText::BindKit(Address(), kit);
            ReinforcementText::RegisterCostKit(Address(), bus, parentBus, kit);
            RegisterCallback();
        }
    }

    // Creates the field writer on first use (string type resolved lazily); false means retry later.
    bool EnsureWriter()
    {
        if (m_ready)
        {
            return true;
        }
        const std::string& field = ReinforcementText::TextField();
        if (ReinforcementText::LocalizedIdMode())
        {
            m_idWriter = CreateFieldOverride<LocalizedStringId>(field.c_str(), typeInfo_LocalizedStringId);
            if (!m_idWriter.HasConnection())
            {
                return false;
            }
        }
        else
        {
            const TypeInfo* stringType = g_program->m_entityManager->GetNativeType("CString");
            if (stringType == nullptr)
            {
                if (!m_typeMissingLogged)
                {
                    m_typeMissingLogged = true;
                    KYBER_LOG(Warning, "[GCTextDiscovery] native type CString not found yet; will retry");
                }
                return false;
            }
            m_strWriter = CreateFieldOverride<char*>(field.c_str(), stringType);
            if (!m_strWriter.HasConnection())
            {
                return false;
            }
        }
        m_ready = true;
        return true;
    }

    // Registers the per-frame callback once the entity is bound to a kit.
    void RegisterCallback()
    {
        if (m_registered)
        {
            return;
        }
        m_registered = true;
        ReinforcementText::RegisterWriter(Address(), [this]() { Apply(); });
    }

    // Reads the current text of GC_TEXT_SOURCE_FIELD (must be a CString field); empty when unavailable.
    std::string ReadSourceText()
    {
        PropertyReader<char*> reader = GetFieldReader<char*>(ReinforcementText::SourceField().c_str());
        if (!reader.HasConnection() || !reader.HasConnectionValue())
        {
            return std::string();
        }
        const char* text = reader.Get();
        if (text == nullptr)
        {
            return std::string();
        }
        std::string value;
        for (size_t i = 0; i < kMaxStringProbe; i++)
        {
            char c = 0;
            if (!SafeRead(text + i, &c, 1))
            {
                return std::string(); // Unreadable: no usable text.
            }
            if (c == 0)
            {
                return value;
            }
            if (static_cast<unsigned char>(c) < 32)
            {
                return std::string(); // Binary: no usable text.
            }
            value += c;
        }
        return value;
    }

    // Captures the original text, tries the GC_TEXT_BIND mapping and rewrites the label.
    void Refresh(bool readSource)
    {
        if (readSource)
        {
            const std::string text = ReadSourceText();
            const bool sameField = ReinforcementText::SourceField() == ReinforcementText::TextField();
            // With one field for source and target, text we wrote (or that carries a count) is never the original.
            // A dedicated GC_TEXT_SOURCE_FIELD is preferred: it avoids this guess entirely.
            const bool ownText = sameField && (text == m_lastWritten || EndsWithCount(text));
            if (!text.empty() && text != m_lastWritten && !ownText)
            {
                if (sameField || m_orig.empty())
                {
                    m_orig = text;
                }
                m_lastWritten.clear(); // The game replaced the label; force a rewrite.
                if (ReinforcementText::TryBindBySubstring(Address(), text) || ReinforcementText::TryBindByKitTokens(Address(), text))
                {
                    uintptr_t bus = 0;
                    uintptr_t parentBus = 0;
                    EntityBuses(this->m_nativeEntity, bus, parentBus);
                    ReinforcementText::RegisterCostKit(Address(), bus, parentBus, ReinforcementText::kitFor(Address()));
                    RegisterCallback();
                    KYBER_LOG(Info, "[GCText] bound entity=" << std::hex << Address() << std::dec
                                                             << " to kit=" << ReinforcementText::kitFor(Address()) << " by text \""
                                                             << text << "\"");
                }
            }
        }
        Apply();
    }

    // Writes FormatLabel(entry) to the field when the entity is bound to a known kit and the text changed.
    void Apply()
    {
        if (!m_active || m_writing)
        {
            return;
        }
        TryBindFromBus();
        std::string kit = ReinforcementText::kitFor(Address());
        if (kit.empty())
        {
            const std::string label = !m_orig.empty() ? m_orig : ReadSourceText();
            if (ReinforcementText::TryBindBySubstring(Address(), label) || ReinforcementText::TryBindByKitTokens(Address(), label))
            {
                uintptr_t bus = 0;
                uintptr_t parentBus = 0;
                EntityBuses(this->m_nativeEntity, bus, parentBus);
                kit = ReinforcementText::kitFor(Address());
                ReinforcementText::RegisterCostKit(Address(), bus, parentBus, kit);
                RegisterCallback();
                KYBER_LOG(Info, "[GCText] bound entity=" << std::hex << Address() << std::dec << " to kit=" << kit
                                                         << " by late text \"" << label << "\"");
            }
        }
        ReinforcementLimits::Entry entry;
        if (kit.empty() || !ReinforcementText::ResolveEntry(kit, entry))
        {
            return;
        }
        const std::string text = ReinforcementText::FormatLabel(entry, m_orig);
        if (text == m_lastWritten || !EnsureWriter())
        {
            return;
        }
        m_writing = true;
        if (ReinforcementText::LocalizedIdMode())
        {
            // The count is part of the id so the hash changes whenever the text does.
            const std::string id = std::string(ReinforcementText::kLocalizedIdPrefix) + kit + "_" + std::to_string(entry.cur) + "_" +
                                   std::to_string(entry.start);
            LocalizedStringId* container = g_program->m_entityManager->CreateContainer<LocalizedStringId>(typeInfo_LocalizedStringId);
            if (container == nullptr)
            {
                if (!m_containerLogged)
                {
                    m_containerLogged = true;
                    KYBER_LOG(Warning, "[GCTextDiscovery] CreateContainer<LocalizedStringId> returned null; write skipped");
                }
                m_writing = false;
                return;
            }
            ReinforcementText::SetLocalizedText(id, text);
            container->StringHash = ReinforcementText::HashStringId(id);
            m_idWriter = container;
        }
        else
        {
            char* copy = StringUtils::CopyWithArena(text);
            if (copy == nullptr)
            {
                m_writing = false;
                return;
            }
            m_strWriter.Set(&copy);
        }
        m_lastWritten = text;
        m_writing = false;
        static int s_writeLogs = 0;
        if (s_writeLogs < 40)
        {
            s_writeLogs++;
            KYBER_LOG(Info, "[GCText] wrote entity=" << std::hex << Address() << std::dec << " text=\"" << text << "\"");
        }
    }

    PropertyWriter<char*> m_strWriter;
    PropertyWriter<LocalizedStringId> m_idWriter;
    bool m_active = false;            // GC_TEXT_FIELD exists on this entity; observing changes.
    bool m_registered = false;        // Bound to a kit and registered for per-frame rewrites.
    bool m_ready = false;             // Field writer created.
    bool m_writing = false;           // Re-entrancy guard while our own write notifies PropertyChanged.
    bool m_typeMissingLogged = false; // CString type warning already logged.
    bool m_containerLogged = false;   // Null-container warning already logged.
    std::string m_orig;               // Original (cost) text captured from the source field.
    std::string m_lastWritten;        // Last text we wrote, to skip no-op rewrites.
};

// Discovery-only wrapper for data types whose changes are not logged (creation only).
class GcCreationOnlyEntity : public GcTextEntityBase<LocalizedStringCacheEntityData>
{
public:
    // Logs creation only.
    GcCreationOnlyEntity(EntityManager* entityManager, NativeEntity* entity, LocalizedStringCacheEntityData* data)
        : GcTextEntityBase<LocalizedStringCacheEntityData>(entity, data, false)
    {}
};

// Cost row for troopers/reinforcements: publishes Character as the kit id.
class GcUnitCostEntity : public GcTextEntityBase<BattlepointUnitCostEntityData>
{
public:
    // Binds Character onto this entity's bus.
    GcUnitCostEntity(EntityManager* entityManager, NativeEntity* entity, BattlepointUnitCostEntityData* data)
        : GcTextEntityBase<BattlepointUnitCostEntityData>(entity, data, true)
    {
        RegisterCostField("Character");
    }

    // Re-reads Character when the game writes it after create.
    void PropertyChanged(PropertyModification* modification) override
    {
        GcTextEntityBase<BattlepointUnitCostEntityData>::PropertyChanged(modification);
        if (modification->Is("Character"))
        {
            RegisterCostField("Character");
        }
    }

    // Drops the bus binding.
    void OnDestroy() override
    {
        ReinforcementText::UnregisterCostKit(Address());
        GcTextEntityBase<BattlepointUnitCostEntityData>::OnDestroy();
    }
};

// Cost row for heroes: publishes Hero as the kit id.
class GcHeroCostEntity : public GcTextEntityBase<BattlepointHeroCostEntityData>
{
public:
    // Binds Hero onto this entity's bus.
    GcHeroCostEntity(EntityManager* entityManager, NativeEntity* entity, BattlepointHeroCostEntityData* data)
        : GcTextEntityBase<BattlepointHeroCostEntityData>(entity, data, false)
    {
        RegisterCostField("Hero");
    }

    // Re-reads Hero when the game writes it after create.
    void PropertyChanged(PropertyModification* modification) override
    {
        GcTextEntityBase<BattlepointHeroCostEntityData>::PropertyChanged(modification);
        if (modification->Is("Hero"))
        {
            RegisterCostField("Hero");
        }
    }

    // Drops the bus binding.
    void OnDestroy() override
    {
        ReinforcementText::UnregisterCostKit(Address());
        GcTextEntityBase<BattlepointHeroCostEntityData>::OnDestroy();
    }
};

// Spawn-card widget: writes Enabled/IsEnabled from live GCR1 counts.
template<class D>
class GcWidgetEntity : public GcTextEntityBase<D>
{
public:
    // Binds this widget to a cost-row kit and registers the enable writer.
    GcWidgetEntity(EntityManager* entityManager, NativeEntity* entity, D* data)
        : GcTextEntityBase<D>(entity, data, false)
    {
        m_enabledField = this->FirstField({ "Enabled", "IsEnabled" });
        uintptr_t bus = 0;
        uintptr_t parentBus = 0;
        EntityBuses(this->m_nativeEntity, bus, parentBus);
        m_bus = bus != 0 ? bus : parentBus;
        m_kit = KitOnBuses(bus, parentBus);
        ReinforcementWidgets::Bind(this->Address(), m_bus, m_kit, [this](bool enabled) { WriteEnabled(enabled); }, parentBus);
        ReinforcementText::RegisterCardWidget(this->Address(), m_bus, parentBus, EntityDataName(this->m_nativeEntity));
        if (ReinforcementWidgets::IsDiscoveryEnabled() && ReinforcementText::DiscoveryLogAllowed())
        {
            KYBER_LOG(Info, "[WidgetDiscovery] widget=" << std::hex << this->Address() << std::dec << " kit=" << m_kit
                                                        << " enabledField=" << m_enabledField << this->SnapshotCStrings());
        }
    }

    // Drops the widget binding.
    void OnDestroy() override
    {
        ReinforcementWidgets::Unbind(this->Address());
        GcTextEntityBase<D>::OnDestroy();
    }

    // Forwards, then retries kit bind if the cost row appeared later.
    void PropertyChanged(PropertyModification* modification) override
    {
        GcTextEntityBase<D>::PropertyChanged(modification);
        if (m_kit.empty())
        {
            uintptr_t bus = 0;
            uintptr_t parentBus = 0;
            EntityBuses(this->m_nativeEntity, bus, parentBus);
            m_kit = KitOnBuses(bus, parentBus);
        }
    }

private:
    // Writes the TypeInfo enable flag and fires Enable/Disable so input stays blocked.
    void WriteEnabled(bool enabled)
    {
        if (m_kit.empty())
        {
            uintptr_t bus = 0;
            uintptr_t parentBus = 0;
            EntityBuses(this->m_nativeEntity, bus, parentBus);
            m_kit = KitOnBuses(bus, parentBus);
        }
        if (this->m_nativeEntity == nullptr)
        {
            return;
        }
        if (!m_enabledField.empty() && EnsureWriter())
        {
            m_enabledWriter.Set(&enabled);
        }
        this->FireEvent(enabled ? "Enable" : "Disable");
    }

    // Creates the Boolean field writer once the native type is available.
    bool EnsureWriter()
    {
        if (m_ready)
        {
            return true;
        }
        if (g_program == nullptr || g_program->m_entityManager == nullptr)
        {
            return false;
        }
        const TypeInfo* boolType = g_program->m_entityManager->GetNativeType("Boolean");
        if (boolType == nullptr)
        {
            boolType = g_program->m_entityManager->GetNativeType("bool");
        }
        if (boolType == nullptr)
        {
            return false;
        }
        m_enabledWriter = this->template CreateFieldOverride<bool>(m_enabledField.c_str(), boolType);
        m_ready = m_enabledWriter.HasConnection();
        return m_ready;
    }

    PropertyWriter<bool> m_enabledWriter;
    std::string m_enabledField;
    std::string m_kit;
    uintptr_t m_bus = 0;
    bool m_ready = false;
};

// Creator matching KB_INTERNAL_IMPLEMENT_ENTITY's generated function.
template<class W, class D>
KyberEntityBase* CreateWrapper(EntityManager* entityManager, NativeEntity* entity, DataContainer* data)
{
    return new W(entityManager, entity, reinterpret_cast<D*>(data));
}

// Registers the spawn-card UI entity wrappers only with GC_TEXT_HOOKS=1 or discovery; they run on every UI text change.
bool RegisterGcTextCreators()
{
    if (!ReinforcementText::LabelHooksEnabled())
    {
        return false;
    }
    static EntityStaticRegistrar textElement("TextElementData", CreateWrapper<GcTextElementEntity, TextElementData>, false);
    static EntityStaticRegistrar unitCost("BattlepointUnitCostEntityData", CreateWrapper<GcUnitCostEntity, BattlepointUnitCostEntityData>, false);
    static EntityStaticRegistrar heroCost("BattlepointHeroCostEntityData", CreateWrapper<GcHeroCostEntity, BattlepointHeroCostEntityData>, false);
    if (ReinforcementText::DiscoveryEnabled())
    {
        static EntityStaticRegistrar stringCache(
            "LocalizedStringCacheEntityData", CreateWrapper<GcCreationOnlyEntity, LocalizedStringCacheEntityData>, false);
        (void)stringCache;
    }
    static EntityStaticRegistrar wsWidget("WSUIWidgetEntityData", CreateWrapper<GcWidgetEntity<WSUIWidgetEntityData>, WSUIWidgetEntityData>, false);
    static EntityStaticRegistrar listItem(
        "FBUIListItemWidgetEntityData", CreateWrapper<GcWidgetEntity<FBUIListItemWidgetEntityData>, FBUIListItemWidgetEntityData>, false);
    return true;
}

[[maybe_unused]] const bool s_gcTextCreatorsRegistered = RegisterGcTextCreators();
} // namespace
} // namespace Kyber
