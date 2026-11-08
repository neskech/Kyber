// Logs ANT compact-id assignment and substitutes a safe node for registry id 0xBBD.

#include <Misc/AntTreeTrace.h>

#include <Base/Log.h>
#include <Hook/HookManager.h>
#include <Misc/ReflectDump.h>
#include <SDK/Funcs.h>
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
std::atomic<uint32_t> s_fallbackCount{ 0 };
std::atomic<uint32_t> s_vehSkip{ 0 };
std::atomic<uint32_t> s_lookupAnt{ 0 };
std::atomic<uint32_t> s_ac4Count{ 0 };
std::atomic<uint32_t> s_ad39Count{ 0 };
std::atomic<uint32_t> s_secondCount{ 0 };
std::atomic<void*> s_fallbackNode{ nullptr };
std::atomic<void*> s_hothAc4This{ nullptr };
std::atomic<void*> s_hothAc4Bank{ nullptr };
std::atomic<uint32_t> s_hothCompartment{ 0 };

constexpr const char* kKaminoAnt = "animations/antanimations/s7_1/levels/kamino_03/kamino_03_win32_antstate";
constexpr const char* kHothAnt = "animations/antanimations/s9_3/hoth_02/hoth_02_win32_antstate";

thread_local int s_getDepth = 0;

// Zeroed stand-in node: +0x48 actors null so 0x5BCA07A takes its skip path.
alignas(16) uint8_t s_dummyNode[0xA0] = {};

bool LogOn()
{
    static const bool on = PlatformUtils::GetEnv("GC_ANT_TREE_LOG", "0") == "1";
    return on;
}

bool FallbackOn()
{
    static const bool on = PlatformUtils::GetEnv("GC_ANT_FALLBACK", "0") == "1";
    return on;
}

bool SecondOn()
{
    static const bool on = PlatformUtils::GetEnv("GC_ANT_SECOND_BANK", "0") == "1";
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

// True when node+0x48 is a non-null readable pointer.
bool HasActors(void* node, uintptr_t* actorsOut)
{
    uintptr_t actors = 0;
    if (node == nullptr || !Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x48, &actors, sizeof(actors)))
    {
        return false;
    }
    if (actorsOut)
    {
        *actorsOut = actors;
    }
    return actors != 0;
}

// Remember the first tree node with a valid actor table.
void ConsiderFallback(void* node)
{
    if (node == nullptr || s_fallbackNode.load(std::memory_order_relaxed) != nullptr)
    {
        return;
    }
    if (HasActors(node, nullptr))
    {
        s_fallbackNode.store(node, std::memory_order_relaxed);
    }
}

