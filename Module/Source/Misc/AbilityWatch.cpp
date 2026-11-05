// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Research probe: guard-page watch on star card modifier fields, logging each reading instruction with registers and stack.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Hook/HookManager.h>
#include <Misc/AbilityWatch.h>
#include <Misc/ReflectDump.h>
#include <SDK/Funcs.h>

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Kyber
{
namespace AbilityWatch
{
namespace
{
using namespace Reflect;

constexpr int kSlots = 64;
constexpr int kFrames = 20;
constexpr uint32_t kMaxHits = 50000;

// First hit of one instruction that touched a watched field.
struct Hit
{
    std::atomic<uint64_t> rip{ 0 };
    std::atomic<uint32_t> count{ 0 };
    std::atomic<bool> ready{ false };
    uint32_t dr = 0;
    uint32_t tid = 0;
    uint64_t regs[16] = {};
    void* frames[kFrames] = {};
    uint16_t frameCount = 0;
    uint32_t logged = 0;
};

Hit s_hits[kSlots];
std::atomic<uint32_t> s_total{ 0 };
uint64_t s_watch[4] = {};
std::string s_labels[4];
bool s_armed = false;

thread_local uint64_t t_rearmPage = 0;

// Page base of an address.
uint64_t PageOf(uint64_t address)
{
    return address & ~0xFFFull;
}

// True when a page holds one of the watched fields.
bool Watched(uint64_t page)
{
    for (uint64_t field : s_watch)
    {
        if (field != 0 && PageOf(field) == page)
        {
            return true;
        }
    }
    return false;
}

// Adds PAGE_GUARD to a watched page.
void Guard(uint64_t page)
{
    MEMORY_BASIC_INFORMATION mbi{};
    DWORD old = 0;
    if (VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) != 0)
    {
        VirtualProtect(reinterpret_cast<void*>(page), 0x1000, (mbi.Protect & ~PAGE_GUARD) | PAGE_GUARD, &old);
    }
}

// Records the instruction behind a watched field access; re-guards the page after one single step.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    CONTEXT* ctx = info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_rearmPage != 0)
    {
        if (s_total.load() <= kMaxHits)
        {
            Guard(t_rearmPage);
        }
        t_rearmPage = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != STATUS_GUARD_PAGE_VIOLATION || info->ExceptionRecord->NumberParameters < 2)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint64_t address = info->ExceptionRecord->ExceptionInformation[1];
    if (!Watched(PageOf(address)))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    t_rearmPage = PageOf(address);
    ctx->EFlags |= 0x100;
    uint32_t dr = 4;
    for (uint32_t i = 0; i < 4; i++)
    {
        if (s_watch[i] != 0 && address >= s_watch[i] && address < s_watch[i] + 4)
        {
            dr = i;
        }
    }
    if (dr == 4 || s_total.fetch_add(1) > kMaxHits)
    {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint64_t rip = ctx->Rip;
    for (Hit& hit : s_hits)
    {
        uint64_t current = hit.rip.load();
        if (current == rip)
        {
            hit.count.fetch_add(1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (current == 0 && hit.rip.compare_exchange_strong(current, rip))
        {
            hit.dr = dr;
            hit.tid = GetCurrentThreadId();
            const DWORD64 regs[16] = { ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp,
                                       ctx->R8,  ctx->R9,  ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15 };
            std::memcpy(hit.regs, regs, sizeof(regs));
            hit.frameCount = RtlCaptureStackBackTrace(0, kFrames, hit.frames, nullptr);
            hit.count.fetch_add(1);
            hit.ready.store(true);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Address as module+offset text.
std::string Where(const void* address)
{
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) ||
        GetModuleFileNameA(module, path, MAX_PATH) == 0)
    {
        std::ostringstream out;
        out << "0x" << std::hex << reinterpret_cast<uintptr_t>(address);
        return out.str();
    }
    const char* name = std::strrchr(path, '\\');
    std::ostringstream out;
    out << (name != nullptr ? name + 1 : path) << "+0x" << std::hex << (reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
    return out.str();
}

// Element pointer i of a class-pointer array field, or nullptr.
const uint8_t* ArrayItem(const uint8_t* object, const TypeInfo* type, const char* field, uint32_t index, uint32_t* size = nullptr)
{
    const FieldInfoData* info = FindField(type, field);
    const uint8_t* data = nullptr;
    uint32_t count = 0;
    if (info == nullptr || !ReadArray(object + info->fieldOffset, data, count) || index >= count)
    {
        return nullptr;
    }
    if (size != nullptr)
    {
        *size = count;
    }
    const uint8_t* item = nullptr;
    SafeRead(data + index * sizeof(void*), &item, sizeof(item));
    return item;
}

// TypeInfo stored in an object's header.
const TypeInfo* ObjectType(const uint8_t* object)
{
    const TypeInfo* type = nullptr;
    SafeRead(object + sizeof(void*), &type, sizeof(type));
    return ReadableType(type) ? type : nullptr;
}

// Address of a named field, or 0.
uint64_t FieldAddress(const uint8_t* object, const TypeInfo* type, const char* field)
{
    const FieldInfoData* info = type != nullptr ? FindField(type, field) : nullptr;
    return info != nullptr ? reinterpret_cast<uint64_t>(object + info->fieldOffset) : 0;
}

// Fills two watch slots (modifier's RequiredUnlockId, its first value's MinAbilityLevel); sets the gate when gate != 0; logs its level table.
bool Resolve(const char* path, uint32_t index, int slot, uint32_t gate)
{
    DataContainer* asset = ResourceManagerLookupDataContainer(path);
    if (asset == nullptr)
    {
        return false;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(asset);
    uint32_t modifierCount = 0;
    const uint8_t* modifier = ArrayItem(base, asset->getType(), "AbilityModifiers", index, &modifierCount);
    const TypeInfo* modifierType = modifier != nullptr ? ObjectType(modifier) : nullptr;
    uint32_t valueCount = 0;
    const uint8_t* value = modifierType != nullptr ? ArrayItem(modifier, modifierType, "Values", 0, &valueCount) : nullptr;
    const TypeInfo* valueType = value != nullptr ? ObjectType(value) : nullptr;
    s_watch[slot] = FieldAddress(modifier, modifierType, "RequiredUnlockId");
    s_watch[slot + 1] = FieldAddress(value, valueType, "MinAbilityLevel");
    if (gate != 0 && s_watch[slot] != 0)
    {
        *reinterpret_cast<uint32_t*>(s_watch[slot]) = gate;
    }
    const std::string mod = " mod" + std::to_string(index);
    s_labels[slot] = std::string(path) + mod + ".RequiredUnlockId";
    s_labels[slot + 1] = std::string(path) + mod + ".val0.MinAbilityLevel";
    std::ostringstream line;
    line << "[GCWatch] asset " << path << " type=" << TypeName(asset->getType()) << " mods=" << modifierCount << mod << "="
         << (modifierType != nullptr ? DumpFields(modifier, modifierType, 0) : "?") << " values=" << valueCount;
    for (uint32_t i = 0; modifierType != nullptr && i < valueCount; i++)
    {
        const uint8_t* item = ArrayItem(modifier, modifierType, "Values", i);
        const TypeInfo* itemType = item != nullptr ? ObjectType(item) : nullptr;
        line << " v" << i << "=" << (itemType != nullptr ? DumpFields(item, itemType, 0) : "?");
    }
    line << " watch=0x" << std::hex << s_watch[slot] << ",0x" << s_watch[slot + 1];
    KYBER_LOG(Info, line.str());
    return s_watch[slot] != 0 && s_watch[slot + 1] != 0;
}

// Writes the live bytes of [address - 0x10000, address + 0x10000) to %TEMP%\gcwatch\<base>.bin for raw-binary disassembly.
void DumpWindow(uint64_t address)
{
    const uint64_t base = (address & ~0xFFFull) - 0x10000;
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    const std::string dir = std::string(temp) + "gcwatch";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::ostringstream name;
    name << dir << "\\" << std::hex << base << ".bin";
    std::vector<uint8_t> bytes(0x20000, 0xCC);
    for (uint64_t offset = 0; offset < bytes.size(); offset += 0x1000)
    {
        SafeRead(reinterpret_cast<const void*>(base + offset), bytes.data() + offset, 0x1000);
    }
    if (FILE* file = std::fopen(name.str().c_str(), "wb"))
    {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        KYBER_LOG(Info, "[GCWatch] dumped 0x" << std::hex << base << " to " << name.str());
    }
}

// Dumps code windows around a hit and its first game-code callers.
void DumpCode(const Hit& hit)
{
    DumpWindow(hit.rip.load());
    int callers = 0;
    for (uint16_t i = 0; i < hit.frameCount && callers < 3; i++)
    {
        const uint64_t frame = reinterpret_cast<uint64_t>(hit.frames[i]);
        if (frame >= 0x140000000 && frame < 0x150000000 && frame != hit.rip.load())
        {
            DumpWindow(frame);
            callers++;
        }
    }
}

// Logs hits not yet logged and updated counts.
void LogHits()
{
    for (Hit& hit : s_hits)
    {
        if (!hit.ready.load())
        {
            continue;
        }
        const uint32_t count = hit.count.load();
        if (hit.logged == 0)
        {
            std::ostringstream line;
            line << "[GCWatch] hit field=" << s_labels[hit.dr] << " rip=" << Where(reinterpret_cast<void*>(hit.rip.load())) << " tid=" << hit.tid
                 << " count=" << count << " regs=" << std::hex;
            const char* names[16] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
            for (int i = 0; i < 16; i++)
            {
                line << " " << names[i] << "=" << hit.regs[i];
            }
            line << " stack=";
            for (uint16_t i = 0; i < hit.frameCount; i++)
            {
                line << " " << Where(hit.frames[i]);
            }
            KYBER_LOG(Info, line.str());
            DumpCode(hit);
        }
        else if (count != hit.logged)
        {
            KYBER_LOG(Info, "[GCWatch] count rip=" << Where(reinterpret_cast<void*>(hit.rip.load())) << " count=" << count);
        }
        hit.logged = count;
    }
}
std::mutex s_levelMutex;

// Modifier level-select (picks the Values entry whose level range holds the level, then applies it).
constexpr uint64_t kLevelSelect = 0x145EF4A40;
std::set<std::pair<uint64_t, int32_t>> s_selectSeen;

// Logs each new (modifier, level) pair the game applies.
bool SelectHk(void* modifier, int32_t level, void* a3, void* a4)
{
    static auto trampoline = HookManager::Call(SelectHk);
    const bool applied = trampoline(modifier, level, a3, a4);
    std::lock_guard<std::mutex> lock(s_levelMutex);
    if (s_selectSeen.size() < 300 && s_selectSeen.insert({ reinterpret_cast<uint64_t>(modifier), level }).second)
    {
        const uint8_t* m = static_cast<const uint8_t*>(modifier);
        const TypeInfo* type = m != nullptr ? ObjectType(m) : nullptr;
        const uint64_t hashField = type != nullptr ? FieldAddress(m, type, "OutputHash") : 0;
        uint32_t hash = 0;
        if (hashField != 0)
        {
            SafeRead(reinterpret_cast<const void*>(hashField), &hash, sizeof(hash));
        }
        KYBER_LOG(Info, "[GCLevel] select modifier=0x" << std::hex << reinterpret_cast<uint64_t>(modifier) << " hash=0x" << hash << std::dec
                                                       << " level=" << level << " applied=" << applied << " tid=" << GetCurrentThreadId());
    }
    return applied;
}

// Hooks the modifier level-select function once to log applied levels.
void HookLevelSelect()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;
    HookManager::CreateHook(reinterpret_cast<Hook::Address>(kLevelSelect), SelectHk);
    Hook::ApplyQueuedActions();
    KYBER_LOG(Info, "[GCLevel] level-select hooked");
}
} // namespace

void Poll()
{
    const char* levelProbe = std::getenv("GC_ABILITY_LEVEL_PROBE");
    if (levelProbe != nullptr && std::strcmp(levelProbe, "1") == 0)
    {
        HookLevelSelect();
    }
    const char* flag = std::getenv("GC_ABILITY_WATCH");
    if (flag == nullptr || std::strcmp(flag, "1") != 0)
    {
        return;
    }
    static const uint64_t firstPoll = GetTickCount64();
    static uint64_t lastPoll = 0;
    const uint64_t now = GetTickCount64();
    if (now - firstPoll < 30000 || now - lastPoll < 10000)
    {
        return;
    }
    lastPoll = now;
    if (s_armed)
    {
        LogHits();
        return;
    }
    uint32_t gates[2] = {};
    if (const char* list = std::getenv("GC_ABILITY_WATCH_GATE"))
    {
        std::sscanf(list, "%u,%u", &gates[0], &gates[1]);
    }
    const char* survivalist = "Gameplay/Equipment/Abilities/Ability_Survivalist/SC_Assault_Troopers_09_Survivalist";
    if (!Resolve(survivalist, 0, 0, gates[0]) || !Resolve(survivalist, 1, 2, gates[1]))
    {
        KYBER_LOG(Info, "[GCWatch] assets not resolved yet");
        return;
    }
    AddVectoredExceptionHandler(1, OnException);
    for (uint64_t field : s_watch)
    {
        Guard(PageOf(field));
    }
    s_armed = true;
    KYBER_LOG(Info, "[GCWatch] guarded pages 0x" << std::hex << PageOf(s_watch[0]) << " 0x" << PageOf(s_watch[1]) << " 0x" << PageOf(s_watch[2])
                                                 << " 0x" << PageOf(s_watch[3]));
}
} // namespace AbilityWatch
} // namespace Kyber
