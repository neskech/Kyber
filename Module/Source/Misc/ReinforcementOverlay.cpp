// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

#define _WINSOCKAPI_
#include <Hook/HookManager.h>
#include <Core/Program.h>
#include <Core/DebugHooks.h>
#include <SDK/Funcs.h>
#include <SDK/TypeInfo.h>
#include <Misc/ReinforcementLimits.h>
#include <Misc/ReinforcementText.h>
#include <Misc/ReinforcementWidgets.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Kyber
{
namespace
{
struct CachedLine
{
    std::string kit;
    std::string text;
    bool depleted = false;
};

std::vector<CachedLine> s_cachedLines;
std::chrono::steady_clock::time_point s_lastRefresh{};
constexpr auto kRefreshInterval = std::chrono::milliseconds(100);
} // namespace

// Per-frame debug kit list (GC_COUNT_OVERLAY=1) and the label/widget writers (GC_TEXT_HOOKS=1).
class ReinforcementOverlay : public GenericUpdateListener
{
public:
    // Rebuilds overlay text at most every 100 ms; draw uses the cached strings every frame.
    void Update(UpdateType type, const UpdateParameters& params) override
    {
        (void)type;
        (void)params;

        const bool hooks = ReinforcementText::LabelHooksEnabled();
        const bool drawOverlay = OverlayEnabled();
        if (!hooks && !drawOverlay)
        {
            return;
        }

        if (hooks)
        {
            ReinforcementText::ResolvePending();
            ReinforcementText::PollDiscovery();
        }

        // Nothing to draw or rewrite: skip all work.
        if (ReinforcementLimits::IsEmpty() && !(hooks && ReinforcementText::HasWriters()))
        {
            s_cachedLines.clear();
            return;
        }

        ClientGameContext* context = ClientGameContext::Get();
        if (context == nullptr || context->m_onlineManager == nullptr)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastRefresh >= kRefreshInterval)
        {
            s_lastRefresh = now;
            s_cachedLines.clear();
            for (const ReinforcementLimits::Entry& entry : ReinforcementLimits::Snapshot())
            {
                CachedLine line;
                line.kit = entry.kit;
                line.depleted = entry.cur == 0;
                line.text = ReinforcementLimits::ShortName(entry.kit) + " " + std::to_string(entry.cur) + "/" +
                            std::to_string(entry.start);
                if (hooks)
                {
                    ReinforcementWidgets::Apply(line.kit, !line.depleted);
                }
                s_cachedLines.push_back(std::move(line));
            }
        }

        int y = 60;
        for (const CachedLine& line : s_cachedLines)
        {
            if (drawOverlay)
            {
                DebugRenderer_drawText(20, y, line.depleted ? Colors::RED : Colors::GREEN, line.text, 1);
                y += 18;
            }
        }

        if (hooks)
        {
            ReinforcementText::RefreshAll();
        }
    }

private:
    // True only when GC_COUNT_OVERLAY=1 turns the debug kit list on.
    static bool OverlayEnabled()
    {
        static const bool enabled = []() {
            const char* value = std::getenv("GC_COUNT_OVERLAY");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
};

static ReinforcementOverlay s_reinforcementOverlay;

KB_REGISTER_GENERIC_UPDATE_LISTENER(s_reinforcementOverlay, UpdateType_Client_PostFrame);
} // namespace Kyber
