// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Recent entity-create type names for crash dumps; per-create logging only with GC_ENTITY_CREATE_LOG=1.

namespace Kyber
{
namespace EntityCreateTrace
{
// Remembers one created entity's data type name and logs it when GC_ENTITY_CREATE_LOG=1.
void Record(const char* typeName);

// Logs the most recent creates, oldest first; called from the crash handler.
void LogRecent();
} // namespace EntityCreateTrace
} // namespace Kyber
