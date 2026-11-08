// Crash-time Zydis of decrypted game code: RIP window, fault operand, GPR dumps, optional image dump.

#include <Misc/CrashDisasm.h>

#include <Base/Log.h>
#include <Misc/ReflectDump.h>
#include <Utilities/PlatformUtils.h>

#include <Zydis/Zydis.h>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace Kyber
{
namespace
{
constexpr int kBefore = 24;
constexpr int kAfter = 8;
constexpr SIZE_T kWholeFuncMax = 400;

// Copies committed readable bytes; never throws.
static bool ReadMem(const void* src, void* dst, SIZE_T n)
{
    if (src == nullptr || dst == nullptr || n == 0)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(src, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
    {
        return false;
    }
    const DWORD protect = info.Protect & 0xff;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE)
    {
        return false;
    }
    __try
    {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// True when GC_* env is "1".
static bool EnvOn(const char* name)
{
    return PlatformUtils::GetEnv(name, "0") == "1";
}

// Labels a pointer: null, low, heap-fill, or live.
static const char* PtrKind(uintptr_t p)
{
    if (p == 0)
    {
        return "null";
    }
    if (p < 0x10000)
    {
        return "low";
    }
    if (p == 0xFEEEFEEEFEEEFEEEull || p == 0xFEEEFEEEull)
    {
        return "freed-FEEE";
    }
    if (p == 0xDDDDDDDDDDDDDDDDull || p == 0xDDDDDDDDull)
    {
        return "freed-DD";
    }
    if (p == 0xCDCDCDCDCDCDCDCDull || p == 0xCDCDCDCDull)
    {
        return "uninit-CD";
    }
    if (p == 0xABABABABABABABABull || p == 0xABABABABull)
    {
        return "guard-AB";
    }
    return ReadMem(reinterpret_cast<const void*>(p), &p, 1) ? "readable" : "unreadable";
}

// Fills Zydis register context from the crash CONTEXT.
static void FillRegs(const CONTEXT* ctx, ZydisRegisterContext* out)
{
    memset(out, 0, sizeof(*out));
#ifdef _M_X64
    out->values[ZYDIS_REGISTER_RAX] = ctx->Rax;
    out->values[ZYDIS_REGISTER_RBX] = ctx->Rbx;
    out->values[ZYDIS_REGISTER_RCX] = ctx->Rcx;
    out->values[ZYDIS_REGISTER_RDX] = ctx->Rdx;
    out->values[ZYDIS_REGISTER_RSI] = ctx->Rsi;
    out->values[ZYDIS_REGISTER_RDI] = ctx->Rdi;
    out->values[ZYDIS_REGISTER_RBP] = ctx->Rbp;
    out->values[ZYDIS_REGISTER_RSP] = ctx->Rsp;
    out->values[ZYDIS_REGISTER_RIP] = ctx->Rip;
    out->values[ZYDIS_REGISTER_R8] = ctx->R8;
    out->values[ZYDIS_REGISTER_R9] = ctx->R9;
    out->values[ZYDIS_REGISTER_R10] = ctx->R10;
    out->values[ZYDIS_REGISTER_R11] = ctx->R11;
    out->values[ZYDIS_REGISTER_R12] = ctx->R12;
    out->values[ZYDIS_REGISTER_R13] = ctx->R13;
    out->values[ZYDIS_REGISTER_R14] = ctx->R14;
    out->values[ZYDIS_REGISTER_R15] = ctx->R15;
#endif
}

// Looks up a GPR value from CONTEXT by Zydis register id.
static uintptr_t GprValue(const CONTEXT* ctx, ZydisRegister reg)
{
#ifdef _M_X64
    switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg))
    {
    case ZYDIS_REGISTER_RAX:
        return static_cast<uintptr_t>(ctx->Rax);
    case ZYDIS_REGISTER_RBX:
        return static_cast<uintptr_t>(ctx->Rbx);
    case ZYDIS_REGISTER_RCX:
        return static_cast<uintptr_t>(ctx->Rcx);
    case ZYDIS_REGISTER_RDX:
        return static_cast<uintptr_t>(ctx->Rdx);
    case ZYDIS_REGISTER_RSI:
        return static_cast<uintptr_t>(ctx->Rsi);
    case ZYDIS_REGISTER_RDI:
        return static_cast<uintptr_t>(ctx->Rdi);
    case ZYDIS_REGISTER_RBP:
        return static_cast<uintptr_t>(ctx->Rbp);
    case ZYDIS_REGISTER_RSP:
        return static_cast<uintptr_t>(ctx->Rsp);
    case ZYDIS_REGISTER_R8:
        return static_cast<uintptr_t>(ctx->R8);
    case ZYDIS_REGISTER_R9:
        return static_cast<uintptr_t>(ctx->R9);
    case ZYDIS_REGISTER_R10:
        return static_cast<uintptr_t>(ctx->R10);
    case ZYDIS_REGISTER_R11:
        return static_cast<uintptr_t>(ctx->R11);
    case ZYDIS_REGISTER_R12:
        return static_cast<uintptr_t>(ctx->R12);
    case ZYDIS_REGISTER_R13:
        return static_cast<uintptr_t>(ctx->R13);
    case ZYDIS_REGISTER_R14:
        return static_cast<uintptr_t>(ctx->R14);
    case ZYDIS_REGISTER_R15:
        return static_cast<uintptr_t>(ctx->R15);
    default:
        return 0;
    }
#else
    (void)ctx;
    (void)reg;
    return 0;
#endif
}

// Resolves .pdata RUNTIME_FUNCTION covering pc; begin/end are absolute.
static bool LookupFn(uintptr_t pc, uintptr_t* begin, uintptr_t* end, uintptr_t* imageBase)
{
    DWORD64 base = 0;
    const PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &base, nullptr);
    if (rf == nullptr || base == 0)
    {
        return false;
    }
    *imageBase = static_cast<uintptr_t>(base);
    *begin = static_cast<uintptr_t>(base + rf->BeginAddress);
    *end = static_cast<uintptr_t>(base + rf->EndAddress);
    return true;
}

// Hex+ASCII plus a nearby C-string for one live pointer.
static void DumpPtr(const char* label, uintptr_t p)
{
    KYBER_LOG(Info, label << " 0x" << std::hex << p << " " << PtrKind(p));
    if (p < 0x10000)
    {
        return;
    }
    unsigned char buf[48] = {};
    if (!ReadMem(reinterpret_cast<const void*>(p), buf, sizeof(buf)))
    {
        return;
    }
    std::ostringstream hex;
    hex << label << " bytes:";
    for (size_t i = 0; i < sizeof(buf); ++i)
    {
        hex << " " << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(buf[i]);
    }
    hex << "  |";
    for (size_t i = 0; i < sizeof(buf); ++i)
    {
        const char c = static_cast<char>(buf[i]);
        hex << (c >= 32 && c < 127 ? c : '.');
    }
    hex << "|";
    KYBER_LOG(Info, hex.str());
    const std::string s = Reflect::ReadCString(reinterpret_cast<const char*>(p));
    if (!s.empty() && s[0] != '?' && s.find_first_not_of("?") != std::string::npos)
    {
        KYBER_LOG(Info, label << " cstr=" << s);
    }
    uintptr_t typeAddr = 0;
    if (ReadMem(reinterpret_cast<const void*>(p + sizeof(void*)), &typeAddr, sizeof(typeAddr)))
    {
        const TypeInfo* type = reinterpret_cast<const TypeInfo*>(typeAddr);
        if (Reflect::ReadableType(type))
        {
            KYBER_LOG(Info, label << " fbType=" << Reflect::TypeName(type));
            const FieldInfoData* nameField = Reflect::FindField(type, "Name");
            if (nameField != nullptr)
            {
                const char* namePtr = nullptr;
                if (ReadMem(reinterpret_cast<const void*>(p + nameField->fieldOffset), &namePtr, sizeof(namePtr)))
                {
                    KYBER_LOG(Info, label << " assetName=" << Reflect::ReadCString(namePtr));
                }
            }
        }
    }
}

// Logs TypeInfo field names at +0x00..+0x80 and each pointer's fbType/assetName.
static void DumpFbSlots(const char* label, uintptr_t p)
{
    if (p < 0x10000)
    {
        return;
    }
    uintptr_t typeAddr = 0;
    if (!ReadMem(reinterpret_cast<const void*>(p + sizeof(void*)), &typeAddr, sizeof(typeAddr)))
    {
        return;
    }
    const TypeInfo* type = reinterpret_cast<const TypeInfo*>(typeAddr);
    if (!Reflect::ReadableType(type))
    {
        return;
    }
    KYBER_LOG(Info, label << " fbSlots type=" << Reflect::TypeName(type));
    const auto fields = Reflect::AllFields(type);
    for (uint16_t off = 0; off <= 0x80; off += 8)
    {
        uintptr_t val = 0;
        if (!ReadMem(reinterpret_cast<const void*>(p + off), &val, sizeof(val)))
        {
            break;
        }
        const char* fname = "";
        for (const FieldInfoData* f : fields)
        {
            if (f != nullptr && f->fieldOffset == off && f->name != nullptr)
            {
                fname = f->name;
                break;
            }
        }
        const char* kind = PtrKind(val);
        KYBER_LOG(Info, label << " +" << std::hex << off << " " << fname << "=0x" << val << " " << kind);
        if (off >= 0x10 && val >= 0x10000 && val < 0x00007FFFFFFFull)
        {
            char child[64];
            std::snprintf(child, sizeof(child), "%s.%s", label, fname[0] ? fname : "ptr");
            DumpPtr(child, val);
        }
    }
    for (const FieldInfoData* f : fields)
    {
        if (f == nullptr || f->name == nullptr || f->fieldOffset > 0xC0)
        {
            continue;
        }
        uintptr_t val = 0;
        if (!ReadMem(reinterpret_cast<const void*>(p + f->fieldOffset), &val, sizeof(val)))
        {
            continue;
        }
        KYBER_LOG(Info, label << " field " << f->name << " off=0x" << std::hex << f->fieldOffset << " val=0x" << val
                              << " " << PtrKind(val));
    }
}

// Decodes and logs instructions in [from, to) marking RIP.
static void DisasmRange(ZydisDecoder* decoder, ZydisFormatter* formatter, uintptr_t from, uintptr_t to,
    uintptr_t rip, int markBefore, int markAfter)
{
    if (to <= from || to - from > 4096)
    {
        KYBER_LOG(Info, "disasm skip range 0x" << std::hex << from << "-0x" << to);
        return;
    }
    unsigned char buf[4096] = {};
    const SIZE_T n = static_cast<SIZE_T>(to - from);
    if (!ReadMem(reinterpret_cast<const void*>(from), buf, n))
    {
        KYBER_LOG(Info, "disasm unreadable 0x" << std::hex << from);
        return;
    }
    struct Row
    {
        uintptr_t addr;
        char text[160];
    };
    Row rows[512] = {};
    int count = 0;
    ZyanUSize offset = 0;
    while (offset < n && count < 512)
    {
        ZydisDecodedInstruction insn = {};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(decoder, buf + offset, n - offset, &insn, ops)))
        {
            break;
        }
        rows[count].addr = from + offset;
        ZydisFormatterFormatInstruction(formatter, &insn, ops, insn.operand_count_visible, rows[count].text,
            sizeof(rows[count].text), rows[count].addr, ZYAN_NULL);
        ++count;
        offset += insn.length;
        if (from + offset > to)
        {
            break;
        }
    }
    int ripIdx = -1;
    for (int i = 0; i < count; ++i)
    {
        if (rows[i].addr == rip)
        {
            ripIdx = i;
            break;
        }
    }
    int start = 0;
    int stop = count;
    if (ripIdx >= 0)
    {
        start = ripIdx > markBefore ? ripIdx - markBefore : 0;
        stop = ripIdx + 1 + markAfter;
        if (stop > count)
        {
            stop = count;
        }
    }
    KYBER_LOG(Info, "disasm count=" << count << " ripIdx=" << ripIdx);
    if (ripIdx < 0 && count > 0)
    {
        KYBER_LOG(Info, "disasm unsynced; skip junk window");
        return;
    }
    const uintptr_t exe = PlatformUtils::BaseAddress();
    for (int i = start; i < stop; ++i)
    {
        const char* mark = rows[i].addr == rip ? ">>" : "  ";
        KYBER_LOG(Info, mark << " exe+0x" << std::hex << (rows[i].addr - exe) << "  " << rows[i].text);
    }
}

