// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Spawn-menu button layer: enable/gray-out of kit buttons via TypeInfo Enabled/IsEnabled.

#include <functional>
#include <string>

namespace Kyber
{
class ReinforcementWidgets
{
public:
    // Writes the game Enabled/IsEnabled flag for every widget bound to this kit.
    static void Apply(const std::string& kitName, bool enabled);

    // Remembers a spawn-card widget so Apply can write its enable flag.
    static void Bind(uintptr_t entityAddress, uintptr_t bus, const std::string& kit, std::function<void(bool)> setEnabled,
                     uintptr_t parentBus = 0);

    // Drops a widget when its entity is destroyed.
    static void Unbind(uintptr_t entityAddress);

    // Clears every widget binding (new online session).
    static void Clear();

    // True when GC_WIDGET_DISCOVERY=1 is set in the environment.
    static bool IsDiscoveryEnabled();

    // Discovery: logs the type name, address and first 0x200 bytes of a UI screen message.
    static void OnUIScreenMessage(const char* typeName, const void* message);

    // Discovery: logs the type name, address and first 0x200 bytes of any object (read-guarded).
    static void LogObject(const char* label, const char* typeName, const void* object);
};
} // namespace Kyber
