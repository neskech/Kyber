// Hooks ANT id/get, optional 0xBBD fallback, and AntStateAsset bank construction.
#pragma once

#include <cstdint>

namespace Kyber
{
namespace AntTreeTrace
{
// Installs id/get hooks when GC_ANT_TREE_LOG=1 or GC_ANT_FALLBACK=1.
void Install();

// Counts and unique ids seen this run (crash-handler summary).
void LogSummary();
} // namespace AntTreeTrace
} // namespace Kyber
