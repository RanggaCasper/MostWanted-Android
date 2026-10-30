#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>

struct X86Context;

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
    bool shader_preload = false;  // native/vulkan.cpp makes earlier runs' shaders and pipelines at the start
    bool motion_blur = true;   // the blur by speed and the nitrous' radial blur, src/game/game.cpp skips them when off
    // cheats, src/game/game.cpp's ApplyCheats
    bool cheat_nos = false;           // the nitrous never runs out
    bool cheat_speedbreaker = false;  // the speedbreaker never runs out
    bool cheat_cash = false;          // the career's cash kept at kCheatCash
    bool cheat_no_police = false;     // no new cops or pursuits
    bool cheat_unlock_all = false;    // every car, part and race unlocked
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

}  // namespace game
