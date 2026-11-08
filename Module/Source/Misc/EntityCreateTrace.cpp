// Lock-free ring of recent entity-create type+asset names for crash dumps.

#include <Misc/EntityCreateTrace.h>

#include <Base/Log.h>
#include <Misc/ReflectDump.h>
#include <SDK/SDK.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>

namespace Kyber
{
namespace EntityCreateTrace
{
namespace
{
constexpr uint32_t kRingSize = 64;
constexpr size_t kNameCap = 96;

struct Rec
{
    const char* type;
    char name[kNameCap];
};

std::atomic<uint32_t> s_next{ 0 };
Rec s_ring[kRingSize] = {};

// True when GC_ENTITY_CREATE_LOG=1 asks for one log line per create.
bool LogEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_ENTITY_CREATE_LOG");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Copies a CString into dest, truncated.
void CopyName(char* dest, const char* src)
{
    dest[0] = 0;
    if (src == nullptr)
    {
        return;
    }
    const std::string s = Reflect::ReadCString(src);
    std::snprintf(dest, kNameCap, "%s", s.c_str());
}
} // namespace

// Spawn/loadout types whose factory params we dump.
bool FocusType(const char* typeName)
{
    if (typeName == nullptr)
    {
        return false;
    }
    return std::strstr(typeName, "Customization") != nullptr || std::strstr(typeName, "VisualUnlock") != nullptr
           || std::strstr(typeName, "SoldierWeapon") != nullptr || std::strstr(typeName, "Preview") != nullptr
           || std::strstr(typeName, "KitLimitation") != nullptr || std::strstr(typeName, "Appearance") != nullptr;
}

void Record(DataContainer* data, void* factoryParams)
{
    const char* typeName = "?";
    char asset[kNameCap] = {};
    uint32_t ident = 0;
    bool haveIdent = false;
    if (data != nullptr)
    {
        const TypeInfo* type = data->getType();
        if (Reflect::ReadableType(type))
        {
            typeName = Reflect::TypeName(type);
            if (FocusType(typeName))
            {
                const FieldInfoData* nameField = Reflect::FindField(type, "Name");
                if (nameField != nullptr)
                {
                    const char* namePtr = nullptr;
                    if (Reflect::SafeRead(reinterpret_cast<const uint8_t*>(data) + nameField->fieldOffset, &namePtr, sizeof(namePtr)))
                    {
                        CopyName(asset, namePtr);
                    }
                }
                haveIdent = Reflect::ReadU32(data, type, "Identifier", ident);
            }
        }
        else if (type != nullptr)
        {
            typeName = type->getName();
        }
    }
    const uint32_t slot = s_next.fetch_add(1, std::memory_order_relaxed) % kRingSize;
    s_ring[slot].type = typeName;
    std::memcpy(s_ring[slot].name, asset, kNameCap);
    if (!LogEnabled() || !FocusType(typeName))
    {
        return;
    }
    if (haveIdent)
    {
        KYBER_LOG(Info, "[EntityCreate] fbType=" << typeName << " assetName=" << (asset[0] ? asset : "-")
                                                << " ident=0x" << std::hex << ident);
    }
    else
    {
        KYBER_LOG(Info, "[EntityCreate] fbType=" << typeName << " assetName=" << (asset[0] ? asset : "-"));
    }
    // No fill-slot walk: a non-Entity pointer at +0x120 AVs (native Kamino).
}

void LogRecent()
{
    const uint32_t next = s_next.load(std::memory_order_relaxed);
    const uint32_t count = next < kRingSize ? next : kRingSize;
    for (uint32_t i = 0; i < count; i++)
    {
        const Rec& rec = s_ring[(next - count + i) % kRingSize];
        KYBER_LOG(Info, "[EntityCreate] recent " << (count - i) << ": " << (rec.type != nullptr ? rec.type : "?")
                                                 << (rec.name[0] ? " " : "") << rec.name);
    }
}
} // namespace EntityCreateTrace
} // namespace Kyber
