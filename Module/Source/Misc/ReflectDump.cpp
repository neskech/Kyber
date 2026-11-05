// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Safe reflection helpers shared by the GC kit-limitation bridge and UI probes.

#include <Misc/ReflectDump.h>

#include <Misc/ReinforcementText.h>

#include <sstream>

namespace Kyber
{
namespace Reflect
{
namespace
{
constexpr int kMaxSuperDepth = 16;     // Guard against cyclic superclass chains.
constexpr int kMaxDumpDepth = 6;       // Nesting cap for reflected dumps.
constexpr uint32_t kMaxArrayDump = 96; // Elements printed per array.

// Fields declared directly on a class or value type (not its superclasses).
void DeclaredFields(const TypeInfo* type, const FieldInfoData*& fields, int& count)
{
    fields = nullptr;
    count = 0;
    if (type == nullptr || type->typeInfoData == nullptr)
    {
        return;
    }
    const TypeCodeEnum code = type->getBasicType();
    if (code == kTypeCode_Class)
    {
        const ClassInfoData* info = reinterpret_cast<const ClassInfoData*>(type->typeInfoData);
        fields = info->fields;
        count = info->fieldCount;
    }
    else if (code == kTypeCode_ValueType)
    {
        const ValueTypeInfoData* info = reinterpret_cast<const ValueTypeInfoData*>(type->typeInfoData);
        fields = info->fields;
        count = info->fieldCount;
    }
}
} // namespace

bool SafeRead(const void* ptr, void* out, size_t size)
{
    return ReinforcementText::SafeReadMemory(ptr, out, size);
}

std::string ReadCString(const char* ptr)
{
    if (ptr == nullptr)
    {
        return "<null>";
    }
    std::string text;
    for (size_t i = 0; i < 200; i++)
    {
        char c = 0;
        if (!SafeRead(ptr + i, &c, 1))
        {
            return text + "<unreadable>";
        }
        if (c == 0)
        {
            return text;
        }
        text += (static_cast<unsigned char>(c) < 32) ? '?' : c;
    }
    return text + "...";
}

bool ReadableType(const TypeInfo* type)
{
    const TypeInfoData* data = nullptr;
    if (type == nullptr || !SafeRead(&type->typeInfoData, &data, sizeof(data)) || data == nullptr)
    {
        return false;
    }
    const char* name = nullptr;
    char first = 0;
    return SafeRead(&data->name, &name, sizeof(name)) && name != nullptr && SafeRead(name, &first, 1) && first != 0;
}

const char* TypeName(const TypeInfo* type)
{
    if (!ReadableType(type))
    {
        return "?";
    }
    return type->typeInfoData->name;
}

const FieldInfoData* FindField(const TypeInfo* type, const char* name)
{
    for (int depth = 0; type != nullptr && depth < kMaxSuperDepth; depth++)
    {
        const FieldInfoData* fields = nullptr;
        int count = 0;
        DeclaredFields(type, fields, count);
        for (int i = 0; fields != nullptr && i < count; i++)
        {
            if (fields[i].name != nullptr && _stricmp(fields[i].name, name) == 0)
            {
                return &fields[i];
            }
        }
        if (type->getBasicType() != kTypeCode_Class)
        {
            return nullptr;
        }
        const ClassInfo* super = reinterpret_cast<const ClassInfoData*>(type->typeInfoData)->superClass;
        const TypeInfo* next = reinterpret_cast<const TypeInfo*>(super);
        if (next == type)
        {
            return nullptr;
        }
        type = next;
    }
    return nullptr;
}

std::vector<const FieldInfoData*> AllFields(const TypeInfo* type)
{
    std::vector<const FieldInfoData*> out;
    for (int depth = 0; type != nullptr && depth < kMaxSuperDepth; depth++)
    {
        const FieldInfoData* fields = nullptr;
        int count = 0;
        DeclaredFields(type, fields, count);
        for (int i = 0; fields != nullptr && i < count; i++)
        {
            out.push_back(&fields[i]);
        }
        if (type->getBasicType() != kTypeCode_Class)
        {
            break;
        }
        const TypeInfo* next = reinterpret_cast<const TypeInfo*>(reinterpret_cast<const ClassInfoData*>(type->typeInfoData)->superClass);
        if (next == type)
        {
            break;
        }
        type = next;
    }
    return out;
}

bool ReadU32(const void* object, const TypeInfo* type, const char* name, uint32_t& out)
{
    const FieldInfoData* field = FindField(type, name);
    return field != nullptr && SafeRead(static_cast<const uint8_t*>(object) + field->fieldOffset, &out, sizeof(out));
}

bool ReadBool(const void* object, const TypeInfo* type, const char* name, bool& out)
{
    const FieldInfoData* field = FindField(type, name);
    return field != nullptr && SafeRead(static_cast<const uint8_t*>(object) + field->fieldOffset, &out, sizeof(out));
}

DataContainer* ReadPointerField(const void* object, const TypeInfo* type, const char* name)
{
    const FieldInfoData* field = FindField(type, name);
    DataContainer* out = nullptr;
    if (field == nullptr || !SafeRead(static_cast<const uint8_t*>(object) + field->fieldOffset, &out, sizeof(out)))
    {
        return nullptr;
    }
    return out;
}

bool ReadArray(const void* slot, const uint8_t*& data, uint32_t& size)
{
    data = nullptr;
    size = 0;
    if (!SafeRead(slot, &data, sizeof(data)) || data == nullptr)
    {
        return false;
    }
    return SafeRead(data - sizeof(uint32_t), &size, sizeof(size));
}

std::string DumpFields(const uint8_t* p, const TypeInfo* type, int depth)
{
    std::ostringstream out;
    out << "{";
    for (int level = 0; type != nullptr && level < kMaxSuperDepth; level++)
    {
        const FieldInfoData* fields = nullptr;
        int count = 0;
        DeclaredFields(type, fields, count);
        for (int i = 0; fields != nullptr && i < count; i++)
        {
            out << " " << (fields[i].name != nullptr ? fields[i].name : "?") << "="
                << DumpValue(p + fields[i].fieldOffset, fields[i].fieldTypePtr, depth + 1);
        }
        if (type->getBasicType() != kTypeCode_Class)
        {
            break;
        }
        const TypeInfo* next = reinterpret_cast<const TypeInfo*>(reinterpret_cast<const ClassInfoData*>(type->typeInfoData)->superClass);
        if (next == type)
        {
            break;
        }
        type = next;
    }
    out << " }";
    return out.str();
}

std::string DumpValue(const uint8_t* p, const TypeInfo* type, int depth)
{
    if (type == nullptr || type->typeInfoData == nullptr)
    {
        return "<notype>";
    }
    if (depth > kMaxDumpDepth)
    {
        return "<deep>";
    }
    std::ostringstream out;
    switch (type->getBasicType())
    {
    case kTypeCode_Boolean: {
        bool v = false;
        SafeRead(p, &v, 1);
        return v ? "true" : "false";
    }
    case kTypeCode_Int8:
    case kTypeCode_Uint8: {
        uint8_t v = 0;
        SafeRead(p, &v, 1);
        return std::to_string(v);
    }
    case kTypeCode_Int16:
    case kTypeCode_Uint16: {
        uint16_t v = 0;
        SafeRead(p, &v, 2);
        return std::to_string(v);
    }
    case kTypeCode_Int32:
    case kTypeCode_Uint32:
    case kTypeCode_Enum: {
        uint32_t v = 0;
        SafeRead(p, &v, 4);
        out << static_cast<int32_t>(v) << "(0x" << std::hex << v << ")";
        return out.str();
    }
    case kTypeCode_Int64:
    case kTypeCode_Uint64: {
        uint64_t v = 0;
        SafeRead(p, &v, 8);
        out << "0x" << std::hex << v;
        return out.str();
    }
    case kTypeCode_Float32: {
        float v = 0;
        SafeRead(p, &v, 4);
        out << v;
        return out.str();
    }
    case kTypeCode_CString: {
        const char* s = nullptr;
        SafeRead(p, &s, sizeof(s));
        return "\"" + ReadCString(s) + "\"";
    }
    case kTypeCode_Class: {
        const uint8_t* obj = nullptr;
        SafeRead(p, &obj, sizeof(obj));
        if (obj == nullptr)
        {
            return "null";
        }
        TypeInfo* objType = nullptr;
        SafeRead(obj + sizeof(void*), &objType, sizeof(objType));
        if (!ReadableType(objType))
        {
            out << "<badtype @" << std::hex << reinterpret_cast<uintptr_t>(obj) << ">";
            return out.str();
        }
        out << "<" << TypeName(objType) << " @" << std::hex << reinterpret_cast<uintptr_t>(obj) << std::dec;
        for (const char* key : { "CharacterKitId", "CharacterId", "Identifier", "KitId" })
        {
            uint32_t id = 0;
            if (objType != nullptr && ReadU32(obj, objType, key, id))
            {
                out << " " << key << "=0x" << std::hex << id << std::dec;
            }
        }
        const FieldInfoData* name = objType != nullptr ? FindField(objType, "Name") : nullptr;
        if (name != nullptr && name->fieldTypePtr != nullptr && name->fieldTypePtr->getBasicType() == kTypeCode_CString)
        {
            out << " Name=" << DumpValue(obj + name->fieldOffset, name->fieldTypePtr, depth + 1);
        }
        out << ">";
        return out.str();
    }
    case kTypeCode_ValueType:
        return DumpFields(p, type, depth);
    case kTypeCode_Array: {
        const TypeInfo* element = reinterpret_cast<const ArrayTypeInfoData*>(type->typeInfoData)->elementType;
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        if (!ReadArray(p, data, size) || element == nullptr || element->typeInfoData == nullptr)
        {
            return "[]";
        }
        const TypeCodeEnum code = element->getBasicType();
        const size_t stride = (code == kTypeCode_Class || code == kTypeCode_CString) ? sizeof(void*) : element->typeInfoData->totalSize;
        out << "[" << size << ":";
        for (uint32_t i = 0; i < size && i < kMaxArrayDump; i++)
        {
            out << " #" << i << "=" << DumpValue(data + i * stride, element, depth + 1);
        }
        out << " ]";
        return out.str();
    }
    default:
        out << "<code" << static_cast<int>(type->getBasicType()) << ">";
        return out.str();
    }
}
std::string Sniff(const void* p, int count, int depth)
{
    std::ostringstream out;
    out << "[";
    for (int i = 0; i < count; i++)
    {
        uint64_t q = 0;
        if (!SafeRead(static_cast<const uint8_t*>(p) + i * 8, &q, sizeof(q)))
        {
            out << " +" << i * 8 << "=<unreadable>";
            break;
        }
        out << " +" << i * 8 << "=0x" << std::hex << q << std::dec;
        const void* ptr = reinterpret_cast<const void*>(q);
        uint64_t probe = 0;
        if (q < 0x10000 || !SafeRead(ptr, &probe, sizeof(probe)))
        {
            continue;
        }
        if (ReadableType(static_cast<const TypeInfo*>(ptr)))
        {
            out << "(T:" << TypeName(static_cast<const TypeInfo*>(ptr)) << ")";
            continue;
        }
        const TypeInfo* objType = nullptr;
        if (SafeRead(static_cast<const uint8_t*>(ptr) + sizeof(void*), &objType, sizeof(objType)) && ReadableType(objType))
        {
            out << "(O:" << TypeName(objType);
            if (depth > 0)
            {
                out << " " << Sniff(ptr, 8, depth - 1);
            }
            out << ")";
            continue;
        }
        const char* text = static_cast<const char*>(ptr);
        char c0 = 0;
        if (SafeRead(text, &c0, 1) && c0 >= 32 && c0 < 127)
        {
            const std::string s = ReadCString(text);
            if (s.size() >= 4 && s.find('?') == std::string::npos)
            {
                out << "(S:\"" << s << "\")";
                continue;
            }
        }
        if (depth > 0)
        {
            out << "(P " << Sniff(ptr, 6, depth - 1) << ")";
        }
    }
    out << " ]";
    return out.str();
}
} // namespace Reflect
} // namespace Kyber
