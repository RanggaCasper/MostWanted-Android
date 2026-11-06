// CINEMATIC MODE: a photo mode from the pause menu. The race is paused already there, and the world is still drawn
// behind the menu each frame; this hides the menu and HUD and flies the camera freely about the car.
//
// How the game moves its camera (decompiled): each frame 0x1B93D0 runs the camera directors (CameraAI, 0x66AE0) and
// the views' camera movers (0x62CA0, mover vtable + 8), only while the race runs: paused (dt 0, game flow 6) both are
// skipped. A mover makes its world to view matrix (the debug world mover 0x649E0: a look-at, 0xC7320) and hands it
// to Camera::SetCameraMatrix (0x599E0, thiscall: the camera; the matrix, dt), which keeps the current key as the
// previous one (+0xE0), sets the matrix, the position, direction and target from it, and each key's velocity
// (+0x200 on) over dt. So the free camera is set the same way, through 0x599E0, once a frame at the frame's render
// (0x0D8720), before any view's projection (0x0C7740 copies the camera's matrix, 0x3FC50). Written into the camera's
// memory instead, its previous key and velocities stayed the old camera's and the picture came apart as it moved.
//
// The player's view (1) shares its Camera with the road reflection (4) and the glow (6). A Camera's current key, as on
// the PC (NFSPluginSDK's CameraParams), measured live:
//   +0x00 the world to view matrix, row vectors: column 0 the right axis, 1 the down axis, 2 forward; row 3 -P·axes
//   +0x40 position, +0x50 direction (forward), +0x60 target (position + direction * +0xB0, the target distance)
//   +0x70 to +0xAF the shake's noise frequencies and amplitudes, +0xC4 the horizontal field of view (16 bit angle)
// World up is +Z, units metres. Leaving gives the camera back as it was, and the menu is where it was.
//
// The precull (a precomputed visible set per 32 m cell of the camera's position, mode at 0x469740) is off while it is
// on, as the game turns it off itself for its cutscenes' cameras ([0x4C8800] > 0, 0x23FB7A).
//
// The menus and HUD: FEng's draws in 0x0D82D0 (the front end, foreground and background; its setup still runs, see
// there), and the rear view mirror (0x0D4860, view 3 drawn at 186..456 x 24..88 of the front end's 640x480) are
// skipped while it is on. The pad is the camera's (game_info.cpp's filter): the game sees
// none, and none until it is let go after leaving, so the B that leaves is not the menu's too.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/translations.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {

constexpr uint32_t kViews = 0x48AC80, kViewSize = 0x70, kPlayerView = 1, kViewCamera = 0x40;
constexpr uint32_t kCamPosition = 0x40, kCamTarget = 0x60;
constexpr uint32_t kCamNoise = 0x70, kCamNoiseEnd = 0xB0, kCamFov = 0xC4;
// the current and previous keys, the velocities and the elapsed time SetCameraMatrix writes: saved, given back
constexpr uint32_t kCamSaved = 0x2B0;
constexpr uint32_t kSetCameraMatrix = 0x599E0;

