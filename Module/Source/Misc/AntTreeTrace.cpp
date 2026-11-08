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
#include <thread>

#include <tlhelp32.h>
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

std::atomic<void*> s_capThis{ nullptr };
std::atomic<void*> s_capRdx{ nullptr };
std::atomic<void*> s_capR8{ nullptr };
std::atomic<void*> s_capR9{ nullptr };
std::atomic<uintptr_t> s_capRet{ 0 };
std::atomic<DWORD> s_capTid{ 0 };
std::atomic<uint32_t> s_loggedHits{ 0 };
std::atomic<int> s_inSecond{ 0 };
std::atomic<int> s_stopArm{ 0 };
PVOID s_veh = nullptr;
uintptr_t s_ac4Abs = 0;
uintptr_t s_callAbs = 0;

struct Hit
{
    uintptr_t rip = 0;
    uintptr_t rcx = 0;
    uintptr_t rdx = 0;
    uintptr_t r8 = 0;
    uintptr_t r9 = 0;
    uintptr_t rsp = 0;
    uintptr_t ret = 0;
    uintptr_t stack[6] = {};
    DWORD tid = 0;
};

constexpr uint32_t kHitCap = 24;
Hit s_hits[kHitCap] = {};
std::atomic<uint32_t> s_hitNext{ 0 };

// Writes one captured BP hit for Poll to print.
void StoreHit(const CONTEXT* ctx, uintptr_t rip)
{
    const uint32_t i = s_hitNext.fetch_add(1, std::memory_order_relaxed);
    if (i >= kHitCap)
    {
        return;
    }
    Hit& h = s_hits[i];
    h.rip = rip;
    h.rcx = static_cast<uintptr_t>(ctx->Rcx);
    h.rdx = static_cast<uintptr_t>(ctx->Rdx);
    h.r8 = static_cast<uintptr_t>(ctx->R8);
    h.r9 = static_cast<uintptr_t>(ctx->R9);
    h.rsp = static_cast<uintptr_t>(ctx->Rsp);
    h.tid = GetCurrentThreadId();
    Reflect::SafeRead(reinterpret_cast<const uint8_t*>(ctx->Rsp), &h.ret, sizeof(h.ret));
    for (int s = 0; s < 6; ++s)
    {
        Reflect::SafeRead(reinterpret_cast<const uint8_t*>(ctx->Rsp) + static_cast<uintptr_t>(s) * 8, &h.stack[s], sizeof(h.stack[s]));
    }
}

// Prints one stored hit with DC names when readable.
void LogHit(const Hit& h, uint32_t n)
{
    const uintptr_t exe = PlatformUtils::BaseAddress();
    const char* typeName = "-";
    char asset[96] = {};
    DescribeDc(reinterpret_cast<void*>(h.rdx), typeName, asset, sizeof(asset));
    const char* thisType = "-";
    char thisAsset[96] = {};
    DescribeDc(reinterpret_cast<void*>(h.rcx), thisType, thisAsset, sizeof(thisAsset));
    KYBER_LOG(Info, "[AntTree] hw n=" << n << " rip=exe+0x" << std::hex << (h.rip >= exe ? h.rip - exe : h.rip) << " tid=" << std::dec
                                      << h.tid << " rcx=0x" << std::hex << h.rcx << " rcxType=" << thisType << " rdx=0x" << h.rdx
                                      << " rdxType=" << typeName << " asset=" << (asset[0] ? asset : "-") << " r8=0x" << h.r8 << " r9=0x"
                                      << h.r9 << " rsp=0x" << h.rsp << " ret=exe+0x" << (h.ret >= exe ? h.ret - exe : h.ret));
}