// Describes the memory operand that matches the AV fault address.
static void PrintFaultOperand(ZydisDecoder* decoder, ZydisFormatter* formatter, const CONTEXT* ctx, uintptr_t rip,
    uintptr_t fault)
{
    unsigned char buf[16] = {};
    if (!ReadMem(reinterpret_cast<const void*>(rip), buf, sizeof(buf)))
    {
        KYBER_LOG(Info, "fault-op: RIP unreadable");
        return;
    }
    ZydisDecodedInstruction insn = {};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT] = {};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(decoder, buf, sizeof(buf), &insn, ops)))
    {
        KYBER_LOG(Info, "fault-op: decode failed");
        return;
    }
    char text[160] = {};
    ZydisFormatterFormatInstruction(formatter, &insn, ops, insn.operand_count_visible, text, sizeof(text), rip, ZYAN_NULL);
    KYBER_LOG(Info, "fault-insn " << text);
    ZydisRegisterContext regs = {};
    FillRegs(ctx, &regs);
    bool hit = false;
    for (ZyanU8 i = 0; i < insn.operand_count; ++i)
    {
        if (ops[i].type != ZYDIS_OPERAND_TYPE_MEMORY)
        {
            continue;
        }
        ZyanU64 ea = 0;
        const ZyanStatus st = ZydisCalcAbsoluteAddressEx(&insn, &ops[i], rip, &regs, &ea);
        const ZydisRegister base = ops[i].mem.base;
        const ZydisRegister index = ops[i].mem.index;
        const uintptr_t baseVal = GprValue(ctx, base);
        const uintptr_t indexVal = GprValue(ctx, index);
        const char* baseName = ZydisRegisterGetString(base);
        const char* indexName = ZydisRegisterGetString(index);
        KYBER_LOG(Info, "mem-op[" << static_cast<int>(i) << "] ea=0x" << std::hex << ea << " ok=" << ZYAN_SUCCESS(st)
                                  << " base=" << (baseName ? baseName : "?") << "=0x" << baseVal << " "
                                  << PtrKind(baseVal) << " index=" << (indexName ? indexName : "?") << "=0x" << indexVal
                                  << " scale=" << static_cast<int>(ops[i].mem.scale) << " disp=0x" << ops[i].mem.disp.value);
        if (ZYAN_SUCCESS(st) && (ea == fault || (fault >= ea && fault < ea + 16) || (ea >= fault && ea < fault + 16)))
        {
            hit = true;
            KYBER_LOG(Info, "fault-match operand[" << static_cast<int>(i) << "] ea=0x" << std::hex << ea << " fault=0x"
                                                   << fault);
            if (baseVal < 0x10000)
            {
                KYBER_LOG(Info, "fault-cause base " << (baseName ? baseName : "?") << " is " << PtrKind(baseVal)
                                                    << " (null-page + disp)");
            }
        }
    }
    if (!hit)
    {
        KYBER_LOG(Info, "fault-op: no mem operand matched fault 0x" << std::hex << fault);
    }
}