struct Vec {
    float x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Normal(Vec a) {
    float l = Length(a);
    return l > 1e-6f ? a * (1.0f / l) : Vec{1, 0, 0};
}

constexpr float kPi = 3.14159265f;
// the field of view as the Camera keeps it: 65536 a whole turn
constexpr float kFovUnit = 65536.0f / 360.0f;
constexpr float kFovMin = 8.0f, kFovMax = 110.0f;  // degrees
// how far from the car the camera may go: the world is streamed about it, and further out it is not loaded. Not
// under its road either, which is not drawn from below. The car's middle is the reflection cube's camera's position
// (view 0x12, drawn about the car), its road about 0.6 m under it
constexpr float kRange = 25.0f, kAboveCar = -0.35f;
constexpr uint32_t kCubeView = 0x12;
constexpr uint32_t kPrecullMode = 0x469740;
constexpr float kPitchLimit = 88.0f * kPi / 180.0f, kRollLimit = 60.0f * kPi / 180.0f;
// full stick: metres a second, degrees a second, the zoom's degrees a second, the roll's
constexpr float kMoveSpeed = 7.0f, kFastScale = 4.0f, kLookSpeed = 110.0f, kZoomSpeed = 40.0f, kRollSpeed = 45.0f;
// the speeds ease to the stick's with these rates (1/s): a start and a stop glide rather than jump
constexpr float kMoveEase = 6.0f, kLookEase = 10.0f, kZoomEase = 8.0f;
constexpr float kDeadZone = 0.16f;

// the camera as it flies
struct Fly {
    Vec pos;
    float yaw = 0, pitch = 0, roll = 0, fov = 70;
    Vec vel;  // m/s
    float yaw_rate = 0, pitch_rate = 0, roll_rate = 0, fov_rate = 0;
};

// game thread state, under g_lock where the pad filter (another guest thread) reads it
std::mutex g_lock;
std::atomic<bool> g_request{false};  // the pause menu's icon picked; taken by the next frame
// Entered from the pause menu, the menu stays as Accept left it: an icon's Accept is its React's to finish, and
// OPTIONS' opens another package, where ours opened none, so the menu took no more input. Leaving opens
// Pause_Main.fng again on the next frame (SwitchPackage on cFEng: the name, 0, 0, 0, as 0x1563C8). Not 1, as the
// options screen's way back passes at times (0x105446): the menu keeps it (+0x144, 0x104CB0) and its Setup (0xEED40)
// then made other icons, Restart where free roam's menu has Resume Free Roam
constexpr uint32_t kFEng = 0x48E94C, kSwitchPackage = 0xEB160, kPauseMainFng = 0x3D0618;
std::atomic<bool> g_request_menu{false};
bool g_from_menu = false, g_reopen_menu = false;
std::atomic<bool> g_active{false};
std::atomic<bool> g_drain{false};  // left: the game sees no pad until all of it is let go
std::atomic<double> g_drain_until{0};
uint32_t g_camera = 0;
uint8_t g_saved[kCamSaved];
uint32_t g_saved_precull = 0;
uint32_t g_matrix = 0;  // guest memory for the matrix handed to SetCameraMatrix
Fly g_start, g_fly;
Vec g_origin;  // what the range is about: the car's middle as it entered
input::Pad g_pad;  // the latest pad, the camera's
uint16_t g_prev_buttons = 0;
uint8_t g_prev_analog[8] = {};
bool g_first_pad = true;
std::chrono::steady_clock::time_point g_last;
std::atomic<bool> g_hints{true};  // the controls' panel, X shows and hides it; kept from one entry to the next

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float F(uint32_t va) { return kernel::G<float>(va); }
Vec V(uint32_t va) { return {F(va), F(va + 4), F(va + 8)}; }
void SetF(uint32_t va, float f) { kernel::G<float>(va) = f; }
void SetV(uint32_t va, Vec v, float w) {
    SetF(va, v.x);
    SetF(va + 4, v.y);
    SetF(va + 8, v.z);
    SetF(va + 12, w);
}

uint32_t PlayerCamera() {
    uint32_t cam = kernel::R32(kViews + kPlayerView * kViewSize + kViewCamera);
    return cam && mem::IsCommitted(cam) && mem::IsCommitted(cam + kCamSaved - 1) ? cam : 0;
}

// a stick past the dead zone, rescaled to 0..1 beyond it and squared for fine aim near the middle
float Stick(int16_t v) {
    float f = std::clamp(float(v) / 32767.0f, -1.0f, 1.0f);
    float a = std::fabs(f);
    if (a < kDeadZone) return 0;
    a = (a - kDeadZone) / (1 - kDeadZone);
    return std::copysign(a * a, f);
}
float Trigger(uint8_t v) { return v > 24 ? float(v - 24) / 231.0f : 0.0f; }

// the camera's axes: forward from the yaw and pitch, right level with the ground, then the roll about forward
void Axes(const Fly& f, Vec& right, Vec& down, Vec& fwd) {
    float cp = std::cos(f.pitch);
    fwd = {cp * std::cos(f.yaw), cp * std::sin(f.yaw), std::sin(f.pitch)};
    Vec r0 = Normal(Cross(fwd, {0, 0, 1}));
    Vec u0 = Cross(r0, fwd);
    float cr = std::cos(f.roll), sr = std::sin(f.roll);
    right = r0 * cr + u0 * sr;
    Vec up = u0 * cr - r0 * sr;
    down = up * -1.0f;
}

// the fly's world to view matrix handed to the game's SetCameraMatrix, as a camera mover hands its own
void WriteCamera(X86Context& ctx, uint32_t cam, const Fly& f, float dt) {
    if (!g_matrix) g_matrix = mem::AllocSystem(64);
    if (!g_matrix) return;
    Vec r, d, fw;
    Axes(f, r, d, fw);
    const Vec cols[3] = {r, d, fw};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            const Vec& c = cols[col];
            SetF(g_matrix + uint32_t(row * 16 + col * 4), row == 0 ? c.x : row == 1 ? c.y : c.z);
        }
        SetF(g_matrix + uint32_t(row * 16 + 12), 0);
    }
    SetF(g_matrix + 0x30, -Dot(f.pos, r));
    SetF(g_matrix + 0x34, -Dot(f.pos, d));
    SetF(g_matrix + 0x38, -Dot(f.pos, fw));
    SetF(g_matrix + 0x3C, 1);
    // no shake (its amplitudes) and the zoom, in the current key it keeps
    for (uint32_t o = kCamNoise + 0x10; o < kCamNoiseEnd; o += 0x20) SetV(cam + o, {}, 0);
    kernel::G<uint16_t>(cam + kCamFov) = uint16_t(std::lround(f.fov * kFovUnit));
    float step = std::max(dt, 1.0f / 240);
    uint32_t bits;
    std::memcpy(&bits, &step, 4);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = cam;
    kernel::CallGuest(ctx, kSetCameraMatrix, {g_matrix, bits});
    ctx.ecx = ecx;
}

