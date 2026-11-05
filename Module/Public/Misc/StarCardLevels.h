// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Per-team star card levels: a hook on the game's ability-level provider answers star card queries with the team's level.

#include <string>

namespace Kyber
{
namespace StarCardLevels
{
// Sets the 0-based PlayerAbilityLevel for teams 1 and 2; -1 keeps the game's own level.
void SetTeamLevels(int team1, int team2);

// Parses a "GCS1|<team1>:<team2>" line into the team levels; false when it is not one.
bool ParseLine(const std::string& line);

// Drops the team levels (new client session).
void Clear();

// Installs the provider hook once the provider exists; called every server and client frame.
void Poll();
} // namespace StarCardLevels
} // namespace Kyber