// BFS from root for a node whose +0x48 actor table is non-null.
void* PickFromTree(void* root)
{
    void* q[64] = {};
    int qh = 0;
    int qt = 0;
    if (root != nullptr)
    {
        q[qt++] = root;
    }
    while (qh < qt)
    {
        void* n = q[qh++];
        if (HasActors(n, nullptr))
        {
            return n;
        }
        void* child = nullptr;
        void* next = nullptr;
        Reflect::SafeRead(reinterpret_cast<uint8_t*>(n) + 0x70, &child, sizeof(child));
        Reflect::SafeRead(reinterpret_cast<uint8_t*>(n) + 0x78, &next, sizeof(next));
        if (child && qt < 64)
        {
            q[qt++] = child;
        }
        if (next && qt < 64)
        {
            q[qt++] = next;
        }
    }
    return nullptr;
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

// Live bytes at an absolute VA; follow a relative JMP so we never hook a thunk body.
void LogAbs(const char* tag, uintptr_t abs)
{
    uint8_t b[16] = {};
    if (!Reflect::SafeRead(reinterpret_cast<const uint8_t*>(abs), b, sizeof(b)))
    {
        KYBER_LOG(Info, "[AntTree] " << tag << " abs=0x" << std::hex << abs << " unreadable");
        return;
    }
    std::ostringstream oss;
    oss << "[AntTree] " << tag << " abs=0x" << std::hex << abs << " bytes";
    for (uint8_t v : b)
    {
        oss << " " << static_cast<unsigned>(v);
    }
    KYBER_LOG(Info, oss.str());
    if (b[0] == 0xE9)
    {
        const int32_t rel = static_cast<int32_t>(static_cast<uint32_t>(b[1]) | (static_cast<uint32_t>(b[2]) << 8)
                                                | (static_cast<uint32_t>(b[3]) << 16) | (static_cast<uint32_t>(b[4]) << 24));
        const uintptr_t tgt = abs + 5 + static_cast<intptr_t>(rel);
        KYBER_LOG(Info, "[AntTree] " << tag << " jmp exe+0x" << std::hex << (tgt - PlatformUtils::BaseAddress()));
        LogAbs("jmp-tgt", tgt);
    }
}

void* __fastcall RegistryGetHk(void* node, uint16_t id)
{
    static auto trampoline = HookManager::Call(RegistryGetHk);
    ++s_getDepth;
    void* found = trampoline(node, id);
    ConsiderFallback(node);
    ConsiderFallback(found);
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

    // Node 0x9's actor table was not harmless (crash exe+0x21C9335 on kit lookup).
    // Dummy +0x48=0: affector caller skips; kit caller hits ActorLookupHk null-table.
    const bool outerMiss = found == nullptr && id == kSentinel && s_getDepth == 1 && FallbackOn();
    if (outerMiss)
    {
        void* sub = s_dummyNode;
        s_fallbackCount.fetch_add(1, std::memory_order_relaxed);
        if (s_fallbackCount.load(std::memory_order_relaxed) <= 8)
        {
            KYBER_LOG(Info, "[AntTree] fallback n=" << n << " want=0x" << std::hex << id << " dummy=0x"
                                                    << reinterpret_cast<uintptr_t>(sub));
        }
        found = sub;
    }

    if (LogOn())
    {
        const bool hot = id == kSentinel || found == nullptr || outerMiss;
        if (hot || n <= kGetCap)
        {
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
                                               << " next=0x" << next << " found=0x" << reinterpret_cast<uintptr_t>(found)
                                               << " depth=" << s_getDepth);
            if (hot || n <= 8)
            {
                LogStack("get");
            }
        }
    }
    --s_getDepth;
    return found;
}

// exe+0x9ACE9E0 thunk: mov rdx,[rcx+0x10]; mov rcx,[rcx+0x08]; jmp. rcx=0 is j1a4 crash.
void* __fastcall VehCustHk(void* node, void* a2, void* a3, void* a4)
{
    static auto trampoline = HookManager::Call(VehCustHk);
    if (node == nullptr)
    {
        s_vehSkip.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    uintptr_t inner = 0;
    uintptr_t child = 0;
    Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x08, &inner, sizeof(inner));
    Reflect::SafeRead(reinterpret_cast<uint8_t*>(node) + 0x10, &child, sizeof(child));
    if (inner == 0 || child == 0)
    {
        s_vehSkip.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return trampoline(node, a2, a3, a4);
}

bool NameHasAntstate(const char* name)
{
    return name != nullptr && (std::strstr(name, "antstate") != nullptr || std::strstr(name, "AntState") != nullptr);
}

void* FnBegin(uintptr_t abs)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(abs, &image, nullptr);
    if (rf == nullptr)
    {
        KYBER_LOG(Info, "[AntTree] pdata miss exe+0x" << std::hex << (abs - PlatformUtils::BaseAddress()));
        return reinterpret_cast<void*>(abs);
    }
    void* begin = reinterpret_cast<void*>(image + rf->BeginAddress);
    KYBER_LOG(Info, "[AntTree] pdata exe+0x" << std::hex << (abs - PlatformUtils::BaseAddress()) << " begin=exe+0x"
                                             << rf->BeginAddress << " end=exe+0x" << rf->EndAddress);
    return begin;
}

void* FindKaminoDc()
{
    const char* names[2] = { kKaminoAnt, "kamino_03_win32_antstate" };
    for (uint32_t c = 0; c <= 8; ++c)
    {
        for (const char* name : names)
        {
            DataContainer* dc = ResourceManager_lookupDataContainer(static_cast<ResourceCompartment>(c), name);
            if (dc != nullptr)
            {
                KYBER_LOG(Info, "[AntTree] kamino dc=0x" << std::hex << reinterpret_cast<uintptr_t>(dc) << " cmp=0x" << c
                                                         << " name=" << name);
                return dc;
            }
        }
    }
    return nullptr;
}

