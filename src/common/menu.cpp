// the settings: their rows (app::rows, which the game's options menu shows, src/game/options_menu.cpp) and the panel on F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // ClearActiveID, the drag scroll's

#include "app/app.h"
#include "audio/output.h"
#include "common/aspect.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "common/translations.h"

// native/gyro.cpp: steering by tilting the phone
namespace gyro {
uint32_t TiltAngle();  // degrees for a full lock
void StepTiltAngle(int step);
}  // namespace gyro

// native/touch.cpp: the touch controls' layout
namespace touch {
uint32_t ControlsOpacity();
void StepOpacity(int step);
uint32_t ControlsSize();
void StepSize(int step);
const char* StickModeName();
void StepStickMode(int step);
const char* SteeringName();
void StepSteering(int step);
void EditLayout();
}  // namespace touch

// native/update.cpp: the activity asks GitHub for a newer release and says what it found
namespace host {
void CheckForUpdates();
// native/stats.cpp: the performance overlay's numbers, percent and degrees, -1 where the device does not tell
void SamplePerf();
float CpuLoad();
float GpuLoad();
float CpuTemp();
float GpuTemp();
float BatteryTemp();
}  // namespace host

namespace app {

Settings DefaultSettings();  // native/main.cpp: a phone's first run, unlimited frame rate and vsync off

namespace {

std::atomic<bool> g_open{false};

// gold on a dark red
constexpr ImVec4 kPanelBg{0.045f, 0.02f, 0.02f, 0.94f};
constexpr ImVec4 kBorder{0.80f, 0.62f, 0.22f, 0.85f};
constexpr ImVec4 kText{0.92f, 0.88f, 0.82f, 1.0f};
constexpr ImVec4 kHighlight{0.42f, 0.07f, 0.05f, 1.0f};
constexpr ImVec4 kHeading{0.92f, 0.72f, 0.28f, 1.0f};
constexpr ImVec4 kDim{0.64f, 0.56f, 0.50f, 1.0f};
constexpr ImVec4 kTab{0.14f, 0.05f, 0.04f, 1.0f};
constexpr ImVec4 kTabHovered{0.26f, 0.07f, 0.05f, 1.0f};
constexpr ImVec4 kButton{0.22f, 0.06f, 0.04f, 1.0f};
// lighter than the highlight, so it shows on a selected row
constexpr ImVec4 kButtonHovered{0.58f, 0.12f, 0.08f, 1.0f};
constexpr ImVec4 kLine{0.92f, 0.72f, 0.28f, 0.35f};
constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kScreenFilters[] = {0, 1, 2, 3, 4, 5};
const char* const kScreenFilterNames[] = {"OFF", "SHARPEN", "VIVID", "SCANLINES", "CRT", "HDR"};
constexpr uint32_t kAntiAliasings[] = {game::kAaOff, game::kAaFxaa, game::kAaSsaa};
const char* const kAntiAliasingNames[] = {"OFF", "FXAA", "SSAA 2X"};
// Options::reflections: how often the cars' reflection is drawn
constexpr uint32_t kReflectionRates[] = {0, 1, 2, 3};
const char* const kReflectionRateNames[] = {"FULL", "HALF", "QUARTER", "OFF"};
// the counter's place on the screen, Options::fps_position; top centre is under the settings gear
enum FpsPosition : uint32_t { kFpsTop, kFpsTopLeft, kFpsTopRight, kFpsBottomLeft, kFpsBottom, kFpsBottomRight };
constexpr uint32_t kFpsPositions[] = {kFpsTop, kFpsTopLeft, kFpsTopRight, kFpsBottomLeft, kFpsBottom, kFpsBottomRight};
const char* const kFpsPositionNames[] = {"TOP CENTER", "TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM CENTER",
                                         "BOTTOM RIGHT"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
const char* const kLanguageNames[] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH",
                                      "INDONESIAN"};  // as game::kLanguages
constexpr const char* kDonateUrl = "https://sociabuzz.com/racas/tribe";
constexpr const char* kGithubUrl = "https://github.com/RanggaCasper";
constexpr const char* kYoutubeUrl = "https://youtube.com/c/@RanggaCasper";
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// index nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(Ui(title).c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// a row's place and size: the label left, the value block (an arrow each side of the value) right
struct RowBox {
    float em;
    float row_w;
    float row_h;
    float value_w;
    float block_w;
    float row_x;
    float row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = ImGui::GetFrameHeight();
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    ImGui::SetCursorPos(ImVec2(box.row_x + ImGui::GetStyle().ItemInnerSpacing.x, box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());
}

// the cursor back to the row's start, then past the whole row
void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

void FillItem() {
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Header));
}

// label left, arrows and value right, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused()) {
        FillItem();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    RowLabel(box, label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = box.row_x + box.row_w - box.block_w;
    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + box.row_h + style.ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + box.value_w + style.ItemSpacing.x, box.row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = !on;
}

// applies and saves
void Changed() {
    audio::SetOutputMode(game::Opt().audio_output);
    Save();
}

enum RowId { kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kAudioOutput, kLanguage, kGyro,
             kGyroSensitivity, kTouchOpacity, kTouchSize, kTouchEdit, kCheckUpdates, kAspectRatio, kScreenFilter,
             kAntiAliasing, kFpsPosition, kFpsTemps, kStickMode, kBloom, kFog, kDonate, kGithub, kYoutube, kFpsUsage,
             kReflections, kParticles, kRain, kRainDrops, kShadows, kRoadReflections,
             kCheatNos, kCheatSpeedbreaker, kCheatCash, kCheatNoPolice, kCheatUnlockAll,
             kSkipMovies, kYellowTint, kMotionBlur, kShaderPreload };

using rows::Kind;
using rows::Row;

// the recomp's settings in pages of their own, grouped within; the game's options menu shows the headings too
constexpr Row kGraphicsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kAspectRatio, "ASPECT RATIO"},
    {Kind::Heading, -1, "PERFORMANCE"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kFpsPosition, "FPS COUNTER POSITION"},
    {Kind::Value, kFpsUsage, "SHOW CPU / GPU USAGE"},
    {Kind::Value, kFpsTemps, "SHOW TEMPERATURES"},
    {Kind::Value, kShaderPreload, "SHADER PRELOADING"},
    {Kind::Heading, -1, "QUALITY"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kAntiAliasing, "ANTI-ALIASING"},
    {Kind::Value, kBloom, "BLOOM"},
    {Kind::Value, kFog, "FOG"},
    {Kind::Value, kReflections, "CAR REFLECTIONS"},
    {Kind::Value, kRoadReflections, "ROAD REFLECTIONS"},
    {Kind::Value, kShadows, "SHADOWS"},
    {Kind::Value, kParticles, "PARTICLE EFFECTS"},
    {Kind::Value, kRain, "FALLING RAIN"},
    {Kind::Value, kRainDrops, "SCREEN RAINDROPS"},
    {Kind::Value, kYellowTint, "YELLOW TINT"},
    {Kind::Value, kMotionBlur, "MOTION BLUR"},
    {Kind::Value, kScreenFilter, "SCREEN FILTER"},
};

