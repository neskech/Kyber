// Logs ANT compact-id assignment (exe+0x1127000 / 0x11270b0) and registry get (exe+0x6A3F960).

#include <Misc/AntTreeTrace.h>

#include <Base/Log.h>
#include <Hook/HookManager.h>
#include <Misc/ReflectDump.h>
#include <SDK/SDK.h>
#include <Utilities/PlatformUtils.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <windows.h>

namespace Kyber
{
namespace AntTreeTrace
{
namespace
{
constexpr int kStackMax = 8;
constexpr uint32_t kIdCap = 120;
constexpr uint32_t kGetCap = 40;
constexpr uint16_t kSentinel = 0xBBD;

std::atomic<uint32_t> s_idObjCount{ 0 };
std::atomic<uint32_t> s_idTypeCount{ 0 };
std::atomic<uint32_t> s_getCount{ 0 };
std::atomic<uint32_t> s_bbdFromObj{ 0 };
std::atomic<uint32_t> s_bbdFromType{ 0 };
std::atomic<uint32_t> s_bbdGet{ 0 };
std::atomic<uint32_t> s_getHit{ 0 };
std::atomic<uint32_t> s_getMiss{ 0 };

bool LogOn()
{
    static const bool on = PlatformUtils::GetEnv("GC_ANT_TREE_LOG", "0") == "1";
    return on;
}

// exe-relative stack of the current hook call.
void LogStack(const char* tag)
{
    void* frames[kStackMax + 2] = {};
    const USHORT n = RtlCaptureStackBackTrace(2, kStackMax, frames, nullptr);
    const uintptr_t exe = PlatformUtils::BaseAddress();
    std::ostringstream oss;
    oss << "[AntTree] " << tag << " stack";
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(frames[i]);
        oss << " " << i << "=exe+0x" << std::hex << (p >= exe ? p - exe : p);
    }
    KYBER_LOG(Info, oss.str());
}

// getType via SEH; no C++ objects (C2712).
const TypeInfo* SafeGetType(void* dc)
{
    if (dc == nullptr)
    {
        return nullptr;
    }
    auto* obj = reinterpret_cast<DataContainer*>(dc);
    const TypeInfo* type = nullptr;
    __try
    {
        type = obj->getType();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        type = nullptr;
    }
    return type;
}

// Type + Name of a DataContainer, or "-" if unreadable.
void DescribeDc(void* dc, const char*& typeName, char* asset, size_t assetCap)
{
    typeName = "-";
    asset[0] = 0;
    const TypeInfo* type = SafeGetType(dc);
    if (!Reflect::ReadableType(type))
    {
        return;
    }
    typeName = Reflect::TypeName(type);
    const FieldInfoData* nameField = Reflect::FindField(type, "Name");
    if (nameField == nullptr)
    {
        return;
    }
    const char* namePtr = nullptr;
    if (Reflect::SafeRead(reinterpret_cast<const uint8_t*>(dc) + nameField->fieldOffset, &namePtr, sizeof(namePtr)))
    {
        const std::string s = Reflect::ReadCString(namePtr);
        std::snprintf(asset, assetCap, "%s", s.c_str());
    }
}

bool InterestingName(const char* typeName, const char* asset)
{
    auto has = [](const char* s, const char* needle) {
        return s != nullptr && std::strstr(s, needle) != nullptr;
    };
    return has(typeName, "Affector") || has(typeName, "Ant") || has(asset, "B1") || has(asset, "b1") || has(asset, "Affector")
           || has(asset, "Clone") || has(asset, "clone");
}

uint16_t __fastcall IdFromObjectHk(void* dc)
{
    static auto trampoline = HookManager::Call(IdFromObjectHk);
    const uint16_t id = trampoline(dc);
    const uint32_t n = s_idObjCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == kSentinel)
    {
        s_bbdFromObj.fetch_add(1, std::memory_order_relaxed);
    }
    if (!LogOn())
    {
        return id;
    }
    const char* typeName = "-";
    char asset[96] = {};
    DescribeDc(dc, typeName, asset, sizeof(asset));
    const bool hot = id == kSentinel || id == 3 || InterestingName(typeName, asset);
    if (!hot && n > kIdCap)
    {
        return id;
    }
    KYBER_LOG(Info, "[AntTree] idObj n=" << n << " id=0x" << std::hex << id << " dc=0x" << reinterpret_cast<uintptr_t>(dc)
                                         << " fbType=" << typeName << " asset=" << (asset[0] ? asset : "-"));
    if (hot || n <= 12)
    {
        LogStack("idObj");
    }
    return id;
}

uint16_t __fastcall IdFromTypeHk(uint16_t typeId)
{
    static auto trampoline = HookManager::Call(IdFromTypeHk);
    const uint16_t id = trampoline(typeId);
    const uint32_t n = s_idTypeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == kSentinel)
    {
        s_bbdFromType.fetch_add(1, std::memory_order_relaxed);
    }
    if (!LogOn())
    {
        return id;
    }
    const bool hot = id == kSentinel || id == 3 || typeId >= 0x7D2;
    if (!hot && n > 50)
    {
        return id;
    }
    KYBER_LOG(Info, "[AntTree] idType n=" << n << " typeId=0x" << std::hex << typeId << " id=0x" << id);
    if (hot || n <= 8)
    {
        LogStack("idType");
    }
    return id;
}