void TrySecondBank()
{
    if (!SecondOn() || s_secondCount.load(std::memory_order_relaxed) != 0)
    {
        return;
    }
    s_secondCount.store(1, std::memory_order_relaxed);
    const uintptr_t exe = PlatformUtils::BaseAddress();
    void* loader = nullptr;
    Reflect::SafeRead(reinterpret_cast<const uint8_t*>(exe + 0x3DF2E98), &loader, sizeof(loader));
    void* kamino = FindKaminoDc();
    KYBER_LOG(Info, "[AntTree] second loader=0x" << std::hex << reinterpret_cast<uintptr_t>(loader) << " kamino=0x"
                                                 << reinterpret_cast<uintptr_t>(kamino));
    if (loader == nullptr || kamino == nullptr)
    {
        return;
    }
    using Ac4Fn = void*(__fastcall*)(void*, void*, void*, void*);
    auto ac4 = reinterpret_cast<Ac4Fn>(exe + 0xAC4AE0);
    ac4(loader, kamino, nullptr, nullptr);
    s_secondCount.store(2, std::memory_order_relaxed);
    KYBER_LOG(Info, "[AntTree] second ac4 raw done");
}

void* __fastcall LoadAntHk(void* a1, void* a2, void* a3, void* a4)
{
    static auto trampoline = HookManager::Call(LoadAntHk);
    void* ret = trampoline(a1, a2, a3, a4);
    const uint32_t n = s_ad39Count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12)
    {
        KYBER_LOG(Info, "[AntTree] loadAnt n=" << n << " a1=0x" << std::hex << reinterpret_cast<uintptr_t>(a1) << " a2=0x"
                                               << reinterpret_cast<uintptr_t>(a2) << " a3=0x" << reinterpret_cast<uintptr_t>(a3));
    }
    TrySecondBank();
    return ret;
}
} // namespace

void Install()
{
    const bool log = LogOn();
    const bool fb = FallbackOn();
    const bool second = SecondOn();
    if (!log && !fb && !second)
    {
        KYBER_LOG(Info, "[AntTree] hooks off (set GC_ANT_TREE_LOG=1 or GC_ANT_SECOND_BANK=1)");
        return;
    }
    if (log || fb)
    {
        HookManager::CreateHook(HOOK_OFFSET(0x141127000), IdFromObjectHk);
        HookManager::CreateHook(HOOK_OFFSET(0x1411270B0), IdFromTypeHk);
        HookManager::CreateHook(HOOK_OFFSET(0x146A3F960), RegistryGetHk);
    }
    if (fb)
    {
        HookManager::CreateHook(HOOK_OFFSET(0x149ACE9E0), VehCustHk);
    }
    if (log || second)
    {
        const uintptr_t exe = PlatformUtils::BaseAddress();
        FnBegin(exe + 0x65FD3F8);
        FnBegin(exe + 0x6A0863E);
        FnBegin(exe + 0xAC4AE0);
        FnBegin(exe + 0xAD39A0);
        void* loadFn = FnBegin(exe + 0x65FD3F8);
        HookManager::CreateHook(loadFn, LoadAntHk);
    }
    Hook::ApplyQueuedActions();
    KYBER_LOG(Info, "[AntTree] hooked log=" << (log ? "1" : "0") << " fallback=" << (fb ? "1" : "0")
                                            << " second=" << (second ? "1" : "0"));
}

void LogSummary()
{
    KYBER_LOG(Info, "[AntTree] summary idObj=" << s_idObjCount.load() << " bbdObj=" << s_bbdFromObj.load()
                                              << " idType=" << s_idTypeCount.load() << " bbdType=" << s_bbdFromType.load()
                                              << " get=" << s_getCount.load() << " hit=" << s_getHit.load()
                                              << " miss=" << s_getMiss.load() << " bbdGet=" << s_bbdGet.load()
                                              << " fallback=" << s_fallbackCount.load() << " vehSkip=" << s_vehSkip.load()
                                              << " lookupAnt=" << s_lookupAnt.load() << " ac4=" << s_ac4Count.load()
                                              << " ad39=" << s_ad39Count.load() << " second=" << s_secondCount.load()
                                              << " fbNode=0x" << std::hex
                                              << reinterpret_cast<uintptr_t>(s_fallbackNode.load()));
}
} // namespace AntTreeTrace
} // namespace Kyber