// the game's camera as a Fly: forward is the direction, the roll measured from the level right axis
Fly ReadCamera(uint32_t cam) {
    Fly f;
    f.pos = V(cam + kCamPosition);
    Vec fw = Normal({F(cam + 8), F(cam + 0x18), F(cam + 0x28)});
    f.yaw = std::atan2(fw.y, fw.x);
    f.pitch = std::asin(std::clamp(fw.z, -1.0f, 1.0f));
    Vec r0 = Normal(Cross(fw, {0, 0, 1}));
    Vec u0 = Cross(r0, fw);
    Vec right = Normal({F(cam), F(cam + 0x10), F(cam + 0x20)});
    f.roll = std::atan2(Dot(right, u0), Dot(right, r0));
    f.fov = std::clamp(float(kernel::G<uint16_t>(cam + kCamFov)) / kFovUnit, kFovMin, kFovMax);
    return f;
}

float Ease(float cur, float want, float rate, float dt) { return cur + (want - cur) * (1 - std::exp(-rate * dt)); }

void Enter() {
    uint32_t cam = PlayerCamera();
    if (!cam || !Racing()) {
        LOGR("cinematic: not entered, camera %08X racing %d", cam, Racing());
        return;
    }
    std::lock_guard lock(g_lock);
    g_camera = cam;
    std::memcpy(g_saved, kernel::HostPtr(cam), kCamSaved);
    g_start = g_fly = ReadCamera(cam);
    // the car's middle from the cube's camera; the game camera's target (about the roof) when that is not near
    g_origin = V(cam + kCamTarget) - Vec{0, 0, 0.7f};
    uint32_t cube = kernel::R32(kViews + kCubeView * kViewSize + kViewCamera);
    if (cube && mem::IsCommitted(cube + kCamPosition + 12)) {
        Vec car = V(cube + kCamPosition);
        if (Length(car - V(cam + kCamTarget)) < 8.0f) g_origin = car;
    }
    g_saved_precull = kernel::R32(kPrecullMode);
    kernel::W32(kPrecullMode, 0);
    g_first_pad = true;
    g_last = std::chrono::steady_clock::now();
    g_active = true;
    g_from_menu = g_request_menu;
}

// why: logged when it was not the player's leaving (the race ended or the camera changed under it)
void Leave(const char* why) {
    if (!g_active) return;
    if (g_camera && mem::IsCommitted(g_camera)) std::memcpy(kernel::HostPtr(g_camera), g_saved, kCamSaved);
    kernel::W32(kPrecullMode, g_saved_precull);
    g_active = false;
    g_reopen_menu = g_from_menu && Racing();  // not when the race ended under it
    g_from_menu = false;
    g_drain_until = Now() + 1.5;
    g_drain = true;
    if (why) LOGR("cinematic: off, %s", why);
}