constexpr Row kTouchRows[] = {
    {Kind::Heading, -1, "TOUCH"},
    {Kind::Value, kGyro, "STEERING"},
    {Kind::Value, kGyroSensitivity, "TILT ANGLE"},
    {Kind::Value, kStickMode, "STICK MODE"},
    {Kind::Value, kTouchOpacity, "CONTROLS OPACITY"},
    {Kind::Value, kTouchSize, "CONTROLS SIZE"},
    {Kind::Link, kTouchEdit, "EDIT CONTROLS POSITION"},
};

// LANGUAGE is the settings panel's: the game takes its own from the console's language
constexpr Row kGeneralRows[] = {
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Value, kSkipMovies, "SKIP MOVIES"},
};

// the panel's alone: the game's options menu (src/game/options_menu.cpp) shows no section of it
constexpr Row kCheatRows[] = {
    {Kind::Value, kCheatNos, "INFINITE NITROUS"},
    {Kind::Value, kCheatSpeedbreaker, "INFINITE SPEEDBREAKER"},
    {Kind::Value, kCheatCash, "MAX CASH"},
    {Kind::Value, kCheatNoPolice, "NO POLICE"},
    {Kind::Value, kCheatUnlockAll, "UNLOCK ALL"},
};

// the credits' names, an info row's id; as they are, untranslated
const char* const kCreditNames[] = {"Rangga Casper", "Zukiru", "bagasromadon", "TGP482",
                                    "SDL, glslang, toml++, Dear ImGui", "xemu, Cxbx-Reloaded"};

