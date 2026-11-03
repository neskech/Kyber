// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Spawn-button label text: kit binding from cost rows, live cur/start writes, and optional GC_TEXT_* discovery.

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

    // Label field to write; GC_TEXT_FIELD or "Text".
    static const std::string& TextField();

    // Field read for the original label; GC_TEXT_SOURCE_FIELD or TextField().
    static const std::string& SourceField();

    // True when GC_TEXT_MODE=localizedId (default is string).
    static bool LocalizedIdMode();

    // True when label writers should run (always, once a Text field exists).
    static bool WriterEnabled();

    // True when at least one per-frame writer is registered.
    static bool HasWriters();

    // Records a cost-row settings id; keeps it pending until a GCR1 kit matches.
    static void RegisterCostKit(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& kit);

    // Drops a cost-row kit when that entity is destroyed.
    static void UnregisterCostKit(uintptr_t entityAddress);

    // Matches pending settings ids to GCR1 kits and cards; safe to call often.
    static void ResolvePending();

    // Records a spawn-card widget in creation order for index pairing.
    static void RegisterCardWidget(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& typeName);

    // Drops a widget from the order list.
    static void UnregisterCardWidget(uintptr_t entityAddress);

    // Records a TextElement so a matched card can receive cur/start.
    static void RegisterTextSlot(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus);

    // Learns settings-id -> widget from a selected card's live Text value.
    static void NoteTextSelection(uintptr_t entityAddress, uintptr_t bus, uintptr_t parentBus, const std::string& text);

    // Kit id bound to this bus or its parent, empty when none.
    static std::string KitForBus(uintptr_t bus);

    // Clears cost-row bus bindings (new online session).
    static void ClearUi();

    // Logs one identifier dump; returns the seq, or -1 when the create cap is hit.
    static int DumpCreate(const char* kind, uintptr_t entity, uintptr_t bus, uintptr_t parentBus, const std::string& typeName,
                          const std::string& fields);

    // Binds a text entity address to a kit id.
    static void BindKit(uintptr_t entityAddress, const std::string& kitShortName);

    // Returns the kit short name bound to an entity address, empty when none.
    static std::string kitFor(uintptr_t entityAddress);

    // Binds the entity via GC_TEXT_BIND when its current text contains a configured substring; true if bound.
    static bool TryBindBySubstring(uintptr_t entityAddress, const std::string& currentText);

    // Binds the entity when the card label matches a live GCR1 kit short name.
    static bool TryBindByKitTokens(uintptr_t entityAddress, const std::string& currentText);

    // Appends cur/start onto a loc result that names a limited kit; nullptr when it does not.
    static const char* RewriteLocalized(const char* id, const char* result);

    // Kit remembered from a loc rewrite, empty when this hash was never a limited card.
    static std::string KitForLocalizedHash(int32_t hash);

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

    // Remembers a cost/text/widget row so PollDiscovery can re-read its CString fields.
    static void RegisterDiscoveryPoll(uintptr_t entityAddress, const std::string& dataType, std::function<std::string()> snapshot);

    // Drops a poll row when that entity is destroyed.
    static void UnregisterDiscoveryPoll(uintptr_t entityAddress);

    // Logs every registered row once per second for 30 s after the first row or spawn-screen open.
    static void PollDiscovery();

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
