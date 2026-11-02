// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Client-side store of per-kit reinforcement counts fed by "GCR1|" admin chat lines.

#include <chrono>
#include <string>
#include <vector>

namespace Kyber
{
class ReinforcementLimits
{
public:
    struct Entry
    {
        std::string kit;
        int cur = 0;
        int start = 0;
        std::chrono::steady_clock::time_point updatedAt{};
    };

    // Parses one GCR1 line and merges it into the store; false if the line is not a GCR1 line (pass it through).
    static bool ParseLine(const std::string& line);

    // Removes every stored kit.
    static void Clear();

    // Returns a copy of the store, sorted by kit name.
    static std::vector<Entry> Snapshot();

    // True when the store holds no kits (no allocation).
    static bool IsEmpty();

    // True when the kit is tracked and has cur == 0.
    static bool IsDepleted(const std::string& kit);

    // Copies the entry for a kit into out; false when the kit is unknown.
    static bool Find(const std::string& kit, Entry& out);

    // Returns the text after the last '/' of a kit name.
    static std::string ShortName(const std::string& kit);
};
} // namespace Kyber