// a page of its own: who made the port, and the links to support it
constexpr Row kCreditRows[] = {
    {Kind::Heading, -1, "CREDITS"},
    {Kind::Info, 0, "ANDROID PORT"},
    {Kind::Info, 1, "HDR"},
    {Kind::Info, 2, "BUTTON DESIGN"},
    {Kind::Info, 3, "RECOMPILER AND RUNTIME"},
    {Kind::Info, 4, "LIBRARIES"},
    {Kind::Info, 5, "XBOX REFERENCES"},
    {Kind::Heading, -1, "SUPPORT"},
    {Kind::Link, kDonate, "DONATE"},
    {Kind::Link, kGithub, "GITHUB"},
    {Kind::Link, kYoutube, "YOUTUBE"},
};

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

const std::vector<Row>& PadRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "CONTROLLER"}};
        size_t count = 0;
        const input::Action* actions = input::PadActions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::PadBind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kFpsTemps: return &o.fps_temps;
        case kFpsUsage: return &o.fps_usage;
        case kBloom: return &o.bloom;
        case kFog: return &o.fog;
        case kParticles: return &o.particles;
        case kShadows: return &o.shadows;
        case kRoadReflections: return &o.road_reflections;
        case kRain: return &o.rain;
        case kRainDrops: return &o.rain_drops;
        case kYellowTint: return &o.yellow_tint;
        case kMotionBlur: return &o.motion_blur;
        case kShaderPreload: return &o.shader_preload;
        case kCheatNos: return &o.cheat_nos;
        case kCheatSpeedbreaker: return &o.cheat_speedbreaker;
        case kCheatCash: return &o.cheat_cash;
        case kCheatNoPolice: return &o.cheat_no_police;
        case kCheatUnlockAll: return &o.cheat_unlock_all;
        case kSkipMovies: return &o.skip_movies;
        default: return nullptr;
    }
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRenderResolution: {
            // 20% to 200% in steps of 5
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            Mode out = Output();
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, out.width * s.render_percent / 100,
                          out.height * s.render_percent / 100);
            return true;
        }
        case kAspectRatio: {
            // 1:1 to the whole screen, an unknown saved value the whole screen's
            constexpr int count = int(std::size(game::kAspects));
            int i = count - 1;
            for (int j = 0; j < count; j++)
                if (game::kAspects[j].code == o.aspect_ratio) i = j;
            if (step) {
                i = StepIndex(i, step, count);
                o.aspect_ratio = game::kAspects[i].code;
            }
            std::snprintf(text, sizeof(text), "%s", game::kAspects[i].name);
            return true;
        }
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), cap == 60 ? "%u (DEFAULT)" : "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kAntiAliasing:
            std::snprintf(text, sizeof(text), "%s", kAntiAliasingNames[StepTable(kAntiAliasings, o.anti_aliasing, step)]);
            return true;
        case kScreenFilter:
            std::snprintf(text, sizeof(text), "%s", kScreenFilterNames[StepTable(kScreenFilters, o.screen_filter, step)]);
            return true;
        case kReflections:
            std::snprintf(text, sizeof(text), "%s", kReflectionRateNames[StepTable(kReflectionRates, o.reflections, step)]);
            return true;
        case kFpsPosition:
            std::snprintf(text, sizeof(text), "%s", kFpsPositionNames[StepTable(kFpsPositions, o.fps_position, step)]);
            return true;
        default:
            return false;
    }
}