// Logs qwords at p+0 .. p+nBytes for a native object (AssetBank this).
static void DumpQwords(const char* label, uintptr_t p, int nBytes)
{
    if (p < 0x10000 || nBytes <= 0 || nBytes > 0x200)
    {
        return;
    }
    for (int off = 0; off < nBytes; off += 8)
    {
        uintptr_t val = 0;
        if (!ReadMem(reinterpret_cast<const void*>(p + static_cast<uintptr_t>(off)), &val, sizeof(val)))
        {
            break;
        }
        KYBER_LOG(Info, label << "+0x" << std::hex << off << " =0x" << val << " " << PtrKind(val));
    }
}

// Logs count u32s at p (hash/index table).
static void DumpU32Table(const char* label, uintptr_t p, int count)
{
    if (p < 0x10000 || count <= 0 || count > 64)
    {
        return;
    }
    int nff = 0;
    for (int i = 0; i < count; ++i)
    {
        uint32_t v = 0;
        if (!ReadMem(reinterpret_cast<const void*>(p + static_cast<uintptr_t>(i) * 4), &v, sizeof(v)))
        {
            KYBER_LOG(Info, label << "[" << i << "] unreadable");
            break;
        }
        if (v == 0xFFFFFFFFu)
        {
            ++nff;
        }
        if (i < 24 || v != 0xFFFFFFFFu)
        {
            KYBER_LOG(Info, label << "[" << i << "]=0x" << std::hex << v);
        }
    }
    KYBER_LOG(Info, label << " first" << count << " minus1=" << nff);
}