// Applies DR0=AC4AE0 and DR1=call-site execute BPs on every thread of this process.
void ArmAllThreads()
{
    if (s_ac4Abs == 0)
    {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return;
    }
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
            {
                continue;
            }
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE,
                te.th32ThreadID);
            if (th == nullptr)
            {
                continue;
            }
            if (te.th32ThreadID != self)
            {
                SuspendThread(th);
            }
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx))
            {
                ctx.Dr0 = s_ac4Abs;
                ctx.Dr1 = s_callAbs;
                ctx.Dr7 = 0xF;
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(th, &ctx);
            }
            if (te.th32ThreadID != self)
            {
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// Re-applies DRx until the second call finishes or the budget expires.
void ArmerLoop()
{
    for (int i = 0; i < 450 && s_stopArm.load(std::memory_order_relaxed) == 0; ++i)
    {
        ArmAllThreads();
        Sleep(200);
    }
}

using Ac4Fn = void*(__fastcall*)(void*, void*, void*, void*);

// SEH wrapper: no C++ objects (C2712).
void* CallAc4(Ac4Fn fn, void* a1, void* a2, void* a3, void* a4, DWORD* seh)
{
    *seh = 0;
    void* ret = nullptr;
    __try
    {
        ret = fn(a1, a2, a3, a4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *seh = GetExceptionCode();
        ret = nullptr;
    }
    return ret;
}

// Looks up Kamino AntStateAsset with no log (safe from VEH).
void* LookupKaminoQuiet()
{
    const char* names[2] = { kKaminoAnt, "kamino_03_win32_antstate" };
    for (uint32_t c = 0; c <= 8; ++c)
    {
        for (const char* name : names)
        {
            DataContainer* dc = ResourceManager_lookupDataContainer(static_cast<ResourceCompartment>(c), name);
            if (dc != nullptr)
            {
                return dc;
            }
        }
    }
    return nullptr;
}

// Calls AC4AE0(captured this, kamino DC, captured r8/r9) once after AC4AE0 returns.
void TrySecondBank()
{
    if (!SecondOn() || s_inSecond.load(std::memory_order_relaxed) != 0)
    {
        return;
    }
    void* loader = s_capThis.load(std::memory_order_relaxed);
    if (loader == nullptr)
    {
        return;
    }
    void* kamino = LookupKaminoQuiet();
    if (kamino == nullptr)
    {
        return;
    }
    uint32_t expected = 0;
    if (!s_secondCount.compare_exchange_strong(expected, 1, std::memory_order_relaxed))
    {
        return;
    }
    s_inSecond.store(1, std::memory_order_relaxed);
    const uintptr_t exe = PlatformUtils::BaseAddress();
    auto ac4 = reinterpret_cast<Ac4Fn>(exe + 0xAC4AE0);
    void* r8 = s_capR8.load(std::memory_order_relaxed);
    void* r9 = s_capR9.load(std::memory_order_relaxed);
    DWORD seh = 0;
    void* ret = CallAc4(ac4, loader, kamino, r8, r9, &seh);
    if (seh != 0)
    {
        s_secondCount.store(3, std::memory_order_relaxed);
    }
    else
    {
        s_secondCount.store(2, std::memory_order_relaxed);
        (void)ret;
    }
    s_inSecond.store(0, std::memory_order_relaxed);
    (void)kamino;
    (void)r8;
    (void)r9;
}

// VEH: execute BP at AC4AE0 entry and at 0x65FD3F8, no instruction patch.
LONG CALLBACK OnHwBp(PEXCEPTION_POINTERS ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t rip = static_cast<uintptr_t>(ep->ContextRecord->Rip);
    const uintptr_t retBp = s_capRet.load(std::memory_order_relaxed);
    if (rip != s_ac4Abs && rip != s_callAbs && !(rip == retBp && retBp != 0))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (s_inSecond.load(std::memory_order_relaxed) != 0)
    {
        ep->ContextRecord->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip == retBp && retBp != 0)
    {
        TrySecondBank();
        ep->ContextRecord->Dr2 = 0;
        ep->ContextRecord->Dr7 &= ~static_cast<DWORD64>(0x30);
        s_capRet.store(0, std::memory_order_relaxed);
        ep->ContextRecord->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    StoreHit(ep->ContextRecord, rip);
    if (rip == s_ac4Abs)
    {
        s_ac4Count.fetch_add(1, std::memory_order_relaxed);
        void* rdx = reinterpret_cast<void*>(ep->ContextRecord->Rdx);
        void* hoth = nullptr;
        for (uint32_t c = 0; c <= 8 && hoth == nullptr; ++c)
        {
            hoth = ResourceManager_lookupDataContainer(static_cast<ResourceCompartment>(c), kHothAnt);
        }
        if (s_capThis.load(std::memory_order_relaxed) == nullptr && hoth != nullptr && rdx == hoth)
        {
            s_capThis.store(reinterpret_cast<void*>(ep->ContextRecord->Rcx), std::memory_order_relaxed);
            s_capRdx.store(rdx, std::memory_order_relaxed);
            s_capR8.store(reinterpret_cast<void*>(ep->ContextRecord->R8), std::memory_order_relaxed);
            s_capR9.store(reinterpret_cast<void*>(ep->ContextRecord->R9), std::memory_order_relaxed);
            uintptr_t ret = 0;
            Reflect::SafeRead(reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rsp), &ret, sizeof(ret));
            s_capRet.store(ret, std::memory_order_relaxed);
            s_capTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
            ep->ContextRecord->Dr2 = ret;
            ep->ContextRecord->Dr7 |= 0x30;
        }
    }
    else if (rip == s_callAbs)
    {
        s_ad39Count.fetch_add(1, std::memory_order_relaxed);
    }
    ep->ContextRecord->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
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
    if (log || fb || second)
    {
        HookManager::CreateHook(HOOK_OFFSET(0x141127000), IdFromObjectHk);
        HookManager::CreateHook(HOOK_OFFSET(0x1411270B0), IdFromTypeHk);
        HookManager::CreateHook(HOOK_OFFSET(0x146A3F960), RegistryGetHk);
    }
    if (fb)
    {
        HookManager::CreateHook(HOOK_OFFSET(0x149ACE9E0), VehCustHk);
    }
    if (log || fb || second)
    {
        Hook::ApplyQueuedActions();
    }
    if (second)
    {
        const uintptr_t exe = PlatformUtils::BaseAddress();
        s_ac4Abs = exe + 0xAC4AE0;
        s_callAbs = exe + 0x65FD3F8;
        s_veh = AddVectoredExceptionHandler(1, OnHwBp);
        ArmAllThreads();
        std::thread(ArmerLoop).detach();
        KYBER_LOG(Info, "[AntTree] hwbp ac4=exe+0xAC4AE0 call=exe+0x65FD3F8 veh=" << (s_veh ? "1" : "0"));
    }
    KYBER_LOG(Info, "[AntTree] hooked log=" << (log ? "1" : "0") << " fallback=" << (fb ? "1" : "0")
                                            << " second=" << (second ? "1" : "0"));
}

void Poll()
{
    if (!SecondOn())
    {
        return;
    }
    const uint32_t next = s_hitNext.load(std::memory_order_relaxed);
    uint32_t logged = s_loggedHits.load(std::memory_order_relaxed);
    while (logged < next && logged < kHitCap)
    {
        LogHit(s_hits[logged], logged + 1);
        logged++;
    }
    s_loggedHits.store(logged, std::memory_order_relaxed);
    static bool loggedSecond = false;
    const uint32_t st = s_secondCount.load(std::memory_order_relaxed);
    if (!loggedSecond && st >= 2)
    {
        loggedSecond = true;
        KYBER_LOG(Info, "[AntTree] second status=" << st << " this=0x" << std::hex
                                                   << reinterpret_cast<uintptr_t>(s_capThis.load()) << " rdx=0x"
                                                   << reinterpret_cast<uintptr_t>(s_capRdx.load()));
    }
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