// the general and touch rows that are no ON / OFF, stepped first
void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kGyro:
            // the RACING layout's arrows, its wheel, or the phone tilted
            if (step) touch::StepSteering(step);
            std::snprintf(text, sizeof(text), "%s", touch::SteeringName());
            break;
        case kStickMode:
            // floating sticks come where the thumb lands, fixed ones stay where they are drawn
            if (step) touch::StepStickMode(step);
            std::snprintf(text, sizeof(text), "%s", touch::StickModeName());
            break;
        case kTouchOpacity:
            if (step) touch::StepOpacity(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsOpacity());
            break;
        case kTouchSize:
            if (step) touch::StepSize(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsSize());
            break;
        case kCheckUpdates:
            // any step asks; the answer comes in a dialog over the game
            if (step) host::CheckForUpdates();
            std::snprintf(text, sizeof(text), "CHECK");
            break;
        case kGyroSensitivity:
            // the phone's tilt for a full lock, 10 to 60 degrees: less turns quicker
            if (step) gyro::StepTiltAngle(step);
            std::snprintf(text, sizeof(text), "%u\xC2\xB0", gyro::TiltAngle());
            break;
        case kLanguage: {
            int count = int(std::size(game::kLanguages));
            int i = 0;
            while (i < count - 1 && o.language != game::kLanguages[i]) i++;
            // past the languages this disc has no strings for; English and Indonesian are always there
            if (step) {
                do i = StepIndex(i, step, count);
                while (!game::LanguageAvailable(game::kLanguages[i]));
                o.language = game::kLanguages[i];
            }
            const char* format = o.language == game::BootLanguage() ? "%s" : "%s (RESTART)";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
    }
}

// steps the value first when step is not zero
std::string RowText(int id, int step, Settings& s = Get()) {
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) Changed();
    return text;
}

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

// as options_menu.cpp's captions
const char* const kPages[] = {"GRAPHICS", "TOUCH CONTROLS", "GENERAL", "KEYBOARD", "CONTROLLER", "CREDITS", "CHEATS"};
constexpr int kSettingsPage = 0;
constexpr int kTouchPage = 1;
constexpr int kGeneralPage = 2;
constexpr int kKeyboardPage = 3;
constexpr int kControllerPage = 4;
constexpr int kCreditsPage = 5;
constexpr int kCheatsPage = 6;

void RestoreDefaults(int page) {
    if (page == kCreditsPage) return;  // nothing to reset
    game::Options& o = game::Opt();
    if (page == kCheatsPage) {
        game::Options none;
        o.cheat_nos = none.cheat_nos;
        o.cheat_speedbreaker = none.cheat_speedbreaker;
        o.cheat_cash = none.cheat_cash;
        o.cheat_no_police = none.cheat_no_police;
        o.cheat_unlock_all = none.cheat_unlock_all;
        Changed();
        return;
    }
    if (page == kKeyboardPage) {
        input::ResetBindings();
        return;
    }
    if (page == kControllerPage) {
        input::ResetPadBindings();
        return;
    }
    game::Options keep = o;
    Get() = DefaultSettings();
    o = game::Options{};
    o.keys = keep.keys;
    o.pad = keep.pad;
    Changed();
}

// the row being rebound, -1 when idle; its keys must be released before the next press binds
std::atomic<int> g_capture{-1};
int g_capture_slot = 0;  // its box: 0 the key, 1 the alternate
int g_hovered_box = -1;  // the box under the mouse or focused with a key in it, this frame
bool g_capture_armed = false;
int g_capture_done = 0;  // frame of the last bind, the key that bound must not start another
// the controller row being rebound, -1 when idle: as the keys', its inputs let go before the next press binds; Escape
// or kPadCaptureMs with no press cancels, for a player with the controller alone
std::atomic<int> g_pad_capture{-1};
bool g_pad_capture_armed = false;
std::chrono::steady_clock::time_point g_pad_capture_since;
constexpr std::chrono::seconds kPadCaptureTimeout{5};
// the panel shows the cheats alone; the settings are the game's own options menu (src/game/options_menu.cpp)
constexpr int g_page = kCheatsPage;
// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