// a button pressed since the last frame
bool Pressed(int analog) { return g_pad.analog[analog] > 0x40 && g_prev_analog[analog] <= 0x40; }

// game thread, at the frame's start: the pad to the camera, eased, kept in range, set
void Step(X86Context& ctx) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
    g_last = now;
    std::lock_guard lock(g_lock);
    if (!Racing() || PlayerCamera() != g_camera) {  // the race ended under it: its camera is not ours to give back
        g_camera = 0;
        Leave(Racing() ? "the camera changed" : "the race ended");
        return;
    }
    const input::Pad& p = g_pad;
    if (g_first_pad) {
        // the buttons held as it entered (the menu's A) are not presses
        g_prev_buttons = p.buttons;
        std::memcpy(g_prev_analog, p.analog, 8);
        g_first_pad = false;
    }
    bool leave = Pressed(1) || ((p.buttons & 0x10) && !(g_prev_buttons & 0x10));  // B, Start
    if (Pressed(3)) {  // Y: back to where it entered
        g_fly = g_start;
        g_fly.vel = {};
        g_fly.yaw_rate = g_fly.pitch_rate = g_fly.roll_rate = g_fly.fov_rate = 0;
    }
    if (Pressed(2)) {  // X: the controls' panel hidden or shown
        g_hints = !g_hints.load();
    }
    g_prev_buttons = p.buttons;
    std::memcpy(g_prev_analog, p.analog, 8);
    if (leave) {
        Leave(nullptr);
        return;
    }

    Fly& f = g_fly;
    // the look slows as it zooms in, so a long lens aims as finely as a wide one
    float zoom = f.fov / 70.0f;
    float look = kLookSpeed * kPi / 180.0f * std::clamp(zoom, 0.15f, 1.2f);
    // the D-pad: on a controller the roll; from the keyboard (the arrows are its D-pad) the look, as a keyboard has
    // its right stick only on the numpad
    float dpad_x = (p.buttons & 0x08 ? 1.0f : 0.0f) - (p.buttons & 0x04 ? 1.0f : 0.0f);
    float dpad_y = (p.buttons & 0x01 ? 1.0f : 0.0f) - (p.buttons & 0x02 ? 1.0f : 0.0f);
    bool keys = input::KeyboardActive();
    float look_x = std::clamp(Stick(p.rx) + (keys ? dpad_x : 0.0f), -1.0f, 1.0f);
    float look_y = std::clamp(Stick(p.ry) + (keys ? dpad_y : 0.0f), -1.0f, 1.0f);
    f.yaw_rate = Ease(f.yaw_rate, -look_x * look, kLookEase, dt);
    f.pitch_rate = Ease(f.pitch_rate, look_y * look, kLookEase, dt);
    float roll_in = keys ? 0.0f : dpad_x;
    f.roll_rate = Ease(f.roll_rate, roll_in * kRollSpeed * kPi / 180.0f, kLookEase, dt);
    float zoom_in = Trigger(p.analog[7]) - Trigger(p.analog[6]);  // RT in, LT out
    f.fov_rate = Ease(f.fov_rate, -zoom_in * kZoomSpeed * std::clamp(zoom, 0.3f, 1.0f), kZoomEase, dt);

    f.yaw += f.yaw_rate * dt;
    f.pitch = std::clamp(f.pitch + f.pitch_rate * dt, -kPitchLimit, kPitchLimit);
    f.roll += f.roll_rate * dt;
    if (p.buttons & 0x80) {  // right thumb: the roll back to level
        f.roll = Ease(f.roll, 0, 8.0f, dt);
        f.roll_rate = 0;
    }
    f.roll = std::clamp(f.roll, -kRollLimit, kRollLimit);
    f.fov = std::clamp(f.fov + f.fov_rate * dt, kFovMin, kFovMax);
    if (f.fov == kFovMin || f.fov == kFovMax) f.fov_rate = 0;

    // moves level, as a dolly: forward the view's heading, the strafe across it, and straight up and down on the
    // shoulders (white down, black up). Along the view itself a look down dived the camera into the road
    Vec ahead{std::cos(f.yaw), std::sin(f.yaw), 0};
    Vec level_right = Normal(Cross(ahead, {0, 0, 1}));
    float vertical = Trigger(p.analog[4]) - Trigger(p.analog[5]);
    float speed = kMoveSpeed * (p.buttons & 0x40 ? kFastScale : 1.0f);
    Vec want = (ahead * Stick(p.ly) + level_right * Stick(p.lx) + Vec{0, 0, 1} * vertical) * speed;
    f.vel = {Ease(f.vel.x, want.x, kMoveEase, dt), Ease(f.vel.y, want.y, kMoveEase, dt),
             Ease(f.vel.z, want.z, kMoveEase, dt)};
    Vec pos = f.pos + f.vel * dt;
    // in range: a sphere about the car, not below its road
    Vec off = pos - g_origin;
    float len = Length(off);
    if (len > kRange) {
        pos = g_origin + off * (kRange / len);
        Vec n = off * (1 / len);
        f.vel = f.vel - n * std::max(0.0f, Dot(f.vel, n));
    }
    if (pos.z < g_origin.z + kAboveCar) {
        pos.z = g_origin.z + kAboveCar;
        f.vel.z = std::max(0.0f, f.vel.z);
    }
    f.pos = pos;
    WriteCamera(ctx, g_camera, f, dt);
}

}  // namespace

