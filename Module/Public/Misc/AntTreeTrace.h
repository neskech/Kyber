// Hooks ANT id assignment and registry get(); crash dump walks the hierarchy tree.
#pragma once

#include <cstdint>

namespace Kyber
{
namespace AntTreeTrace
{
// Installs id/get hooks when GC_ANT_TREE_LOG=1.
void Install();

// Counts and unique ids seen this run (crash-handler summary).
void LogSummary();
} // namespace AntTreeTrace
} // namespace Kyber