void* __fastcall RegistryGetHk(void* node, uint16_t id)
{
    static auto trampoline = HookManager::Call(RegistryGetHk);
    void* found = trampoline(node, id);
    const uint32_t n = s_getCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (found != nullptr)
    {
        s_getHit.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        s_getMiss.fetch_add(1, std::memory_order_relaxed);
    }
    if (id == kSentinel)
    {
        s_bbdGet.fetch_add(1, std::memory_order_relaxed);
    }
    if (!LogOn())
    {
        return found;
    }
    const bool hot = id == kSentinel || found == nullptr;
    if (!hot && n > kGetCap)
    {
        return found;
    }
    uint16_t nodeId = 0;
    uintptr_t child = 0;
    uintptr_t next = 0;
    if (node != nullptr)
    {
        Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x90, &nodeId, sizeof(nodeId));
        Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x70, &child, sizeof(child));
        Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x78, &next, sizeof(next));
    }
    KYBER_LOG(Info, "[AntTree] get n=" << n << " want=0x" << std::hex << id << " hit=" << (found ? "1" : "0") << " node=0x"
                                       << reinterpret_cast<uintptr_t>(node) << " nodeId=0x" << nodeId << " child=0x" << child
                                       << " next=0x" << next << " found=0x" << reinterpret_cast<uintptr_t>(found));
    if (hot || n <= 8)
    {
        LogStack("get");
    }
    return found;
}
} // namespace

void Install()
{
    if (!LogOn())
    {
        KYBER_LOG(Info, "[AntTree] hooks off (set GC_ANT_TREE_LOG=1)");
        return;
    }
    HookManager::CreateHook(HOOK_OFFSET(0x141127000), IdFromObjectHk);
    HookManager::CreateHook(HOOK_OFFSET(0x1411270B0), IdFromTypeHk);
    HookManager::CreateHook(HOOK_OFFSET(0x146A3F960), RegistryGetHk);
    Hook::ApplyQueuedActions();
    KYBER_LOG(Info, "[AntTree] hooked idObj=0x1127000 idType=0x11270b0 get=0x6A3F960");
}

void LogSummary()
{
    KYBER_LOG(Info, "[AntTree] summary idObj=" << s_idObjCount.load() << " bbdObj=" << s_bbdFromObj.load()
                                              << " idType=" << s_idTypeCount.load() << " bbdType=" << s_bbdFromType.load()
                                              << " get=" << s_getCount.load() << " hit=" << s_getHit.load()
                                              << " miss=" << s_getMiss.load() << " bbdGet=" << s_bbdGet.load());
}
} // namespace AntTreeTrace
} // namespace Kyber