// Disassembles each CALL target in [fnBegin, fnEnd).
static void DumpCallTargets(ZydisDecoder* decoder, ZydisFormatter* formatter, uintptr_t fnBegin, uintptr_t fnEnd)
{
    if (fnEnd <= fnBegin || fnEnd - fnBegin > 4096)
    {
        return;
    }
    unsigned char buf[4096] = {};
    const SIZE_T n = static_cast<SIZE_T>(fnEnd - fnBegin);
    if (!ReadMem(reinterpret_cast<const void*>(fnBegin), buf, n))
    {
        return;
    }
    const uintptr_t exe = PlatformUtils::BaseAddress();
    ZyanUSize offset = 0;
    int dumped = 0;
    while (offset < n && dumped < 6)
    {
        ZydisDecodedInstruction insn = {};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(decoder, buf + offset, n - offset, &insn, ops)))
        {
            break;
        }
        if (insn.mnemonic == ZYDIS_MNEMONIC_CALL)
        {
            ZyanU64 abs = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &ops[0], fnBegin + offset, &abs)))
            {
                uintptr_t begin = 0;
                uintptr_t end = 0;
                uintptr_t img = 0;
                KYBER_LOG(Info, "call-tgt exe+0x" << std::hex << (abs - exe) << " from exe+0x"
                                                  << (fnBegin + offset - exe));
                uintptr_t dumpAt = static_cast<uintptr_t>(abs);
                unsigned char stub[16] = {};
                if (ReadMem(reinterpret_cast<const void*>(dumpAt), stub, sizeof(stub)))
                {
                    ZydisDecodedInstruction jn = {};
                    ZydisDecodedOperand jops[ZYDIS_MAX_OPERAND_COUNT] = {};
                    if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(decoder, stub, sizeof(stub), &jn, jops))
                        && jn.mnemonic == ZYDIS_MNEMONIC_JMP)
                    {
                        ZyanU64 jabs = 0;
                        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&jn, &jops[0], dumpAt, &jabs)))
                        {
                            KYBER_LOG(Info, "call-tgt thunk jmp exe+0x" << std::hex << (jabs - exe));
                            dumpAt = static_cast<uintptr_t>(jabs);
                        }
                    }
                }
                if (LookupFn(dumpAt, &begin, &end, &img) && end > begin)
                {
                    KYBER_LOG(Info, "call-tgt pdata [exe+0x" << (begin - exe) << ",exe+0x" << (end - exe)
                                                             << ") size=" << std::dec << (end - begin));
                    uintptr_t to = end;
                    if (to - begin > 768)
                    {
                        to = begin + 768;
                    }
                    DisasmRange(decoder, formatter, begin, to, dumpAt, 0, 768);
                }
                else
                {
                    DisasmRange(decoder, formatter, dumpAt, dumpAt + 256, dumpAt, 0, 64);
                }
                ++dumped;
            }
        }
        offset += insn.length;
    }
}