void CinematicRequest(bool from_pause_menu) {
    g_request_menu = from_pause_menu;
    g_request = true;
}
bool CinematicActive() { return g_active; }

// the pad filter, before the game sees it: true while the pad is the camera's (or still held after leaving)
bool CinematicPad(int port, const input::Pad& pad) {
    if (port != 0) return g_active || g_drain;
    if (g_active) {
        std::lock_guard lock(g_lock);
        g_pad = pad;
        return true;
    }
    if (g_drain) {
        // let go: the buttons up, the sticks in their dead zone (a real one never rests at 0), or a time out
        constexpr int kRest = 8000;
        auto moved = [](int16_t v) { return v > kRest || v < -kRest; };
        bool held = pad.buttons || moved(pad.lx) || moved(pad.ly) || moved(pad.rx) || moved(pad.ry) ||
                    std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t a) { return a > 0x30; });
        if (!held || Now() > g_drain_until) g_drain = false;
        return g_drain;
    }
    return false;
}

// the controls' panel along the bottom, until X hides it (X shows it again); never in a snapshot of the frame
void DrawCinematicHud() {
    if (!g_active || !g_hints || app::MenuOpen()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    // in the front end's units, 480 down the screen, so it is the size of the game's own menus on any screen
    const float u = screen.y / 480.0f;
    auto col = [](int r, int g, int b, int a) { return IM_COL32(r, g, b, a); };
    // the pause menu's colours: amber bars, a dark striped panel, cream text and corner brackets
    const ImU32 amber = col(236, 156, 40, 255), bar = col(196, 124, 28, 165), panel = col(22, 16, 8, 185),
                stripe = col(255, 200, 120, 14), cream = col(255, 238, 208, 255), bracket = col(240, 228, 205, 230),
                hollow = col(0, 0, 0, 110);
    auto width = [font](float px, const std::string& s) { return font->CalcTextSizeA(px, FLT_MAX, 0, s.c_str()).x; };
    auto text = [&](float px, ImVec2 c, ImU32 colour, const std::string& s) {  // centred on c
        dl->AddText(font, px, ImVec2(c.x - width(px, s) / 2, c.y - px / 2), colour, s.c_str());
    };

    // the prompts: a pad's glyphs, then what they do
    enum class Glyph { Face, Pill, Stick, Dpad };
    struct Key {
        Glyph glyph;
        const char* label;
    };
    struct Prompt {
        std::vector<Key> keys;
        const char* action;
    };
    static const std::vector<Prompt> rows[2] = {
        {{{{Glyph::Stick, "L"}}, "MOVE"},
         {{{Glyph::Stick, "R"}}, "LOOK"},
         {{{Glyph::Pill, "LT"}, {Glyph::Pill, "RT"}}, "ZOOM"},
         {{{Glyph::Pill, "LB"}, {Glyph::Pill, "RB"}}, "HEIGHT"}},
        {{{{Glyph::Dpad, ""}}, "ROLL"},
         {{{Glyph::Pill, "R3"}}, "LEVEL"},
         {{{Glyph::Pill, "L3"}}, "FAST"},
         {{{Glyph::Face, "Y"}}, "RESET"},
         {{{Glyph::Face, "X"}}, "HIDE"},
         {{{Glyph::Face, "B"}}, "BACK"}},
    };
    const float gh = 15 * u, label_px = 12.5f * u, key_px = 9.5f * u, gap = 4 * u, item_gap = 13 * u;
    auto glyph_w = [&](const Key& k) { return k.glyph == Glyph::Pill ? gh * 1.75f : gh; };
    auto prompt_w = [&](const Prompt& p) {
        float w = 0;
        for (const Key& k : p.keys) w += glyph_w(k) + 2.5f * u;
        return w + gap + width(label_px, app::Ui(p.action));
    };
    auto draw_key = [&](const Key& k, ImVec2 c) {
        float r = gh / 2, th = 1.4f * u;
        switch (k.glyph) {
            case Glyph::Face:
                dl->AddCircleFilled(c, r, hollow, 32);
                dl->AddCircle(c, r, amber, 32, th);
                text(key_px * 1.15f, c, cream, k.label);
                break;
            case Glyph::Pill: {
                float hw = glyph_w(k) / 2, hh = r * 0.82f;
                dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), hollow, hh);
                dl->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), amber, hh, 0, th);
                text(key_px, c, cream, k.label);
                break;
            }
            case Glyph::Stick:
                dl->AddCircle(c, r, amber, 32, th);
                dl->AddCircleFilled(c, r * 0.62f, amber, 32);
                text(key_px, c, col(30, 20, 8, 255), k.label);
                break;
            case Glyph::Dpad: {
                float a = r, b = r * 0.36f;
                for (ImVec4 q : {ImVec4(-a, -b, a, b), ImVec4(-b, -a, b, a)}) {
                    dl->AddRectFilled(ImVec2(c.x + q.x, c.y + q.y), ImVec2(c.x + q.z, c.y + q.w), amber, b * 0.4f);
                }
                dl->AddCircleFilled(c, b * 0.55f, col(30, 20, 8, 255), 12);
                break;
            }
        }
    };

    // the panel: as wide as the longer row, the title bar over the prompts
    float rows_w = 0;
    for (const auto& row : rows) {
        float w = 0;
        for (const Prompt& p : row) w += prompt_w(p) + item_gap;
        rows_w = std::max(rows_w, w - item_gap);
    }
    const float pad = 14 * u, title_h = 28 * u, row_h = 24 * u;
    const float w = std::min(rows_w + pad * 2, screen.x - 16 * u), h = title_h + row_h * 2 + 10 * u;
    const ImVec2 p0(screen.x / 2 - w / 2, screen.y - h - 22 * u), p1(p0.x + w, p0.y + h);
    const float split = p0.y + title_h;

    // the title bar: the camera's icon and the mode's name
    dl->AddRectFilled(p0, ImVec2(p1.x, split), bar);
    dl->AddLine(ImVec2(p0.x, split), ImVec2(p1.x, split), amber, 1.2f * u);
    {
        float s = 20 * u, x = p0.x + pad, y = p0.y + (title_h - s) / 2;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), amber, 3 * u);
        ImU32 ink = col(40, 26, 8, 255);
        dl->AddRectFilled(ImVec2(x + s * 0.16f, y + s * 0.34f), ImVec2(x + s * 0.84f, y + s * 0.78f), ink, 2 * u);
        dl->AddRectFilled(ImVec2(x + s * 0.34f, y + s * 0.24f), ImVec2(x + s * 0.56f, y + s * 0.36f), ink, 1 * u);
        dl->AddCircleFilled(ImVec2(x + s * 0.5f, y + s * 0.56f), s * 0.17f, amber, 20);
        dl->AddCircleFilled(ImVec2(x + s * 0.5f, y + s * 0.56f), s * 0.09f, ink, 16);
        const std::string title = app::Ui("CINEMATIC MODE");
        float px = 16 * u;
        dl->AddText(font, px, ImVec2(x + s + 9 * u + u, p0.y + (title_h - px) / 2 + u), col(0, 0, 0, 160), title.c_str());
        dl->AddText(font, px, ImVec2(x + s + 9 * u, p0.y + (title_h - px) / 2), cream, title.c_str());
    }

    // the prompts' panel, striped as the game's dialogs
    dl->AddRectFilled(ImVec2(p0.x, split), p1, panel);
    dl->PushClipRect(ImVec2(p0.x, split), p1, true);
    for (float x = p0.x - h; x < p1.x; x += 7 * u)
        dl->AddLine(ImVec2(x, p1.y), ImVec2(x + (p1.y - split), split), stripe, 2.5f * u);
    dl->PopClipRect();
    for (int r = 0; r < 2; r++) {
        float total = -item_gap;
        for (const Prompt& p : rows[r]) total += prompt_w(p) + item_gap;
        float x = screen.x / 2 - total / 2, cy = split + 5 * u + row_h * (float(r) + 0.5f);
        for (const Prompt& p : rows[r]) {
            for (const Key& k : p.keys) {
                draw_key(k, ImVec2(x + glyph_w(k) / 2, cy));
                x += glyph_w(k) + 2.5f * u;
            }
            x += gap;
            const std::string action = app::Ui(p.action);
            dl->AddText(font, label_px, ImVec2(x, cy - label_px / 2), cream, action.c_str());
            x += width(label_px, action) + item_gap;
        }
    }

    // the corner brackets about it all, as about the pause menu
    const float o = 4 * u, l = 11 * u, bt = 1.6f * u;
    for (int i = 0; i < 4; i++) {
        float sx = i & 1 ? 1.0f : -1.0f, sy = i & 2 ? 1.0f : -1.0f;
        ImVec2 c(i & 1 ? p1.x + o : p0.x - o, i & 2 ? p1.y + o : p0.y - o);
        dl->AddLine(c, ImVec2(c.x - sx * l, c.y), bracket, bt);
        dl->AddLine(c, ImVec2(c.x, c.y - sy * l), bracket, bt);
    }
}

}  // namespace game