// label left, a box per key right (the key and its alternate side by side), ending where the other rows' values do;
// Enter or click starts a box's capture, right click or Delete sets `cleared`; returns the box, -1 for none
int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, int& cleared,
            const char* prompt = "PRESS A KEY") {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = n > 1 ? row.em * kKeyBoxEms : row.value_w;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        bool deleted = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || deleted) cleared = i;
        bool pointed = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (pointed && !keys[i].empty() && keys[i] != kUnbound) g_hovered_box = i;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        // key names as they are
        std::string value = keys[i];
        if (capturing == i) value = Ui(prompt);
        else if (value == kUnbound) value = Ui(kUnbound);
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

// Escape cancels, any other bindable key or mouse button binds
void PollCapture() {
    if (g_capture < 0) return;
    int vk = input::PressedKey();
    if (!g_capture_armed) {
        g_capture_armed = !vk;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
    g_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// any input of a controller binds once those held when it began are let go
void PollPadCapture() {
    if (g_pad_capture < 0) return;
    std::string input = input::PressedPadInput();
    bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                  std::chrono::steady_clock::now() - g_pad_capture_since > kPadCaptureTimeout;
    if (!cancel && !g_pad_capture_armed) {
        g_pad_capture_armed = input.empty();
        return;
    }
    if (!cancel && input.empty()) return;
    if (!cancel) input::PadBind(size_t(g_pad_capture.load()), input);
    g_pad_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// not under a bind or on the button that just bound
bool CaptureFree() { return g_capture < 0 && g_pad_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1; }

void ValueRow(const Row& r) {
    int step = OptionRow(r.label, Ui(RowText(r.id, 0)).c_str());
    if (step) RowText(r.id, step);
}

const char* LinkUrl(int id) {
    switch (id) {
        case kDonate: return kDonateUrl;
        case kGithub: return kGithubUrl;
        case kYoutube: return kYoutubeUrl;
        default: return nullptr;
    }
}

// label left, OPEN right; a tap, click, Enter or A anywhere on the row opens the page in the browser, or the touch
// layout's editor
void LinkRow(const Row& r) {
    RowBox box = MeasureRow();
    ImGui::PushID(r.label);
    bool open = ImGui::Selectable("##link", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) FillItem();
    RowLabel(box, r.label);
    std::string value = Ui("OPEN");
    float value_x = box.row_x + box.row_w - box.block_w + box.row_h + ImGui::GetStyle().ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ImGui::PopID();
    EndRow(box);
    if (!open) return;
    if (r.id == kTouchEdit) touch::EditLayout();
    else if (const char* url = LinkUrl(r.id)) SDL_OpenURL(url);
}

// the role left, the name right-aligned at the row's end
void InfoRow(const Row& r) {
    RowBox box = MeasureRow();
    RowLabel(box, r.label);
    if (r.id >= 0 && r.id < int(std::size(kCreditNames))) {
        const char* name = kCreditNames[r.id];
        float x = box.row_x + box.row_w - ImGui::CalcTextSize(name).x - ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, box.row_y));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
    }
    EndRow(box);
}

void KeyRow(const Row& r) {
    int cleared = -1;
    int box = BindRow(r.label, KeyBoxes(size_t(r.id)), g_capture == r.id ? g_capture_slot : -1, cleared);
    // a Delete that answered a capture is no clear
    if (cleared >= 0 && CaptureFree()) input::Bind(size_t(r.id), 0, cleared);
    if (box < 0 || !CaptureFree()) return;
    g_capture_slot = box;
    g_capture = r.id;
    g_capture_armed = false;
}

// the bound input, else the mapping's; a clear goes back to the mapping
void PadRow(const Row& r) {
    size_t count = 0;
    const input::Action& action = input::PadActions(&count)[r.id];
    std::string bound = input::PadBinding(size_t(r.id));
    int cleared = -1;
    int box = BindRow(r.label, {bound.empty() ? action.defaults : bound}, g_pad_capture == r.id ? 0 : -1, cleared,
                      "PRESS A BUTTON");
    if (cleared >= 0 && CaptureFree()) input::PadBind(size_t(r.id), "");
    if (box < 0 || !CaptureFree()) return;
    g_pad_capture = r.id;
    g_pad_capture_armed = false;
    g_pad_capture_since = std::chrono::steady_clock::now();
}

void DrawRows(int page) {
    size_t count = 0;
    const Row* list = rows::Page(page, &count);
    for (size_t i = 0; i < count; i++) {
        const Row& r = list[i];
        if (r.kind == Kind::Heading) Section(r.label);
        else if (r.kind == Kind::Value) ValueRow(r);
        else if (r.kind == Kind::Link) LinkRow(r);
        else if (r.kind == Kind::Info) InfoRow(r);
        else if (r.kind == Kind::PadBind) PadRow(r);
        else KeyRow(r);
    }
}

void Defaults() {
    g_capture = -1;
    g_pad_capture = -1;
    RestoreDefaults(g_page);
}

void DrawTitle() {
    std::string title = Ui("CHEATS");
    float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title.c_str()).x) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
    ImGui::TextUnformatted(title.c_str());
}

