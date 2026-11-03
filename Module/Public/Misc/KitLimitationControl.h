// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Read access to the GC kit-limitation bridge's tracked reinforcement counts.

#pragma once

#include <cstdint>

namespace Kyber
{
namespace KitLimitationControl
{
// Current and starting count for a KitLimitation kit id (CharacterId); false when GCR1 does not track it.
bool CountFor(uint32_t kitId, int& cur, int& start);
} // namespace KitLimitationControl
} // namespace Kyber