// the frame's render: the request taken, the camera stepped and set before any view's projection
X86_EXTERN_FUNC(__imp__sub_000D8720);
X86_FUNC(sub_000D8720) {
    if (game::g_reopen_menu) {
        game::g_reopen_menu = false;
        if (uint32_t feng = kernel::R32(game::kFEng)) {
            uint32_t ecx = ctx.ecx;
            ctx.ecx = feng;
            kernel::CallGuest(ctx, game::kSwitchPackage, {game::kPauseMainFng, 0, 0, 0});
            ctx.ecx = ecx;
        }
    }
    if (game::g_request.exchange(false) && !game::g_active) {
        game::Enter();
        if (!game::g_active && game::g_request_menu) game::g_reopen_menu = true;  // no camera: the menu back next frame
    }
    if (game::g_active) game::Step(ctx);
    __imp__sub_000D8720(ctx, base);
}

// the front end's draw (cdecl, the layer): its setup runs, FEng's draws do not. The setup ("FE Setup", 0x0C0F90 at
// 0x0D84C2) binds the frame with the world's depth surface again, and the next frame's first clear (colour and depth)
// clears what is bound: skipped whole, the depth was never cleared, and once the camera moved the world failed the
// depth test against the last frame's and the sky showed through, black below the horizon. FEng draws only while the
// dword at 0x427A74 (read only at 0x0D85EC, never written) is set
X86_EXTERN_FUNC(__imp__sub_000D82D0);
X86_FUNC(sub_000D82D0) {
    if (!game::g_active) return __imp__sub_000D82D0(ctx, base);
    constexpr uint32_t kDrawFEng = 0x427A74;
    uint32_t draw = kernel::R32(kDrawFEng);
    kernel::W32(kDrawFEng, 0);
    __imp__sub_000D82D0(ctx, base);
    kernel::W32(kDrawFEng, draw);
}

X86_EXTERN_FUNC(__imp__sub_000D4860);
X86_FUNC(sub_000D4860) {
    if (!game::g_active) return __imp__sub_000D4860(ctx, base);
    ctx.esp += 4;
}