constexpr const char* kCredit = "Port by Rangga Casper";

// the hint line, then the credit on the left and Back and Defaults on the right
void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const char* hint = "";
    if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
    else if (g_pad_capture >= 0) hint = "Press a button on the controller to bind, wait 5 seconds to cancel";
    ImGui::TextUnformatted(Ui(hint, false).c_str());
    ImGui::PopStyleColor();
    float bw = em * 8;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kDim, "%s", kCredit);
    ImGui::SameLine(0, 0);
    float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
    if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(bw, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(bw, 0))) Defaults();
}

// The rows a finger drags up or down scroll with it, as a phone's lists do: the row it came down on is let go, so the
// drag presses nothing. A drag of the scrollbar itself stays the scrollbar's.
void DragScroll() {
    static bool dragging = false;
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging = false;
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!dragging) {
        bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
        bool inside = window->Rect().Contains(io.MouseClickedPos[ImGuiMouseButton_Left]);
        bool scrollbar = ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);
        float moved = std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y);
        dragging = touch && inside && !scrollbar && moved > ImGui::GetFontSize() * 0.6f;
        if (!dragging) return;
    }
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    ImGui::ClearActiveID();
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 40, em * 16), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    // a scrollbar a thumb can take
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, em * 1.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, em * 2.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        DrawTitle();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        g_hovered_box = -1;
        DrawRows(g_page);
        DragScroll();
        ImGui::EndChild();
        DrawFooter(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

// the window's display's refresh rate, 0 when unreadable
uint32_t RefreshRate() {
    SDL_Window* w = static_cast<SDL_Window*>(Window());
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
}

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == kKeyboardPage) {
        *count = InputRows().size();
        return InputRows().data();
    }
    if (page == kControllerPage) {
        *count = PadRows().size();
        return PadRows().data();
    }
    if (page == kTouchPage) {
        *count = std::size(kTouchRows);
        return kTouchRows;
    }
    if (page == kGeneralPage) {
        *count = std::size(kGeneralRows);
        return kGeneralRows;
    }
    if (page == kCreditsPage) {
        *count = std::size(kCreditRows);
        return kCreditRows;
    }
    if (page == kCheatsPage) {
        *count = std::size(kCheatRows);
        return kCheatRows;
    }
    *count = std::size(kGraphicsRows);
    return kGraphicsRows;
}

int rows::PageCount() { return int(std::size(kPages)); }

const char* rows::PageName(int page) { return page >= 0 && page < rows::PageCount() ? kPages[page] : ""; }

std::string rows::Label(const Row& r) { return Ui(r.label); }

