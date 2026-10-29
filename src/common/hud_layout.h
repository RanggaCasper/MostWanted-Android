#pragma once
#include <atomic>
#include <cstddef>

// The HUD's speedometer, minimap, race information (top right) and EA Trax banner, moved where the touch layout
// editor puts them (native/touch.cpp, saved in layout.toml): src/game/hud_layout.cpp moves their FEng groups by these
// offsets
namespace game {

enum HudItem : size_t { kHudSpeedometer, kHudMinimap, kHudRaceInfo, kHudTrax, kHudItems };

// FEng's screen: its origin the frame's middle, 480 high, x right and y down
struct HudBox {
    float x, y;    // the item's middle where the game puts it
    float hw, hh;  // half its width and height
};
// as HUD_SingleRace.fng and EA_TRAX.fng draw them, measured off the frame; the race's information with three racers
inline constexpr HudBox kHudBoxes[kHudItems] = {
    {307, 133, 80, 78}, {-307, 131, 68, 68}, {296, -136, 78, 64}, {-168, -183, 176, 32}};

// how far each is moved, FEng units; the editor writes them, the game thread reads them at each present
inline std::atomic<float> g_hud_x[kHudItems] = {}, g_hud_y[kHudItems] = {};

void ApplyHudLayout();  // src/game/hud_layout.cpp: game thread, at each present

}  // namespace game
