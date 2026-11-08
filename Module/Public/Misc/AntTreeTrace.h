// ANT id/get hooks plus hardware-BP capture of the bank constructor.
#pragma once

#include <cstdint>

namespace Kyber
{
namespace AntTreeTrace
{
// Arms VEH+DR BPs when GC_ANT_SECOND_BANK=1; id/get when log/fallback.
void Install();

// Re-arms DRx and retries the second AC4AE0 from a game thread.
void Poll();

// Counts seen this run (crash-handler summary).
void LogSummary();
} // namespace AntTreeTrace
} // namespace Kyber
