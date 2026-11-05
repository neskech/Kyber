// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Walks a kit's weapon customization tables to list weapon and attachment unlock assets.

#pragma once

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Kyber
{
namespace UnlockCatalog
{
// One unlock asset in a kit's weapon tree.
struct Entry
{
    std::string name;          // Asset name.
    std::string type;          // Unlock asset TypeInfo name.
    std::string guid;          // Instance GUID text, empty when the asset has none.
    uint32_t identifier = 0;   // UnlockAssetBase.Identifier.
    uint32_t score = 0;        // UnlockAssetBase.UnlockScore.
    int weaponSlot = -1;       // Index of the weapon's UnlockParts group (-1 for the weapon itself).
    int weaponIndex = -1;      // Weapon position within its group, or the parent weapon for attachments.
    bool attachment = false;   // True for attachment unlocks under a weapon.
    int level = 0;             // Chain depth through NextLevelUnlockAsset (0 = base).
};

// Logs a "[GCUnlock]" tree for the kit (weapons, attachments, criteria); runs once per kit path.
void Probe(DataContainer* kit, const std::string& path);

// Probes every kit path in GC_UNLOCK_PROBE_KITS (comma list) that has loaded and was not probed yet.
void ProbeFromEnv();

// Polls the ability watch probe, then logs ability asset pointers found in the player's ability extent, with nearby bytes.
void DumpAbilities(ServerPlayer* player);

// Enables WSOfflineInventory with the item aliases in GC_OFFLINE_INVENTORY (comma list); no-op when unset.
void ApplyOfflineInventoryFromEnv();
} // namespace UnlockCatalog
} // namespace Kyber
