// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#pragma once

// Spawn-menu button layer: widget discovery logging and (not yet mapped) enable/gray-out of kit buttons.

#include <string>

namespace Kyber
{
class ReinforcementWidgets
{
public:
    // Enables or disables the spawn button for a kit; a no-op until the widget offsets are mapped.
    static void Apply(const std::string& kitName, bool enabled);

    // True when GC_WIDGET_DISCOVERY=1 is set in the environment.
    static bool IsDiscoveryEnabled();

    // Discovery: logs the type name, address and first 0x200 bytes of a UI screen message.
    static void OnUIScreenMessage(const char* typeName, const void* message);

    // Discovery: logs the type name, address and first 0x200 bytes of any object (read-guarded).
    static void LogObject(const char* label, const char* typeName, const void* object);
};
} // namespace Kyber
