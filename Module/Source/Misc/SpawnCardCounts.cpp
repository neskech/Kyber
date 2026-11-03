// Copyright Armchair Developers / MagixGames. Licensed under GPLv3.

// Draws each GC-tracked kit's "cur/total" on its spawn-screen card, using the spawn grids' kit lists for card order.

#define _WINSOCKAPI_
#include <Entity/NativeEntityManager.h>

#include <Base/Log.h>
#include <Core/DebugHooks.h>
#include <Core/Program.h>
#include <Misc/KitLimitationControl.h>
#include <Misc/ReflectDump.h>
#include <SDK/Funcs.h>
#include <SDK/SDK.h>
#include <Utilities/StringUtils.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Kyber
{
namespace
{
using namespace Reflect;

// Empty stand-in so the wrapper has a typed data pointer.
class SpawnGridData : public DataContainer
{};

// Spawn-screen layout on the 1920x1080 UI stage.
constexpr float kStageW = 1920.0f;
constexpr float kStageH = 1080.0f;
constexpr float kGridLeft = 604.0f; // Left edge of the first spawn grid (vehicles/reinforcements).
constexpr float kGridTop = 701.0f;  // Top edge of the first card row.
constexpr float kCellPitch = 112.0f; // Card size; grids have no spacing.
constexpr int kGridRows = 2;

constexpr float kCharW = 14.0f;     // Debug-font glyph advance at scale 1, in stage units.
// The debug font brightens colors about 4x, so tokens are passed at a quarter of their value.
constexpr Color32 kShadow = Color32(0x00, 0x00, 0x00);
constexpr Color32 kText = Color32(0x37, 0x3a, 0x3b);  // --title #dde7ec
constexpr Color32 kEmpty = Color32(0x39, 0x15, 0x10); // --bad #e4543f

// "0" in GC_CARD_COUNTS turns the card counts off.
bool CountsEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_CARD_COUNTS");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// GC_CARD_COUNTS_DEBUG=1 marks every computed card cell.
bool DebugCells()
{
    static const bool enabled = []() {
        const char* value = std::getenv("GC_CARD_COUNTS_DEBUG");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// CharacterIds held by a grid's DataInput PropertyWrapper (wrapped array slot at +24, its TypeInfo at +32).
std::vector<uint32_t> DecodeCharacterIds(const uint8_t* wrapper)
{
    std::vector<uint32_t> ids;
    const TypeInfo* wrapped = nullptr;
    const uint8_t* slot = nullptr;
    if (wrapper == nullptr || !SafeRead(wrapper + 32, &wrapped, sizeof(wrapped)) || !ReadableType(wrapped) ||
        wrapped->getBasicType() != kTypeCode_Array || !SafeRead(wrapper + 24, &slot, sizeof(slot)) || slot == nullptr)
    {
        return ids;
    }
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!ReadArray(slot, data, size) || size > 64)
    {
        return ids;
    }
    for (uint32_t i = 0; i < size; i++)
    {
        const uint8_t* element = nullptr;
        const TypeInfo* type = nullptr;
        uint32_t id = 0;
        if (SafeRead(data + i * sizeof(void*), &element, sizeof(element)) && element != nullptr &&
            SafeRead(element + sizeof(void*), &type, sizeof(type)) && ReadableType(type) && ReadU32(element, type, "CharacterId", id))
        {
            ids.push_back(id);
        }
        else
        {
            ids.push_back(0);
        }
    }
    return ids;
}

// PropertyWrapper object behind a property value: inline when its TypeInfo sits at +8, else behind a pointer.
const uint8_t* WrapperAt(const void* value)
{
    const TypeInfo* inlineType = nullptr;
    if (SafeRead(static_cast<const uint8_t*>(value) + sizeof(void*), &inlineType, sizeof(inlineType)) && ReadableType(inlineType))
    {
        return static_cast<const uint8_t*>(value);
    }
    const uint8_t* object = nullptr;
    return SafeRead(value, &object, sizeof(object)) ? object : nullptr;
}

// True when a grid's CellTemplate is the spawn-screen resource card.
bool IsSpawnGrid(const SpawnGridData* data)
{
    DataContainer* cell = ReadPointerField(data, data->getType(), "CellTemplate");
    if (cell == nullptr || !ReadableType(cell->getType()))
    {
        return false;
    }
    const FieldInfoData* name = FindField(cell->getType(), "Name");
    const char* text = nullptr;
    if (name == nullptr || !SafeRead(reinterpret_cast<const uint8_t*>(cell) + name->fieldOffset, &text, sizeof(text)))
    {
        return false;
    }
    return ReadCString(text).find("SpawnResourceGridCell") != std::string::npos;
}

class SpawnGrid;
std::mutex s_mutex;
std::vector<SpawnGrid*> s_grids;
std::atomic<int> s_seq{ 0 };

// Wraps a spawn-screen GridElementData and keeps its card kit ids and visibility.
class SpawnGrid : public KyberEntity<SpawnGridData>
{
public:
    // Registers spawn grids; other grids are passed through untouched.
    SpawnGrid(NativeEntity* entity, SpawnGridData* data)
        : KyberEntity<SpawnGridData>(entity, data)
        , m_spawn(IsSpawnGrid(data))
        , m_seq(s_seq.fetch_add(1))
    {
        if (!m_spawn)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        s_grids.push_back(this);
    }

    // Unregisters before the native entity goes away.
    void OnDestroy() override
    {
        if (m_spawn)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_grids.erase(std::remove(s_grids.begin(), s_grids.end(), this), s_grids.end());
        }
        KyberEntityBase::OnDestroy();
    }

    // Forwards, then captures DataInput and Visible changes.
    void PropertyChanged(PropertyModification* modification) override
    {
        KyberEntityBase::PropertyChanged(modification);
        if (!m_spawn || modification->value == nullptr)
        {
            return;
        }
        if (modification->nameHash == DataInputHash())
        {
            std::vector<uint32_t> ids = DecodeCharacterIds(WrapperAt(modification->value));
            std::lock_guard<std::mutex> lock(s_mutex);
            m_ids = std::move(ids);
        }
        else if (modification->nameHash == VisibleHash())
        {
            bool visible = true;
            SafeRead(modification->value, &visible, 1);
            std::lock_guard<std::mutex> lock(s_mutex);
            m_visible = visible;
        }
    }

    // Reads DataInput from its connection when no change has arrived yet.
    void PollIds()
    {
        if (!m_ids.empty())
        {
            return;
        }
        DataContext dc;
        DataContext_ctor(m_nativeEntity->m_entityBus, &dc);
        PropertyReaderBase reader;
        PropertyReaderBase_set(&reader, &dc, m_data, static_cast<int32_t>(DataInputHash()), nullptr, nullptr);
        void* value = nullptr;
        if (reader.m_cache != nullptr && SafeRead(&reader.m_cache->value, &value, sizeof(value)) && value != nullptr)
        {
            m_ids = DecodeCharacterIds(WrapperAt(value));
        }
    }

    // Creation order; the vehicles/reinforcements grid is created before the hero grid.
    int Seq() const
    {
        return m_seq;
    }

    // Last Visible input value.
    bool Visible() const
    {
        return m_visible;
    }

    // Card CharacterIds in display order.
    const std::vector<uint32_t>& Ids() const
    {
        return m_ids;
    }

private:
    // Property hash of DataInput.
    static uint32_t DataInputHash()
    {
        static const uint32_t hash = static_cast<uint32_t>(StringUtils::HashQuick("DataInput"));
        return hash;
    }

    // Property hash of Visible.
    static uint32_t VisibleHash()
    {
        static const uint32_t hash = static_cast<uint32_t>(StringUtils::HashQuick("Visible"));
        return hash;
    }

    bool m_spawn;
    int m_seq;
    bool m_visible = true;
    std::vector<uint32_t> m_ids;
};

// Finds the game's "STAR WARS Battlefront II" window once (the process also owns the Kyber console).
HWND GameWindow()
{
    static HWND window = nullptr;
    if (window != nullptr && IsWindow(window))
    {
        return window;
    }
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            char title[64] = {};
            GetWindowTextA(hwnd, title, sizeof(title));
            if (pid == GetCurrentProcessId() && std::strncmp(title, "STAR WARS Battlefront", 21) == 0)
            {
                window = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        0);
    return window;
}

// True while the local player controls nothing, i.e. the spawn screen can be up.
bool LocalPlayerDead()
{
    ClientGameContext* context = ClientGameContext::Get();
    if (context == nullptr || context->GetPlayerManager() == nullptr)
    {
        return false;
    }
    ClientPlayer* player = context->GetPlayerManager()->GetLocalPlayer(LocalPlayerId_0);
    return player != nullptr && player->controlledControllable == nullptr;
}

// Maps the 1920x1080 stage onto the back buffer (fit, centered).
struct StageMap
{
    float scale = 0;
    float offsetX = 0;
    float offsetY = 0;

    // Stage point to back-buffer pixels.
    Vec2 At(float x, float y) const
    {
        return Vec2(offsetX + x * scale, offsetY + y * scale);
    }
};

// Stage mapping for the game window's client area, or scale 0 when unknown.
StageMap CurrentStage()
{
    StageMap map;
    RECT rect{};
    HWND window = GameWindow();
    if (window == nullptr || !GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0)
    {
        return map;
    }
    const float w = static_cast<float>(rect.right);
    const float h = static_cast<float>(rect.bottom);
    map.scale = std::min(w / kStageW, h / kStageH);
    map.offsetX = (w - kStageW * map.scale) / 2;
    map.offsetY = (h - kStageH * map.scale) / 2;
    return map;
}

// Draws one card's "cur/total" in its top-right corner, with a dark 1px shadow for contrast.
void DrawCount(const StageMap& stage, float cellX, float cellY, int cur, int start)
{
    const std::string text = std::to_string(std::max(0, cur)) + "/" + std::to_string(start);
    const float textW = kCharW * static_cast<float>(text.size());
    const Vec2 pos = stage.At(cellX + kCellPitch - textW - 8, cellY + 6);
    const int x = static_cast<int>(pos.x);
    const int y = static_cast<int>(pos.y);
    DebugRenderer_drawText(x + 1, y + 1, kShadow, text, 1);
    DebugRenderer_drawText(x, y, cur <= 0 ? kEmpty : kText, text, 1);
}

// Draws the card counts every client frame while the spawn screen is up.
class SpawnCardCounts : public GenericUpdateListener
{
public:
    // Lays the spawn grids out left to right and draws tracked kits' counts.
    void Update(UpdateType type, const UpdateParameters& params) override
    {
        (void)type;
        (void)params;
        if (!CountsEnabled() || !LocalPlayerDead())
        {
            return;
        }
        const StageMap stage = CurrentStage();
        if (stage.scale <= 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<SpawnGrid*> grids = s_grids;
        std::sort(grids.begin(), grids.end(), [](const SpawnGrid* a, const SpawnGrid* b) { return a->Seq() < b->Seq(); });
        float left = kGridLeft;
        for (SpawnGrid* grid : grids)
        {
            grid->PollIds();
            const std::vector<uint32_t>& ids = grid->Ids();
            if (ids.empty() || !grid->Visible())
            {
                continue;
            }
            const int count = static_cast<int>(ids.size());
            const int columns = (count + kGridRows - 1) / kGridRows;
            for (int i = 0; i < count; i++)
            {
                const float x = left + static_cast<float>(i % columns) * kCellPitch;
                const float y = kGridTop + static_cast<float>(i / columns) * kCellPitch;
                if (DebugCells())
                {
                    const Vec2 corner = stage.At(x, y);
                    DebugRenderer_drawText(static_cast<int>(corner.x), static_cast<int>(corner.y), Colors::YELLOW, "+" + std::to_string(i), 1);
                }
                int cur = 0;
                int start = 0;
                if (KitLimitationControl::CountFor(ids[i], cur, start))
                {
                    DrawCount(stage, x, y, cur, start);
                }
            }
            left += static_cast<float>(columns) * kCellPitch;
        }
    }
};

SpawnCardCounts s_spawnCardCounts;
KB_REGISTER_GENERIC_UPDATE_LISTENER(s_spawnCardCounts, UpdateType_Client_PostFrame);

// Creator for GridElementData.
KyberEntityBase* CreateSpawnGrid(EntityManager* entityManager, NativeEntity* entity, DataContainer* data)
{
    (void)entityManager;
    return new SpawnGrid(entity, reinterpret_cast<SpawnGridData*>(data));
}

// Registers the grid wrapper unless GC_CARD_COUNTS=0.
bool RegisterSpawnGrid()
{
    if (!CountsEnabled())
    {
        return false;
    }
    static EntityStaticRegistrar grid("GridElementData", CreateSpawnGrid, false);
    return true;
}

[[maybe_unused]] const bool s_spawnGridRegistered = RegisterSpawnGrid();
} // namespace
} // namespace Kyber