// Prints 16 stack qwords as exe-relative PCs with .pdata function bounds.
static void PrintFnStack(CONTEXT* ctx)
{
#ifdef _M_X64
    const uintptr_t exe = PlatformUtils::BaseAddress();
    const uintptr_t rsp = static_cast<uintptr_t>(ctx->Rsp);
    KYBER_LOG(Info, "fn-stack exeBase=0x" << std::hex << exe);
    int printed = 0;
    for (int i = 0; i < 48 && printed < 16; ++i)
    {
        uintptr_t pc = 0;
        if (!ReadMem(reinterpret_cast<const void*>(rsp + static_cast<uintptr_t>(i) * sizeof(uintptr_t)), &pc, sizeof(pc)))
        {
            break;
        }
        if (pc < exe || pc > exe + 0x20000000ull)
        {
            continue;
        }
        uintptr_t begin = 0;
        uintptr_t end = 0;
        uintptr_t img = 0;
        const bool ok = LookupFn(pc, &begin, &end, &img);
        std::ostringstream oss;
        oss << "fn#" << printed << " slot=" << i << " pc=0x" << std::hex << pc << " exe+0x" << (pc - exe);
        if (ok)
        {
            oss << " fn=[exe+0x" << (begin - exe) << ",exe+0x" << (end - exe) << ")";
        }
        else
        {
            oss << " fn=none";
        }
        KYBER_LOG(Info, oss.str());
        ++printed;
    }
#else
    (void)ctx;
#endif
}

// Writes one range of decrypted exe bytes to %APPDATA%\ArmchairDevelopers\Kyber\Logs\<name>.
static void WriteDumpRange(const wchar_t* appdata, const char* name, uintptr_t start, DWORD size)
{
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\ArmchairDevelopers\\Kyber\\Logs\\%S", appdata, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        KYBER_LOG(Info, "image-dump: create failed " << name);
        return;
    }
    unsigned char chunk[1 << 16] = {};
    DWORD writtenAll = 0;
    for (DWORD off = 0; off < size; off += sizeof(chunk))
    {
        const DWORD n = (size - off) < sizeof(chunk) ? (size - off) : sizeof(chunk);
        if (!ReadMem(reinterpret_cast<const void*>(start + off), chunk, n))
        {
            memset(chunk, 0, n);
        }
        DWORD w = 0;
        WriteFile(file, chunk, n, &w, nullptr);
        writtenAll += w;
    }
    CloseHandle(file);
    const uintptr_t exe = PlatformUtils::BaseAddress();
    KYBER_LOG(Info, "image-dump " << name << " bytes=" << writtenAll << " va=exe+0x" << std::hex << (start - exe));
}

// Walks AntStateHierarchyNode from root: +0x70 child, +0x78 next, +0x90 id, +0x48 actors.
static void DumpAntTree(uintptr_t root)
{
    if (root < 0x10000)
    {
        KYBER_LOG(Info, "ant-tree skip root=0x" << std::hex << root);
        return;
    }
    uintptr_t queue[256] = {};
    int qt = 0;
    int qh = 0;
    queue[qt++] = root;
    int n = 0;
    int bbd = 0;
    int ids[32] = {};
    int idn = 0;
    while (qh < qt && n < 200)
    {
        const uintptr_t p = queue[qh++];
        uint16_t id = 0;
        uintptr_t child = 0;
        uintptr_t next = 0;
        uintptr_t actors = 0;
        if (!ReadMem(reinterpret_cast<const void*>(p + 0x90), &id, sizeof(id)))
        {
            KYBER_LOG(Info, "ant-tree unreadable n=" << n << " p=0x" << std::hex << p);
            break;
        }
        ReadMem(reinterpret_cast<const void*>(p + 0x70), &child, sizeof(child));
        ReadMem(reinterpret_cast<const void*>(p + 0x78), &next, sizeof(next));
        ReadMem(reinterpret_cast<const void*>(p + 0x48), &actors, sizeof(actors));
        KYBER_LOG(Info, "ant-tree n=" << n << " p=0x" << std::hex << p << " id=0x" << id << " actors=0x" << actors
                                      << " child=0x" << child << " next=0x" << next);
        if (id == 0xBBD)
        {
            ++bbd;
        }
        if (idn < 32)
        {
            bool seen = false;
            for (int i = 0; i < idn; ++i)
            {
                if (ids[i] == static_cast<int>(id))
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                ids[idn++] = static_cast<int>(id);
            }
        }
        if (child >= 0x10000 && qt < 256)
        {
            queue[qt++] = child;
        }
        if (next >= 0x10000 && qt < 256)
        {
            queue[qt++] = next;
        }
        ++n;
    }
    std::ostringstream oss;
    oss << "ant-tree summary nodes=" << n << " bbd=" << bbd << " uniqueIds=" << idn << " ids";
    for (int i = 0; i < idn; ++i)
    {
        oss << " 0x" << std::hex << ids[i];
    }
    KYBER_LOG(Info, oss.str());
}

