// Live Zydis dump of the decrypted exe around a crash RIP.
#pragma once

#include <windows.h>

namespace Kyber
{
// Logs exe-relative RIP, Zydis around RIP, the faulting operand, GPR dumps, and .pdata bounds.
void PrintCrashDisasm(EXCEPTION_POINTERS* exceptionInfo);
} // namespace Kyber