std::string rows::Value(const Row& r, int step) {
    switch (r.kind) {
        case Kind::Heading:
            return "";
        case Kind::Value:
            return Ui(RowText(r.id, step));
        case Kind::Link:
            return Ui("OPEN");
        case Kind::Info:
            return r.id >= 0 && r.id < int(std::size(kCreditNames)) ? kCreditNames[r.id] : "";
        case Kind::Bind: {
            if (g_capture == r.id) return Ui("PRESS A KEY");
            std::vector<std::string> keys = KeyBoxes(size_t(r.id));
            std::string text = keys[0] == kUnbound ? Ui(kUnbound) : keys[0];
            if (!keys[1].empty()) text += " / " + keys[1];
            return text;
        }
        case Kind::PadBind: {
            if (g_pad_capture == r.id) return Ui("PRESS A BUTTON");
            size_t count = 0;
            std::string bound = input::PadBinding(size_t(r.id));
            return bound.empty() ? input::PadActions(&count)[r.id].defaults : bound;
        }
    }
    return "";
}

void rows::Activate(const Row& r) {
    if (r.kind == Kind::Link) {
        if (r.id == kTouchEdit) touch::EditLayout();
        else if (const char* url = LinkUrl(r.id)) SDL_OpenURL(url);
    } else if (r.kind == Kind::Bind && g_capture < 0 && g_pad_capture < 0) {
        g_capture_slot = 0;
        g_capture_armed = false;
        g_pad_capture_since = std::chrono::steady_clock::now();
        g_capture = r.id;
    } else if (r.kind == Kind::PadBind && g_capture < 0 && g_pad_capture < 0) {
        g_pad_capture_armed = false;
        g_pad_capture_since = std::chrono::steady_clock::now();
        g_pad_capture = r.id;
    }
}

// as PollCapture and PollPadCapture without the panel: the key or button that started the wait comes up first, Escape
// or kPadCaptureTimeout gives up
bool rows::PollBinding() {
    using Clock = std::chrono::steady_clock;
    static std::string candidate;
    static Clock::time_point first_press{};

    if ((g_capture >= 0 || g_pad_capture >= 0) &&
        (input::PressedKey() == VK_ESCAPE ||
         Clock::now() - g_pad_capture_since > kPadCaptureTimeout)) {
        candidate.clear();
        rows::CancelBinding();
        return true;
    }

    if (g_capture >= 0) {
        const int key = input::PressedKey();
        if (!g_capture_armed) {
            g_capture_armed =
                !key && input::PressedPadInput().empty();
            return false;
        }
        if (!key) return false;
        input::Bind(size_t(g_capture.load()), key, g_capture_slot);
        g_capture = -1;
        return true;
    }

    if (g_pad_capture >= 0) {
        const std::string source = input::PressedPadInput();

        if (!g_pad_capture_armed) {
            candidate.clear();
            g_pad_capture_armed = source.empty();
            return false;
        }

        if (candidate.empty()) {
            if (source.empty()) return false;
            candidate = source;
            first_press = Clock::now();
        }

        // Logical trigger wins over the first raw sample.
        // Otherwise retain the first input rather than chasing noise.
        const bool trigger =
            source == "trigger left" || source == "trigger right";
        const bool already_trigger =
            candidate == "trigger left" || candidate == "trigger right";

        if (trigger && !already_trigger)
            candidate = source;

        if (Clock::now() - first_press <
            std::chrono::milliseconds(100))
            return false;

        input::PadBind(size_t(g_pad_capture.load()), candidate);
        candidate.clear();
        g_pad_capture = -1;
        return true;
    }

    candidate.clear();
    return false;
}

void rows::CancelBinding() {
    g_capture = -1;
    g_pad_capture = -1;
}

void rows::Defaults(int page) {
    g_capture = -1;
    g_pad_capture = -1;
    RestoreDefaults(page);
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture >= 0 || g_pad_capture >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    g_capture = -1;
    g_pad_capture = -1;
    if (open) input::ReleaseCursor();
}

