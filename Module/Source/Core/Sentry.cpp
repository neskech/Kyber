// Copyright Armchair Developers / Sean Kahler. Licensed under GPLv3.

#include <Core/Sentry.h>

#include <Base/Log.h>
#include <Hook/HookManager.h>
#include <Misc/EntityCreateTrace.h>
#include <Misc/ReflectDump.h>
#include <Utilities/PlatformUtils.h>
#include <Utilities/StringUtils.h>

#include <sentry.h>

#include <windows.h>
#include <DbgHelp.h>

#include <cstring>
#include <iomanip>
#include <sstream>

namespace Kyber
{
Sentry::DebugContextData Sentry::s_debugContextData;

class MiniDumpInfo
{
public:
    char pad_0000[16]; // 0x0000
    EXCEPTION_POINTERS* exceptionInfo;
};

const char* ExceptionCodeToString(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    default:
        return "UNKNOWN EXECPTION";
    }
}

// Copies readable process bytes; used to dump the decrypted opcode at RIP.
static bool SafeReadBytes(const void* src, void* dst, SIZE_T n)
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

// Logs a hex dump of live bytes at addr (decrypted by the time we crash).
static void PrintLiveBytes(const char* label, const void* addr, SIZE_T n)
{
    unsigned char buf[64] = {};
    if (n > sizeof(buf))
    {
        n = sizeof(buf);
    }
    if (!SafeReadBytes(addr, buf, n))
    {
        KYBER_LOG(Info, label << " unreadable at 0x" << std::hex << reinterpret_cast<uintptr_t>(addr));
        return;
    }
    std::ostringstream oss;
    oss << label << " @0x" << std::hex << reinterpret_cast<uintptr_t>(addr) << ":";
    for (SIZE_T i = 0; i < n; ++i)
    {
        oss << " " << std::setw(2) << std::setfill('0') << static_cast<int>(buf[i]);
    }
    KYBER_LOG(Info, oss.str());
}

// Logs qwords on the crash stack.
static void PrintStackWords(CONTEXT* ctx, int count)
{
#ifdef _M_X64
    const uintptr_t rsp = static_cast<uintptr_t>(ctx->Rsp);
#else
    const uintptr_t rsp = static_cast<uintptr_t>(ctx->Esp);
#endif
    for (int i = 0; i < count; ++i)
    {
        uintptr_t word = 0;
        const void* slot = reinterpret_cast<const void*>(rsp + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        if (!SafeReadBytes(slot, &word, sizeof(word)))
        {
            KYBER_LOG(Info, "stack[" << i << "] unreadable");
            break;
        }
        KYBER_LOG(Info, "stack[" << i << "] 0x" << std::hex << word);
    }
}

// Writes a small minidump next to the Kyber log.
static void WriteOwnedMiniDump(EXCEPTION_POINTERS* exceptionInfo)
{
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0)
    {
        KYBER_LOG(Info, "minidump skipped: no APPDATA");
        return;
    }
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\ArmchairDevelopers\\Kyber\\Logs\\luke-crash.dmp", appdata);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        KYBER_LOG(Info, "minidump create failed");
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exceptionInfo;
    mei.ClientPointers = FALSE;
    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
        &mei, nullptr, nullptr);
    CloseHandle(file);
    KYBER_LOG(Info, (ok ? "minidump wrote luke-crash.dmp" : "minidump write failed"));
}