// Zydis a known RVA; follow a leading JMP thunk (CE7640 / BD1270).
static void DumpRvaFn(ZydisDecoder* decoder, ZydisFormatter* formatter, const char* name, uintptr_t rva, size_t n)
{
    const uintptr_t exe = PlatformUtils::BaseAddress();
    uintptr_t p = exe + rva;
    unsigned char stub[16] = {};
    if (ReadMem(reinterpret_cast<const void*>(p), stub, sizeof(stub)))
    {
        ZydisDecodedInstruction jn = {};
        ZydisDecodedOperand jops[ZYDIS_MAX_OPERAND_COUNT] = {};
        if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(decoder, stub, sizeof(stub), &jn, jops))
            && jn.mnemonic == ZYDIS_MNEMONIC_JMP)
        {
            ZyanU64 jabs = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&jn, &jops[0], p, &jabs)))
            {
                KYBER_LOG(Info, "dump-fn " << name << " exe+0x" << std::hex << rva << " thunk jmp exe+0x"
                                           << (static_cast<uintptr_t>(jabs) - exe));
                p = static_cast<uintptr_t>(jabs);
            }
        }
    }
    uintptr_t begin = 0;
    uintptr_t end = 0;
    uintptr_t img = 0;
    KYBER_LOG(Info, "dump-fn " << name << " at exe+0x" << std::hex << (p - exe));
    uintptr_t from = p;
    uintptr_t to = p + n;
    if (LookupFn(p, &begin, &end, &img) && end > begin)
    {
        KYBER_LOG(Info, "dump-fn pdata [exe+0x" << std::hex << (begin - exe) << ",exe+0x" << (end - exe) << ") size="
                                                << std::dec << (end - begin));
        from = begin;
        to = (end - begin > 768) ? begin + 768 : end;
    }
    DisasmRange(decoder, formatter, from, to, from, 400, 400);
    DumpCallTargets(decoder, formatter, from, to);
}

