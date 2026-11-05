// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Diagnostic frame timing (GC_TICK_STATS=1): per-section time inside the server and client pre-frame hooks, logged every 10 s.

#include <cstdint>

namespace Kyber
{
namespace TickStats
{
// Pre-frame sections timed on each side.
enum Section
{
    Section_Game,
    Section_Entities,
    Section_Listeners,
    Section_Heartbeat,
    Section_Executor,
    Section_Events,
    Section_Api,
    Section_Lua,
    Section_Generic,
    Section_Count
};

// True when GC_TICK_STATS=1.
bool Enabled();

// QueryPerformanceCounter now, or 0 when disabled.
uint64_t Now();

// Adds the time since `start` to one section and returns the new now.
uint64_t Add(bool server, Section section, uint64_t start);

// Counts one frame for a side and logs the window every 10 s; the first call starts the hot-thread stack sampler.
void Frame(bool server);
} // namespace TickStats
} // namespace Kyber