void PrintStackTrace(CONTEXT* ctx)
{
    CONTEXT walk = *ctx;
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymInitialize(process, NULL, TRUE);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

    STACKFRAME64 frame = {};
    DWORD machineType;

#ifdef _M_X64
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = walk.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = walk.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walk.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif _M_IX86
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = walk.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = walk.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walk.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    std::cout << "Unsupported architecture\n";
    return;
#endif

    int frameNum = 0;
    while (frameNum < 16 &&
           StackWalk64(machineType, process, thread, &frame, &walk, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
    {
        std::ostringstream oss;

        DWORD64 addr = frame.AddrPC.Offset;

        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        oss << "#" << std::setw(2) << frameNum++ << " ";
        oss << "0x" << std::hex << addr << std::dec << " ";

        IMAGEHLP_MODULE64 moduleInfo = {};
        moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
        if (SymGetModuleInfo64(process, addr, &moduleInfo))
        {
            DWORD64 offset = addr - moduleInfo.BaseOfImage;
            oss << moduleInfo.ModuleName << "+0x" << std::hex << offset << std::dec << " ";
        }

        DWORD64 displacement = 0;
        if (SymFromAddr(process, addr, &displacement, symbol))
        {
            oss << symbol->Name << "+0x" << std::hex << displacement << std::dec;

            IMAGEHLP_LINE64 line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisp = 0;

            if (SymGetLineFromAddr64(process, addr, &lineDisp, &line))
            {
                oss << " at " << line.FileName << ":" << line.LineNumber;
            }

            // Also show module name
            IMAGEHLP_MODULE64 moduleInfo = {};
            moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
            if (SymGetModuleInfo64(process, addr, &moduleInfo))
            {
                oss << " [" << moduleInfo.ModuleName << "]";
            }
        }

        KYBER_LOG(Info, oss.str());
    }

    SymCleanup(process);
}

void PrintRegisters(CONTEXT* ctx)
{

#ifdef _M_X64
    KYBER_LOG(Info, "RAX: 0x" << std::hex << ctx->Rax);
    KYBER_LOG(Info, "RBX: 0x" << std::hex << ctx->Rbx);
    KYBER_LOG(Info, "RCX: 0x" << std::hex << ctx->Rcx);
    KYBER_LOG(Info, "RDX: 0x" << std::hex << ctx->Rdx);
    KYBER_LOG(Info, "RSI: 0x" << std::hex << ctx->Rsi);
    KYBER_LOG(Info, "RDI: 0x" << std::hex << ctx->Rdi);
    KYBER_LOG(Info, "RBP: 0x" << std::hex << ctx->Rbp);
    KYBER_LOG(Info, "RSP: 0x" << std::hex << ctx->Rsp);
    KYBER_LOG(Info, "RIP: 0x" << std::hex << ctx->Rip);
    KYBER_LOG(Info, "R8:  0x" << std::hex << ctx->R8 );
    KYBER_LOG(Info, "R9:  0x" << std::hex << ctx->R9 );
    KYBER_LOG(Info, "R10: 0x" << std::hex << ctx->R10);
    KYBER_LOG(Info, "R11: 0x" << std::hex << ctx->R11);
    KYBER_LOG(Info, "R12: 0x" << std::hex << ctx->R12);
    KYBER_LOG(Info, "R13: 0x" << std::hex << ctx->R13);
    KYBER_LOG(Info, "R14: 0x" << std::hex << ctx->R14);
    KYBER_LOG(Info, "R15: 0x" << std::hex << ctx->R15);
#elif _M_IX86
    KYBER_LOG(Info, "EAX: 0x" << std::hex << ctx->Eax);
    KYBER_LOG(Info, "EBX: 0x" << std::hex << ctx->Ebx);
    KYBER_LOG(Info, "ECX: 0x" << std::hex << ctx->Ecx);
    KYBER_LOG(Info, "EDX: 0x" << std::hex << ctx->Edx);
    KYBER_LOG(Info, "ESI: 0x" << std::hex << ctx->Esi);
    KYBER_LOG(Info, "EDI: 0x" << std::hex << ctx->Edi);
    KYBER_LOG(Info, "EBP: 0x" << std::hex << ctx->Ebp);
    KYBER_LOG(Info, "ESP: 0x" << std::hex << ctx->Esp);
    KYBER_LOG(Info, "EIP: 0x" << std::hex << ctx->Eip);
#endif
}

// Logs TypeInfo name and the +0x20 pointer on a live Frostbite object.
static void PrintObjectType(const char* label, uintptr_t obj)
{
    if (obj == 0)
    {
        KYBER_LOG(Info, label << " null");
        return;
    }
    uintptr_t vtable = 0;
    uintptr_t typeAddr = 0;
    uintptr_t field20 = 0;
    SafeReadBytes(reinterpret_cast<const void*>(obj), &vtable, sizeof(vtable));
    SafeReadBytes(reinterpret_cast<const void*>(obj + sizeof(void*)), &typeAddr, sizeof(typeAddr));
    SafeReadBytes(reinterpret_cast<const void*>(obj + 0x20), &field20, sizeof(field20));
    const TypeInfo* type = reinterpret_cast<const TypeInfo*>(typeAddr);
    const TypeInfo* asType = reinterpret_cast<const TypeInfo*>(obj);
    if (!Reflect::ReadableType(type) && Reflect::ReadableType(asType))
    {
        KYBER_LOG(Info, label << " typeinfo=0x" << std::hex << obj << " name=" << Reflect::TypeName(asType));
        return;
    }
    const char* name = Reflect::ReadableType(type) ? Reflect::TypeName(type) : "?";
    KYBER_LOG(Info, label << " obj=0x" << std::hex << obj << " vtbl=0x" << vtable << " type=0x" << typeAddr
                          << " name=" << name << " +20=0x" << field20);
    if (!Reflect::ReadableType(type))
    {
        return;
    }
    if (field20 != 0)
    {
        uintptr_t childType = 0;
        SafeReadBytes(reinterpret_cast<const void*>(field20 + sizeof(void*)), &childType, sizeof(childType));
        const TypeInfo* ct = reinterpret_cast<const TypeInfo*>(childType);
        KYBER_LOG(Info, label << "+20 name=" << (Reflect::ReadableType(ct) ? Reflect::TypeName(ct) : "?"));
    }
    const FieldInfoData* field = Reflect::FindField(type, "Name");
    if (field != nullptr)
    {
        const char* namePtr = nullptr;
        SafeReadBytes(reinterpret_cast<const void*>(obj + field->fieldOffset), &namePtr, sizeof(namePtr));
        KYBER_LOG(Info, label << " assetName=" << Reflect::ReadCString(namePtr));
    }
}

// Logs crash registers, live opcode bytes, stack words, then a short walk.
void PrintDumpToLog(EXCEPTION_POINTERS* exceptionInfo)
{
    DWORD errorCode = exceptionInfo->ExceptionRecord->ExceptionCode;
    KYBER_LOG(Info, "------------- EXCEPTION DUMP -------------");

    KYBER_LOG(Info, "Exception Code: 0x" << std::hex << errorCode << " (" << ExceptionCodeToString(errorCode) << ")");
    KYBER_LOG(Info, "Exception Flags: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionFlags);
    KYBER_LOG(Info, "Exception Address: 0x" << std::hex << exceptionInfo->ExceptionRecord->ExceptionAddress);
    if (errorCode == EXCEPTION_ACCESS_VIOLATION && exceptionInfo->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR kind = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR fault = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
        const char* verb = kind == 0 ? "read" : kind == 1 ? "write" : "exec";
        KYBER_LOG(Info, "Access " << verb << " of 0x" << std::hex << fault);
    }
    KYBER_LOG(Info, "");
    PrintRegisters(exceptionInfo->ContextRecord);
#ifdef _M_X64
    PrintObjectType("rbx", exceptionInfo->ContextRecord->Rbx);
    PrintObjectType("rdx", exceptionInfo->ContextRecord->Rdx);
    PrintObjectType("rsi", exceptionInfo->ContextRecord->Rsi);
    PrintObjectType("rdi", exceptionInfo->ContextRecord->Rdi);
    PrintObjectType("r12", exceptionInfo->ContextRecord->R12);
    PrintObjectType("r14", exceptionInfo->ContextRecord->R14);
#endif
    PrintLiveBytes("code", exceptionInfo->ExceptionRecord->ExceptionAddress, 64);
    PrintStackWords(exceptionInfo->ContextRecord, 16);
    KYBER_LOG(Info, "");
    PrintStackTrace(exceptionInfo->ContextRecord);
    EntityCreateTrace::LogRecent();
    WriteOwnedMiniDump(exceptionInfo);

    KYBER_LOG(Info, "----------- EXCEPTION DUMP END -----------");
}

void WriteMiniDumpHk(MiniDumpInfo* params)
{
    static const auto trampoline = HookManager::Call(WriteMiniDumpHk);

    KYBER_LOG(Info, "Redirecting crash to Sentry");

    for (const auto& [key, value] : Sentry::s_debugContextData)
    {
        if (key == "ModuleInfo" || key == "MachineUniqueId")
        {
            continue;
        }

        eastl::string sanitizedValue = value.substr(0, 200);
        sanitizedValue.erase(eastl::remove(sanitizedValue.begin(), sanitizedValue.end(), '\n'), sanitizedValue.end());
        sentry_set_tag(key.c_str(), sanitizedValue.c_str());
    }

    sentry_ucontext_t uctx;
    memset(&uctx, 0, sizeof(uctx));
    uctx.exception_ptrs = *params->exceptionInfo;
    sentry_handle_exception(&uctx);

    //if (std::getenv("POD_NAME") != nullptr || SHOULD_LOG(Kyber::LogLevel::Debug))
    // Always do now
    {
        PrintDumpToLog(params->exceptionInfo);
    }

    // For now always create crash dump for testing
    if (PlatformUtils::GetEnv("KYBER_PRESERVE_CRASH_DUMP", "0") == "1")
    {
        trampoline(params);
    }
}

static sentry_value_t OnCrash(const sentry_ucontext_t* uctx, sentry_value_t event, void* closure)
{
    KYBER_LOG(Info, "Crash detected, uploading");
    sentry_uuid_t uuid = sentry_capture_event(event);

    char buf[37];
    sentry_uuid_as_string(&uuid, buf);

    KYBER_LOG(Info, "Crash uploaded " << buf);
    return sentry_value_new_null();
}

void Sentry::Initialize()
{
#if defined(_DEBUG)
    KYBER_LOG(Info, "Sentry disabled in debug builds");
    return;
#endif

    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, "https://0e9433cdc24aa998ae3944ebe424a313@o4510921032859648.ingest.de.sentry.io/4510937325699152");

    sentry_options_set_database_pathw(options, (PlatformUtils::GetProgramDataPath() / "ModuleData/Sentry").c_str());

    std::string moduleVersion = PlatformUtils::GetEnv("KYBER_MODULE_VERSION", "unknown");
    sentry_options_set_release(options, ("kyber-module@" + moduleVersion).c_str());
    sentry_options_set_debug(options, PlatformUtils::GetEnv("KYBER_SENTRY_DEBUG", "0") == "1");
    sentry_options_set_sample_rate(options, 1.0);
    
    sentry_init(options);

    sentry_value_t user = sentry_value_new_object();
    sentry_value_set_by_key(user, "id", sentry_value_new_string(PlatformUtils::GetEnv("EASecureLaunchTokenTemp", "unknown").c_str()));
    sentry_value_set_by_key(user, "username", sentry_value_new_string(PlatformUtils::GetEnv("EALaunchEAID", "unknown").c_str()));
    sentry_set_user(user);

    sentry_options_set_on_crash(options, OnCrash, nullptr);

    if (std::getenv("KYBER_DEV_MODE") == nullptr)
    {
        HookManager::CreateHook(HOOK_OFFSET(0x1401FFC30), WriteMiniDumpHk);
        Hook::ApplyQueuedActions();
    
        KYBER_LOG(Info, "Sentry initialized");
    }
}
} // namespace Kyber
