// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#include <Misc/ReinforcementWidgets.h>

#include <Base/Log.h>
#include <Misc/ReinforcementLimits.h>
#include <Misc/ReinforcementText.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace Kyber
{
namespace
{
constexpr size_t kDumpSize = 0x200;

struct WidgetSlot
{
    uintptr_t bus = 0;
    uintptr_t parentBus = 0;
    std::string kit;
    std::function<void(bool)> setEnabled;
};

std::mutex s_mutex;
std::unordered_map<uintptr_t, WidgetSlot> s_widgets;

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

void ReinforcementWidgets::Bind(uintptr_t entityAddress, uintptr_t bus, const std::string& kit, std::function<void(bool)> setEnabled,
                                uintptr_t parentBus)
{
    if (entityAddress == 0 || !setEnabled)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_widgets[entityAddress] = WidgetSlot{ bus, parentBus, kit, std::move(setEnabled) };
    ReinforcementText::RegisterCardWidget(entityAddress, bus, parentBus, "widget");
    static int s_bindLogs = 0;
    if (s_bindLogs < 40)
    {
        s_bindLogs++;
        KYBER_LOG(Info, "[GCWidget] bind entity=" << std::hex << entityAddress << " bus=" << bus << std::dec << " kit=" << kit);
    }
}

void ReinforcementWidgets::Unbind(uintptr_t entityAddress)
{
    ReinforcementText::UnregisterCardWidget(entityAddress);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_widgets.erase(entityAddress);
}

void ReinforcementWidgets::Clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_widgets.clear();
}

void ReinforcementWidgets::Apply(const std::string& kitName, bool enabled)
{
    if (kitName.empty())
    {
        return;
    }
    ReinforcementText::ResolvePending();
    std::vector<std::function<void(bool)>> writers;
    size_t widgetCount = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        widgetCount = s_widgets.size();
        for (auto& [address, slot] : s_widgets)
        {
            if (slot.kit.empty())
            {
                slot.kit = ReinforcementText::kitFor(address);
            }
            if (slot.kit.empty())
            {
                slot.kit = ReinforcementText::KitForBus(slot.bus);
            }
            if (slot.kit.empty())
            {
                slot.kit = ReinforcementText::KitForBus(slot.parentBus);
            }
            if (ReinforcementLimits::Matches(slot.kit, kitName) && slot.setEnabled)
            {
                writers.push_back(slot.setEnabled);
            }
        }
    }
    static int s_applyLogs = 0;
    if (s_applyLogs < 40)
    {
        s_applyLogs++;
        KYBER_LOG(Info, "[GCWidget] apply kit=" << kitName << " enabled=" << enabled << " matches=" << writers.size()
                                                << " widgets=" << widgetCount);
    }
    for (const auto& writer : writers)
    {
        writer(enabled);
    }
}
} // namespace Kyber
