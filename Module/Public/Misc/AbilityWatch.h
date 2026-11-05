// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Research probe: guard-page watch on star card modifier fields, to find the code that reads ability levels and unlock gates.

#pragma once

namespace Kyber
{
namespace AbilityWatch
{
// With GC_ABILITY_LEVEL_PROBE=1, hooks the modifier level-select function and logs each (modifier, level) it applies.
// Guards Survivalist's two modifiers once (gated by GC_ABILITY_WATCH_GATE="<mod0 id>,<mod1 id>" when set); later calls log new hits. Runs only when GC_ABILITY_WATCH=1.
void Poll();
} // namespace AbilityWatch
} // namespace Kyber
