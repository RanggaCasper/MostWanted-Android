#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>

struct X86Context;
namespace input {
struct Pad;
}

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};
inline bool KnownLanguage(const std::string& code) {
    return std::find(std::begin(kLanguages), std::end(kLanguages), code) != std::end(kLanguages);
}

// SSAA renders at twice the height, the presenter averages it down
inline constexpr uint32_t kAaOff = 0, kAaFxaa = 1, kAaSsaa = 2;

struct Options {
    bool fps_counter = false;
    uint32_t fps_position = 0;  // the counter's place, src/common/menu.cpp's kFpsPositionNames
    bool fps_usage = true;      // the counter's CPU and GPU load
    bool fps_temps = true;      // the counter's CPU, GPU and battery temperatures
    uint32_t screen_filter = 0;  // the present's look: 0 none, 1 sharpen, 2 vivid, 3 scanlines, 4 CRT, 5 HDR
    uint32_t anti_aliasing = 0;  // kAaOff, kAaFxaa or kAaSsaa
    uint32_t aspect_ratio = 0;  // the frame's shape in hundredths (133 is 4:3), 0 the whole screen; common/aspect.h
    bool bloom = true;  // the glow added over the frame, native/vulkan.cpp skips it when off
    bool fog = true;    // the distance fog, native/vulkan.cpp draws without it when off
    // the cars' reflection, six more views: drawn every frame (0), every 2nd (1), every 4th (2) or off (3, a grey
    // cube read instead); src/game/game.cpp's ApplyGraphicsSwitches
    uint32_t reflections = 2;  // mobile default: update the six-face car reflection every fourth frame
    bool particles = true;    // tyre smoke, sparks and crash debris, the same
    bool shadows = true;      // the shadows under the cars, the same; the trees' and buildings', native/vulkan.cpp
    bool road_reflections = true;  // the wet road's reflection, the same
    bool rain = true;         // the falling rain, src/game/game.cpp
    bool rain_drops = true;   // the drops on the screen in the rain, the same
    bool yellow_tint = false;  // the game's yellow colour grading, src/game/game.cpp greys it when off
    bool night_mode = false;   // night time visual mod: nocturnal grading, headlights, dark sky, src/game/game.cpp
    bool music_restored = false;  // once: the music switches an earlier build saved at 0 given back, src/game/game.cpp
    bool shader_preload = false;  // native/vulkan.cpp makes earlier runs' shaders and pipelines at the start
    bool texture_packs = true;  // the Mods Loader's texture packs drawn over the game's own, src/game/texture_packs.cpp
    uint32_t texture_resolution = 0;  // the packs' images at most this many texels across, 0 as installed; native/vulkan.cpp
    bool motion_blur = true;   // the blur by speed and the nitrous' radial blur, src/game/game.cpp skips them when off
    // cheats, src/game/game.cpp's ApplyCheats
    bool cheat_nos = false;           // the nitrous never runs out
    bool cheat_speedbreaker = false;  // the speedbreaker never runs out
    bool cheat_cash = false;          // the career's cash kept at kCheatCash
    bool cheat_no_police = false;     // no new cops or pursuits
    bool cheat_unlock_all = false;    // every car, part and race unlocked
    bool cheat_no_traffic = false;    // no traffic cars, src/game/game.cpp
    bool cheat_drag_shift = false;    // the gears shift by themselves in drag races, from the next race
    bool skip_movies = false;         // the movies end as they start, src/game/game.cpp
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
    std::map<std::string, std::string> pad;   // action id to a controller's input, absent is SDL's mapping
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool LanguageAvailable(const std::string& code);  // src/game/game.cpp: the LANGUAGE row offers it
inline std::atomic<uint32_t> g_road_reflection{0};  // the road reflection target's data, virtual; game.cpp, each present
inline std::atomic<uint32_t> g_env_cube{0};         // the cars' reflection cube's data, the same
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
void WidenAspect();         // src/game/game.cpp: game thread, at each present
void ApplyGraphicsSwitches();  // src/game/game.cpp: game thread, at each present
void ApplyCheats();            // src/game/game.cpp: game thread, at each present
bool Racing();    // src/game/game.cpp, any thread: the game flow is in a race (free roam too), not loading or the front end
inline std::atomic<bool> g_reset_car{false};  // native/touch.cpp's RESET, taken by ResetCarFrame
void ResetCarFrame(X86Context& ctx);          // src/game/game.cpp: game thread, at each present
void DragShiftFrame(X86Context& ctx);   // src/game/game.cpp: game thread, at each present
void MusicFrame(X86Context& ctx);                 // src/game/game.cpp: game thread, at each present
void ModCarsFrame(X86Context& ctx);     // src/game/car_mods.cpp: game thread, at each present
uint32_t ModCarTexture(X86Context& ctx, uint32_t hash);  // src/game/car_mods.cpp: an add-on's name texture, or 0
void TexturePacksFrame(X86Context& ctx);  // src/game/texture_packs.cpp: game thread, at each present
// cinematic mode (src/game/cinematic.cpp): the pause menu's icon asks for it, the next frame enters
void CinematicRequest(bool from_pause_menu = false);  // from the menu: leaving opens the pause menu again
bool CinematicActive();
bool CinematicClean();  // on, its menu hidden or its path playing: the picture alone, the touch controls out of it
bool CinematicPad(int port, const input::Pad& pad);  // the pad filter: true while the camera has the pad
void DrawCinematicHud();                             // inside the ImGui frame: its menu, Start shows and hides it
bool CinematicMenuHit(float x, float y);             // screen pixels on its menu: the touch controls leave them
// the replay (src/game/replay.cpp): the race recorded as it runs, played again from the pause menu's icon or the results
void ReplayRequest(bool from_pause_menu = false);  // from the menu: leaving opens the pause menu again
bool ReplayActive();
void ReplayFrame(X86Context& ctx);  // game thread, the frame's start (cinematic.cpp's hook): records, or plays
bool ReplayPad(int port, const input::Pad& pad);  // the pad filter: true while the replay has the pad
void DrawReplayHud();                             // inside the ImGui frame: its menu and timeline
bool ReplayMenuHit(float x, float y);             // screen pixels on its menu, or the results' REPLAY
bool ReplayStreaming();  // the replay itself sets the streamed place (replay.cpp's 0x235CD0 hook)
void ReplayRaceRestarted();  // ERestartRace ran (replay.cpp's 0x1A6DE0 hook): the recording starts over

}  // namespace game
