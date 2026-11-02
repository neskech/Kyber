// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Spawn-button label text: pure formatting, kit binding for text entities, and GC_TEXT_* discovery helpers.

#include <Misc/ReinforcementLimits.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Kyber
{
class ReinforcementText
{
public:
    // Spacing used by FormatLabel.
    static constexpr int kLeadingSpaces = 2; // Spaces before the count when no cost text is known.
    static constexpr int kCostGapSpaces = 2; // Spaces between the cost text and the count.

    // Prefix of the synthetic localization ids used in GC_TEXT_MODE=localizedId.
    static constexpr const char* kLocalizedIdPrefix = "GC_REINF_";

    // Builds the label: "  cur/start" or, when cost is known, "<cost>  cur/start".
    static std::string FormatLabel(const ReinforcementLimits::Entry& entry, const std::string& cost = std::string());

    // Same hash the game uses for localization string ids (result = c + 33 * result).
    static int32_t HashStringId(const std::string& id);

    // True when GC_TEXT_DISCOVERY=1.
    static bool DiscoveryEnabled();

    // Value of GC_TEXT_FIELD, empty when unset.
    static const std::string& TextField();

    // Value of GC_TEXT_SOURCE_FIELD (falls back to GC_TEXT_FIELD): the string field read for binding and the cost text.
    static const std::string& SourceField();

    // True when GC_TEXT_MODE=localizedId (default is string).
    static bool LocalizedIdMode();

    // True when the writer is configured (GC_TEXT_FIELD set).
    static bool WriterEnabled();

    // Binds a text entity address to a kit short name.
    static void BindKit(uintptr_t entityAddress, const std::string& kitShortName);

    // Returns the kit short name bound to an entity address, empty when none.
    static std::string kitFor(uintptr_t entityAddress);

    // Binds the entity via GC_TEXT_BIND when its current text contains a configured substring; true if bound.
    static bool TryBindBySubstring(uintptr_t entityAddress, const std::string& currentText);

    // Finds the live entry whose short kit name matches; false when unknown.
    static bool ResolveEntry(const std::string& kitShortName, ReinforcementLimits::Entry& out);

    // Registers a per-entity writer called by RefreshAll.
    static void RegisterWriter(uintptr_t entityAddress, std::function<void()> writer);

    // Removes the writer of an entity.
    static void UnregisterWriter(uintptr_t entityAddress);

    // Calls every registered writer (per-frame path from the overlay).
    static void RefreshAll();

    // Returns true while fewer than 200 discovery lines were logged in the current second.
    static bool DiscoveryLogAllowed();

    // Marks that a UI screen (spawn screen) was just opened and resets the localization log cap.
    static void NoteSpawnScreenOpened();

    // Logs a localization lookup while the spawn screen was opened in the last 10 s; never alters results.
    static void LogLocalization(const char* id, const char* result);

    // Stores the text served for a synthetic localization id.
    static void SetLocalizedText(const std::string& id, const std::string& text);

    // Returns the custom text for a synthetic id, or nullptr when the id is not ours.
    static const char* LookupLocalized(const char* id);

    // Copies size bytes only when the whole range lies in one committed, readable region.
    static bool SafeReadMemory(const void* ptr, void* out, size_t size);

    // Passive discovery log for a component entity (no data pointer is read); only text-related type names are logged.
    static void LogComponentCreated(uintptr_t entityAddress, const char* entityTypeName);
};
} // namespace Kyber