// "CPU 40% 65°C", the parts the device tells
void AppendPerf(std::string& line, const char* name, float load, float temp) {
    if (load < 0 && temp < 0) return;
    char part[48];
    int n = std::snprintf(part, sizeof(part), "%s%s", line.empty() ? "" : "   ", name);
    if (load >= 0) n += std::snprintf(part + n, sizeof(part) - n, " %.0f%%", load);
    if (temp >= 0) std::snprintf(part + n, sizeof(part) - n, " %.0f\xC2\xB0" "C", temp);
    line += part;
}

// a line of text with a drop shadow, its left edge at x
void DrawShadowed(ImDrawList* dl, float size, ImVec2 at, ImU32 color, const char* text) {
    float shadow = std::max(2.0f, size / 12.0f);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(ImGui::GetFont(), size, at, color, text);
}

// the frame rate, and under it the CPU's and GPU's load and temperatures and the battery's, over the game at the
// place Options::fps_position picks
void DrawFpsCounter(double fps) {
    const game::Options& o = game::Opt();
    if (!o.fps_counter || MenuOpen()) return;
    host::SamplePerf();
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    std::string perf;
    AppendPerf(perf, "CPU", o.fps_usage ? host::CpuLoad() : -1, o.fps_temps ? host::CpuTemp() : -1);
    AppendPerf(perf, "GPU", o.fps_usage ? host::GpuLoad() : -1, o.fps_temps ? host::GpuTemp() : -1);
    if (o.fps_temps) AppendPerf(perf, "BAT", -1, host::BatteryTemp());

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    float big = ImGui::GetFontSize() * 1.5f, small = ImGui::GetFontSize();
    ImVec2 fs = ImGui::GetFont()->CalcTextSizeA(big, FLT_MAX, 0, text);
    ImVec2 ps = perf.empty() ? ImVec2(0, 0) : ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, perf.c_str());
    ImVec2 cs = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, kCredit);
    // the first line: the frame rate, the credit beside it
    float gap = small;
    float line_w = fs.x + gap + cs.x;
    float block = fs.y + ps.y;
    float margin = screen.y * 0.03f;
    uint32_t pos = o.fps_position;
    bool bottom = pos == kFpsBottomLeft || pos == kFpsBottom || pos == kFpsBottomRight;
    // top centre sits under the settings gear, bottom centre over Back and Start, which the touch controls keep there
    float y = pos == kFpsTop ? screen.y * 0.14f
              : pos == kFpsBottom ? screen.y * 0.86f - block
              : bottom ? screen.y - margin - block
                       : margin;
    // left, centred or right aligned, each line on its own; every corner at the edge, as far in as it is from the
    // top or bottom
    float side = margin;
    auto x = [&](float w) {
        if (pos == kFpsTopLeft || pos == kFpsBottomLeft) return side;
        if (pos == kFpsTopRight || pos == kFpsBottomRight) return screen.x - side - w;
        return screen.x / 2 - w / 2;
    };
    float line_x = x(line_w);
    DrawShadowed(dl, big, ImVec2(line_x, y), IM_COL32(255, 214, 90, 255), text);
    // the credit beside the frame rate, dimmer, its foot on the same line
    DrawShadowed(dl, small, ImVec2(line_x + fs.x + gap, y + fs.y - cs.y), IM_COL32(200, 180, 150, 220), kCredit);
    if (!perf.empty()) DrawShadowed(dl, small, ImVec2(x(ps.x), y + fs.y), IM_COL32(235, 235, 235, 255), perf.c_str());
}

void DrawUi(double fps) {
    DrawFpsCounter(fps);
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    // scale the font with the output height: a phone's dense screen reads 13 px text at 540 rows to the height, the
    // game's font (native/ui.cpp, taller) brought to that size
    float base = io.Fonts->Fonts.empty() ? 13.0f : io.Fonts->Fonts[0]->FontSize;
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 540.0f) * 13.0f / base;
    if (MenuOpen()) {
        PollCapture();
        PollPadCapture();
        DrawPanel();
    }
}

}  // namespace app