// Writes decrypted .text/.rdata plus an 8MB ANT-window dump when GC_DUMP_IMAGE=1.
static void DumpImage()
{
    if (!EnvOn("GC_DUMP_IMAGE"))
    {
        return;
    }
    const uintptr_t exe = PlatformUtils::BaseAddress();
    IMAGE_DOS_HEADER dos = {};
    if (!ReadMem(reinterpret_cast<const void*>(exe), &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
    {
        KYBER_LOG(Info, "image-dump: bad DOS");
        return;
    }
    IMAGE_NT_HEADERS64 nt = {};
    if (!ReadMem(reinterpret_cast<const void*>(exe + dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
    {
        KYBER_LOG(Info, "image-dump: bad NT");
        return;
    }
    KYBER_LOG(Info, "image-dump SizeOfImage=0x" << std::hex << nt.OptionalHeader.SizeOfImage
                                                << " sections=" << std::dec << nt.FileHeader.NumberOfSections
                                                << " base=0x" << std::hex << exe);
    const uintptr_t secAddr = exe + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    IMAGE_SECTION_HEADER secs[16] = {};
    const unsigned nsec = nt.FileHeader.NumberOfSections < 16 ? nt.FileHeader.NumberOfSections : 16;
    if (!ReadMem(reinterpret_cast<const void*>(secAddr), secs, nsec * sizeof(IMAGE_SECTION_HEADER)))
    {
        KYBER_LOG(Info, "image-dump: sections unreadable");
        return;
    }
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0)
    {
        return;
    }
    for (unsigned i = 0; i < nsec; ++i)
    {
        char name[9] = {};
        memcpy(name, secs[i].Name, 8);
        KYBER_LOG(Info, "image-sec " << name << " va=exe+0x" << std::hex << secs[i].VirtualAddress
                                     << " vsize=0x" << secs[i].Misc.VirtualSize << " raw=0x" << secs[i].SizeOfRawData);
        if (strcmp(name, ".text") != 0 && strcmp(name, ".rdata") != 0)
        {
            continue;
        }
        char fileName[24] = {};
        std::snprintf(fileName, sizeof(fileName), "exe-%s.bin", name[0] == '.' ? name + 1 : name);
        WriteDumpRange(appdata, fileName, exe + secs[i].VirtualAddress, secs[i].Misc.VirtualSize);
    }
    // 0xBD1270, 0xCE7640, 0x1127000 live here; PE .text is a 57KB stub at 0x482f000.
    WriteDumpRange(appdata, "exe-ant.bin", exe + 0xB00000u, 0x800000u);
}

} // namespace

void PrintCrashDisasm(EXCEPTION_POINTERS* exceptionInfo)
{
    if (exceptionInfo == nullptr || exceptionInfo->ContextRecord == nullptr || exceptionInfo->ExceptionRecord == nullptr)
    {
        return;
    }
    CONTEXT* ctx = exceptionInfo->ContextRecord;
#ifdef _M_X64
    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    const uintptr_t exe = PlatformUtils::BaseAddress();
    KYBER_LOG(Info, "----- CRASH DISASM -----");
    char gpr[384];
    std::snprintf(gpr, sizeof(gpr), "RIP abs=0x%llx exe+0x%llx base=0x%llx",
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(rip >= exe ? rip - exe : 0),
        static_cast<unsigned long long>(exe));
    KYBER_LOG(Info, gpr);
    std::snprintf(gpr, sizeof(gpr), "RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx",
        static_cast<unsigned long long>(ctx->Rax), static_cast<unsigned long long>(ctx->Rbx),
        static_cast<unsigned long long>(ctx->Rcx), static_cast<unsigned long long>(ctx->Rdx));
    KYBER_LOG(Info, gpr);
    std::snprintf(gpr, sizeof(gpr), "RSI=0x%llx RDI=0x%llx RBP=0x%llx RSP=0x%llx",
        static_cast<unsigned long long>(ctx->Rsi), static_cast<unsigned long long>(ctx->Rdi),
        static_cast<unsigned long long>(ctx->Rbp), static_cast<unsigned long long>(ctx->Rsp));
    KYBER_LOG(Info, gpr);
    std::snprintf(gpr, sizeof(gpr), "R8=0x%llx R9=0x%llx R10=0x%llx R11=0x%llx",
        static_cast<unsigned long long>(ctx->R8), static_cast<unsigned long long>(ctx->R9),
        static_cast<unsigned long long>(ctx->R10), static_cast<unsigned long long>(ctx->R11));
    KYBER_LOG(Info, gpr);
    std::snprintf(gpr, sizeof(gpr), "R12=0x%llx R13=0x%llx R14=0x%llx R15=0x%llx",
        static_cast<unsigned long long>(ctx->R12), static_cast<unsigned long long>(ctx->R13),
        static_cast<unsigned long long>(ctx->R14), static_cast<unsigned long long>(ctx->R15));
    KYBER_LOG(Info, gpr);
    uintptr_t fault = 0;
    if (exceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
        && exceptionInfo->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR kind = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
        fault = static_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionInformation[1]);
        KYBER_LOG(Info, "AV " << (kind == 0 ? "read" : kind == 1 ? "write" : "exec") << " fault=0x" << std::hex << fault
                              << " " << PtrKind(fault));
    }
    uintptr_t fnBegin = 0;
    uintptr_t fnEnd = 0;
    uintptr_t fnImg = 0;
    if (LookupFn(rip, &fnBegin, &fnEnd, &fnImg))
    {
        KYBER_LOG(Info, "pdata fn=[exe+0x" << std::hex << (fnBegin - exe) << ",exe+0x" << (fnEnd - exe) << ") size="
                                           << std::dec << (fnEnd - fnBegin));
    }
    else
    {
        KYBER_LOG(Info, "pdata fn=none");
    }
    ZydisDecoder decoder;
    ZydisFormatter formatter;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))
        || !ZYAN_SUCCESS(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)))
    {
        KYBER_LOG(Info, "zydis init failed");
        return;
    }
    PrintFaultOperand(&decoder, &formatter, ctx, rip, fault);
    if (fnBegin && fnEnd > fnBegin && (fnEnd - fnBegin) <= kWholeFuncMax)
    {
        DisasmRange(&decoder, &formatter, fnBegin, fnEnd, rip, 256, 256);
    }
    else
    {
        uintptr_t from = rip > 96 ? rip - 96 : rip;
        if (fnBegin && from < fnBegin)
        {
            from = fnBegin;
        }
        uintptr_t to = rip + 64;
        if (fnEnd && to > fnEnd)
        {
            to = fnEnd;
        }
        bool synced = false;
        for (uintptr_t tryFrom = from; tryFrom < rip && tryFrom < from + 16; ++tryFrom)
        {
            unsigned char probe[16] = {};
            uintptr_t pc = tryFrom;
            bool hit = false;
            for (int i = 0; i < 48 && pc <= rip; ++i)
            {
                if (!ReadMem(reinterpret_cast<const void*>(pc), probe, sizeof(probe)))
                {
                    break;
                }
                ZydisDecodedInstruction insn = {};
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT] = {};
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, probe, sizeof(probe), &insn, ops)))
                {
                    break;
                }
                if (pc == rip)
                {
                    hit = true;
                    break;
                }
                pc += insn.length;
            }
            if (hit)
            {
                from = tryFrom;
                synced = true;
                break;
            }
        }
        if (!synced)
        {
            from = rip;
        }
        KYBER_LOG(Info, "disasm window exe+0x" << std::hex << (from - exe) << " .. exe+0x" << (to - exe)
                                               << (synced ? " synced" : " from-RIP"));
        DisasmRange(&decoder, &formatter, from, to, rip, kBefore, kAfter);
    }
    DumpQwords("r15reg", static_cast<uintptr_t>(ctx->R15), 0xA0);
    DumpAntTree(static_cast<uintptr_t>(ctx->R15));
    DumpRvaFn(&decoder, &formatter, "idTypeCaller", 0xB89ADB, 256);
    DumpRvaFn(&decoder, &formatter, "idTypeParent", 0x66CCC11, 256);
    DumpRvaFn(&decoder, &formatter, "idTypeP2", 0xAC4BC0, 256);
    DumpRvaFn(&decoder, &formatter, "idTypeP3", 0xAD3ACE, 256);
    DumpImage();
    DumpPtr("RAX", static_cast<uintptr_t>(ctx->Rax));
    DumpFbSlots("RAX", static_cast<uintptr_t>(ctx->Rax));
    DumpPtr("RBX", static_cast<uintptr_t>(ctx->Rbx));
    DumpPtr("RCX", static_cast<uintptr_t>(ctx->Rcx));
    DumpPtr("RDX", static_cast<uintptr_t>(ctx->Rdx));
    DumpPtr("RSI", static_cast<uintptr_t>(ctx->Rsi));
    DumpFbSlots("RSI", static_cast<uintptr_t>(ctx->Rsi));
    DumpPtr("RDI", static_cast<uintptr_t>(ctx->Rdi));
    DumpPtr("RBP", static_cast<uintptr_t>(ctx->Rbp));
    DumpQwords("this", static_cast<uintptr_t>(ctx->Rdi), 0x100);
    {
        uintptr_t t98 = 0;
        uintptr_t tD8 = 0;
        uintptr_t tF8 = 0;
        const uintptr_t self = static_cast<uintptr_t>(ctx->Rdi);
        ReadMem(reinterpret_cast<const void*>(self + 0x98), &t98, sizeof(t98));
        ReadMem(reinterpret_cast<const void*>(self + 0xD8), &tD8, sizeof(tD8));
        ReadMem(reinterpret_cast<const void*>(self + 0xF8), &tF8, sizeof(tF8));
        KYBER_LOG(Info, "this+0x98=0x" << std::hex << t98 << " +0xD8=0x" << tD8 << " +0xF8=0x" << tF8);
        DumpU32Table("idxD8", tD8, 32);
        DumpQwords("tab98", t98, 0x40);
        DumpQwords("tabF8", tF8, 0x40);
    }
    if (fnBegin && fnEnd > fnBegin)
    {
        DumpCallTargets(&decoder, &formatter, fnBegin, fnEnd);
    }
    DumpPtr("R8", static_cast<uintptr_t>(ctx->R8));
    DumpPtr("R9", static_cast<uintptr_t>(ctx->R9));
    DumpFbSlots("R9", static_cast<uintptr_t>(ctx->R9));
    DumpPtr("R12", static_cast<uintptr_t>(ctx->R12));
    DumpPtr("R13", static_cast<uintptr_t>(ctx->R13));
    DumpPtr("R14", static_cast<uintptr_t>(ctx->R14));
    DumpPtr("R15", static_cast<uintptr_t>(ctx->R15));
    // Home space / extra args of the crashing frame (x64 RCX/RDX/R8/R9 spilled here).
    for (int off = 0x08; off <= 0x50; off += 8)
    {
        uintptr_t slot = 0;
        char lab[32];
        std::snprintf(lab, sizeof(lab), "stack+0x%x", off);
        if (ReadMem(reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx->Rsp) + off), &slot, sizeof(slot)))
        {
            DumpPtr(lab, slot);
            DumpFbSlots(lab, slot);
        }
    }
    PrintFnStack(ctx);
    uintptr_t ret0 = 0;
    if (ReadMem(reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx->Rsp)), &ret0, sizeof(ret0)))
    {
        uintptr_t cb = 0;
        uintptr_t ce = 0;
        uintptr_t ci = 0;
        if (LookupFn(ret0, &cb, &ce, &ci) && ce > cb)
        {
            KYBER_LOG(Info, "caller-fn exe+0x" << std::hex << (cb - exe) << " .. exe+0x" << (ce - exe)
                                               << " ret=exe+0x" << (ret0 - exe));
            uintptr_t cfrom = ret0 > 80 ? ret0 - 80 : cb;
            if (cfrom < cb)
            {
                cfrom = cb;
            }
            uintptr_t cto = ret0 + 24;
            if (cto > ce)
            {
                cto = ce;
            }
            DisasmRange(&decoder, &formatter, cfrom, cto, ret0, 16, 4);
        }
    }
    KYBER_LOG(Info, "----- CRASH DISASM END -----");
#else
    (void)ctx;
#endif
}
} // namespace Kyber
