// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

class DataContainer;

// Recent entity-create type+asset names for crash dumps; per-create logging only with GC_ENTITY_CREATE_LOG=1.

namespace Kyber
{
namespace EntityCreateTrace
{
// Remembers one created entity's TypeInfo name, Name, and Identifier.
void Record(DataContainer* data, void* factoryParams = nullptr);

// Logs the most recent creates, oldest first; called from the crash handler.
void LogRecent();
} // namespace EntityCreateTrace
} // namespace Kyber
