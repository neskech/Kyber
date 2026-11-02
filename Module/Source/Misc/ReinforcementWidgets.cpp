// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementWidgets.h>

#include <Base/Log.h>
#include <Misc/ReinforcementText.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace Kyber
{
namespace
{
// Spawn widget layout is NOT mapped yet. Leave 0 until verified from a discovery log; 0 keeps Apply() inert.
constexpr size_t kEnabledFlagOffset = 0; // Offset of the enabled flag inside the kit button widget.
constexpr size_t kTintOffset = 0;        // Offset of the gray tint inside the kit button widget.
constexpr uintptr_t kSetTextFn = 0;      // Address of the set-text function (unmapped).

constexpr size_t kDumpSize = 0x200;

// Copies size bytes from ptr only when the whole range lies in one readable region.
bool SafeCopy(const void* ptr, void* out, size_t size)
{
    return ReinforcementText::SafeReadMemory(ptr, out, size);
}
} // namespace

bool ReinforcementWidgets::IsDiscoveryEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_WIDGET_DISCOVERY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void ReinforcementWidgets::LogObject(const char* label, const char* typeName, const void* object)
{
    if (!IsDiscoveryEnabled() || object == nullptr || !ReinforcementText::DiscoveryLogAllowed())
    {
        return;
    }

    uint8_t bytes[kDumpSize];
    if (!SafeCopy(object, bytes, kDumpSize))
    {
        KYBER_LOG(Info, "[WidgetDiscovery] " << label << " " << typeName << " @ " << object << ": memory not readable");
        return;
    }

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (size_t i = 0; i < kDumpSize; i++)
    {
        if (i % 16 == 0)
        {
            hex << "\n  +" << std::setw(3) << i << ":";
        }
        hex << ' ' << std::setw(2) << static_cast<int>(bytes[i]);
    }

    KYBER_LOG(Info, "[WidgetDiscovery] " << label << " " << typeName << " @ " << object << hex.str());
}

void ReinforcementWidgets::OnUIScreenMessage(const char* typeName, const void* message)
{
    LogObject("UIScreenMessage", typeName, message);
}

void ReinforcementWidgets::Apply(const std::string& kitName, bool enabled)
{
    if (kEnabledFlagOffset == 0)
    {
        static bool logged = false;
        if (!logged && IsDiscoveryEnabled())
        {
            logged = true;
            KYBER_LOG(Info, "[ReinforcementWidgets] widget offsets not mapped");
        }
        return;
    }

    // Offsets mapped but no widget lookup exists yet: the per-kit widget instance must be resolved here before writing.
    (void)kitName;
    (void)enabled;
    (void)kTintOffset;
    (void)kSetTextFn;
}
} // namespace Kyber
