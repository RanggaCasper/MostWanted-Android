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
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "common/aspect.h"
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
// the menu: Start shows and hides it, open as it enters. D-pad up and down pick an item, A does it, B closes it
// (hidden, B leaves); a touch or a click on an item does it too (the UI thread's, taken by the next frame)
enum Item { kLive, kAddKey, kUndoKey, kClearKeys, kPlayPath, kPathSpeed, kResetCamera, kHideMenu, kExit, kItems };
std::atomic<bool> g_menu{true};
std::atomic<int> g_sel{kAddKey};
std::atomic<int> g_tap{-1};  // an item touched or clicked; with kTapLeft or kTapRight, the speed's arrows
constexpr int kTapLeft = 0x100, kTapRight = 0x200;
std::atomic<float> g_menu_rect[4] = {0, 0, 0, 0};  // its place on the screen, for the touch controls to leave it
constexpr float kSpeeds[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};

// LIVE: the race runs under the camera. The game's own events pause and resume it: EUnPause (0x19E7B0, 8 bytes,
// processed as 0x19E7C0: pops the pause package, the sim resumes) as the pause menu's Resume posts it (0x10F2AC), and
// EPause (0x19D400: port, mode, 0; 0x14 bytes; processed as 0x1A58B0: pushes Pause_Main.fng with the mode, which
// pauses the sim through cFEng's PushPackage, 0xEB00A) as the player's Start posts it (0x1087C4: 0, 0, 0). Mode 1 is
// the RESTART MENU, which has no way back and no CINEMATIC MODE. Events come off the event
// heap (0x19F9A0, cdecl: the size) and run at the next pump. While live the car gets no pad, and the range is about
// the car where it is now
constexpr uint32_t kEventAlloc = 0x19F9A0, kEUnPause = 0x19E7B0, kEPause = 0x19D400;
std::atomic<bool> g_live{false};
bool g_paused_by_us = false;  // EPause posted here: the pause menu it pushed is a live one, not reopened on leaving

// keyframes: the menu adds the camera as it is, takes the last back or all of them, and plays them as one path
struct Key {
    Vec pos;
    float yaw = 0, pitch = 0, roll = 0, fov = 70;
};
constexpr float kPathMetres = 6.0f;               // metres a second along the path at 1x, before the ease
constexpr float kPathTurn = 50.0f * kPi / 180.0f;  // and turning, radians a second
constexpr float kSegmentMin = 0.8f;               // seconds, a segment at the least
std::vector<Key> g_keys;
std::vector<float> g_segments;  // the path's segments' seconds at 1x, set as it starts
std::atomic<int> g_key_count{0};
std::atomic<bool> g_playing{false};
float g_play_t = 0;
std::atomic<float> g_play_speed{1};

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

uint32_t CallThis(X86Context& ctx, uint32_t self, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

// the game paused or resumed by its own events (see kEPause)
void PostPause(X86Context& ctx, bool pause) {
    uint32_t ev = kernel::CallGuest(ctx, kEventAlloc, {pause ? 0x14u : 8u});
    if (!ev) return;
    if (pause) CallThis(ctx, ev, kEPause, {0, 0, 0});
    else CallThis(ctx, ev, kEUnPause, {});
}

// ---- the keyframes' path: a Catmull-Rom curve through them, eased in and out over the whole of it

Key KeyOf(const Fly& f) { return {f.pos, f.yaw, f.pitch, f.roll, f.fov}; }

float CatmullRom(float a, float b, float c, float d, float s) {
    return 0.5f * (2 * b + (c - a) * s + (2 * a - 5 * b + 4 * c - d) * s * s + (3 * b - a - 3 * c + d) * s * s * s);
}

float PathSeconds() {
    float t = 0;
    for (float d : g_segments) t += d;
    return t;
}

// the segments' times, from how far each moves and turns; a key's yaw within half a turn of the one before
void PreparePath() {
    for (size_t i = 1; i < g_keys.size(); i++) {
        float& y = g_keys[i].yaw;
        while (y - g_keys[i - 1].yaw > kPi) y -= 2 * kPi;
        while (y - g_keys[i - 1].yaw < -kPi) y += 2 * kPi;
    }
    g_segments.clear();
    for (size_t i = 0; i + 1 < g_keys.size(); i++) {
        const Key &a = g_keys[i], &b = g_keys[i + 1];
        float turn = std::max({std::fabs(b.yaw - a.yaw), std::fabs(b.pitch - a.pitch), std::fabs(b.roll - a.roll)});
        g_segments.push_back(std::max({kSegmentMin, Length(b.pos - a.pos) / kPathMetres, turn / kPathTurn}));
    }
}

// the camera t seconds into the path
Key PathAt(float t) {
    float total = PathSeconds();
    float x = total > 0 ? std::clamp(t / total, 0.0f, 1.0f) : 1.0f;
    float u = (0.5f - 0.5f * std::cos(kPi * x)) * total;  // eased: it starts and stops gently
    size_t i = 0;
    while (i + 1 < g_segments.size() && u > g_segments[i]) u -= g_segments[i++];
    float s = g_segments.empty() ? 0.0f : std::clamp(u / g_segments[i], 0.0f, 1.0f);
    size_t n = g_keys.size();
    const Key &k0 = g_keys[i ? i - 1 : 0], &k1 = g_keys[i], &k2 = g_keys[std::min(i + 1, n - 1)],
              &k3 = g_keys[std::min(i + 2, n - 1)];
    auto cr = [s](float a, float b, float c, float d) { return CatmullRom(a, b, c, d, s); };
    Key k;
    k.pos = {cr(k0.pos.x, k1.pos.x, k2.pos.x, k3.pos.x), cr(k0.pos.y, k1.pos.y, k2.pos.y, k3.pos.y),
             cr(k0.pos.z, k1.pos.z, k2.pos.z, k3.pos.z)};
    k.yaw = cr(k0.yaw, k1.yaw, k2.yaw, k3.yaw);
    k.pitch = std::clamp(cr(k0.pitch, k1.pitch, k2.pitch, k3.pitch), -kPitchLimit, kPitchLimit);
    k.roll = std::clamp(cr(k0.roll, k1.roll, k2.roll, k3.roll), -kRollLimit, kRollLimit);
    k.fov = std::clamp(cr(k0.fov, k1.fov, k2.fov, k3.fov), kFovMin, kFovMax);
    return k;
}

void SetFly(const Key& k) {
    g_fly.pos = k.pos;
    g_fly.yaw = k.yaw;
    g_fly.pitch = k.pitch;
    g_fly.roll = k.roll;
    g_fly.fov = k.fov;
    g_fly.vel = {};
    g_fly.yaw_rate = g_fly.pitch_rate = g_fly.roll_rate = g_fly.fov_rate = 0;
}

// the car's middle: the reflection cube's camera, drawn about the car (see kRange)
bool CarMiddle(Vec& out) {
    uint32_t cube = kernel::R32(kViews + kCubeView * kViewSize + kViewCamera);
    if (!cube || !mem::IsCommitted(cube + kCamPosition + 12)) return false;
    out = V(cube + kCamPosition);
    return true;
}

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
    if (Vec car; CarMiddle(car) && Length(car - V(cam + kCamTarget)) < 8.0f) g_origin = car;
    g_saved_precull = kernel::R32(kPrecullMode);
    kernel::W32(kPrecullMode, 0);
    g_first_pad = true;
    g_last = std::chrono::steady_clock::now();
    g_active = true;
    g_from_menu = g_request_menu;
    g_live = false;  // the pause menu paused it
    g_paused_by_us = false;
    g_playing = false;
    g_menu = true;
}

// why: logged when it was not the player's leaving (the race ended or the camera changed under it)
void Leave(const char* why) {
    if (!g_active) return;
    if (g_camera && mem::IsCommitted(g_camera)) std::memcpy(kernel::HostPtr(g_camera), g_saved, kCamSaved);
    kernel::W32(kPrecullMode, g_saved_precull);
    g_active = false;
    g_playing = false;
    // live, it goes back to the race running; paused, to the pause menu: the one Accept left without input is opened
    // again, one EPause pushed is the menu itself. Not when the race ended under it
    g_reopen_menu = g_from_menu && !g_live && !g_paused_by_us && Racing();
    g_from_menu = false;
    g_live = false;
    g_drain_until = Now() + 1.5;
    g_drain = true;
    if (why) LOGR("cinematic: off, %s", why);
}

// a menu item done: dir 0 its A or a touch, -1 or 1 the speed's arrows. True to leave
bool Act(X86Context& ctx, int item, int dir) {
    switch (item) {
        case kLive: {  // the game's own pause and resume
            bool live = !g_live;
            PostPause(ctx, !live);
            if (!live) g_paused_by_us = true;
            g_live = live;
            break;
        }
        case kAddKey: g_keys.push_back(KeyOf(g_fly)); break;
        case kUndoKey:
            if (!g_keys.empty()) g_keys.pop_back();
            break;
        case kClearKeys: g_keys.clear(); break;
        case kPlayPath:
            if (g_keys.size() >= 2) {
                PreparePath();
                g_play_t = 0;
                g_playing = true;
            }
            break;
        case kPathSpeed: {  // A steps up and wraps round; the arrows step either way
            constexpr int n = int(std::size(kSpeeds));
            int i = 0;
            for (int k = 0; k < n; k++)
                if (std::fabs(kSpeeds[k] - g_play_speed) < std::fabs(kSpeeds[i] - g_play_speed)) i = k;
            i = dir == 0 ? (i + 1) % n : std::clamp(i + dir, 0, n - 1);
            g_play_speed = kSpeeds[i];
            break;
        }
        case kResetCamera: SetFly(KeyOf(g_start)); break;
        case kHideMenu: g_menu = false; break;
        case kExit: return true;
        default: break;
    }
    return false;
}

// a button pressed since the last frame
bool Pressed(int analog) { return !g_first_pad && g_pad.analog[analog] > 0x40 && g_prev_analog[analog] <= 0x40; }

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
        // nothing is a press until every button is up once: the menu's A that entered is held a frame or two
        // later, and the pad kept from before showed it up first
        g_prev_buttons = p.buttons;
        std::memcpy(g_prev_analog, p.analog, 8);
        bool held = p.buttons || std::any_of(std::begin(p.analog), std::end(p.analog), [](uint8_t a) { return a > 0x40; });
        if (!held) g_first_pad = false;
    }
    const bool settled = !g_first_pad;
    auto edge = [&](uint16_t bit) { return settled && (p.buttons & bit) && !(g_prev_buttons & bit); };
    const bool keys = input::KeyboardActive();
    bool leave = false;
    if (edge(0x10)) {  // Start: the menu shown or hidden; while the path plays, it stops and the menu is back
        if (g_playing) g_playing = false, g_menu = true;
        else g_menu = !g_menu.load();
    }
    int tap = g_tap.exchange(-1);
    if (g_playing) {
        if (Pressed(1)) g_playing = false;  // B stops it where it is
    } else if (g_menu) {
        int sel = g_sel;
        // D-pad up and down (the keyboard's arrows); Back (Tab, Backspace) steps down too
        if (edge(0x01)) sel = (sel + kItems - 1) % kItems;
        if (edge(0x02)) sel = (sel + 1) % kItems;
        if (edge(0x20)) sel = (sel + 1) % kItems;
        g_sel = sel;
        if (Pressed(0)) leave |= Act(ctx, sel, 0);
        if (tap >= 0) {
            g_sel = tap & 0xFF;
            leave |= Act(ctx, tap & 0xFF, tap & kTapLeft ? -1 : tap & kTapRight ? 1 : 0);
        }
        if (Pressed(1)) g_menu = false;
    } else {
        leave = Pressed(1);  // B, the menu hidden: out
    }
    g_key_count = int(g_keys.size());
    g_prev_buttons = p.buttons;
    std::memcpy(g_prev_analog, p.analog, 8);
    if (leave) {
        Leave(nullptr);
        return;
    }
    // live, the range follows the car
    if (g_live) {
        if (Vec car; CarMiddle(car)) g_origin = car;
    }

    if (g_playing) {
        // RT faster, LT slower, a quarter to four times
        g_play_speed = std::clamp(
            g_play_speed.load() * std::exp((Trigger(p.analog[7]) - Trigger(p.analog[6])) * dt * 1.2f), 0.25f, 4.0f);
        g_play_t += dt * g_play_speed;
        SetFly(PathAt(g_play_t));
        if (g_play_t >= PathSeconds()) g_playing = false;  // stays on the last key
        WriteCamera(ctx, g_camera, g_fly, dt);
        return;
    }

    Fly& f = g_fly;
    // the look slows as it zooms in, so a long lens aims as finely as a wide one
    float zoom = f.fov / 70.0f;
    float look = kLookSpeed * kPi / 180.0f * std::clamp(zoom, 0.15f, 1.2f);
    // the D-pad: on a controller the roll; from the keyboard (the arrows are its D-pad) the look too while the menu is
    // hidden, beside the right stick's own keys (the numpad, T F G H)
    float dpad_x = (p.buttons & 0x08 ? 1.0f : 0.0f) - (p.buttons & 0x04 ? 1.0f : 0.0f);
    float dpad_y = (p.buttons & 0x01 ? 1.0f : 0.0f) - (p.buttons & 0x02 ? 1.0f : 0.0f);
    const bool arrows_look = keys && !g_menu;  // open, the arrows work the menu; the look stays on its own keys
    float look_x = std::clamp(Stick(p.rx) + (arrows_look ? dpad_x : 0.0f), -1.0f, 1.0f);
    float look_y = std::clamp(Stick(p.ry) + (arrows_look ? dpad_y : 0.0f), -1.0f, 1.0f);
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
bool CinematicClean() { return g_active && (!g_menu || g_playing); }

// the pad filter, before the game sees it: true while the pad is the camera's (or still held after leaving)
bool CinematicPad(int port, const input::Pad& pad) {
    if (port != 0) return g_active || g_drain;
    {
        // kept always, so it is the pad as it is when the camera takes it, not one from its last time
        std::lock_guard lock(g_lock);
        g_pad = pad;
    }
    if (g_active) return true;
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

// the menu at the left, in the pause menu's colours, and under it the camera's own controls; nothing while hidden or
// while the path plays, so the picture is the game's alone. Its place is kept for the touch controls
void DrawCinematicHud() {
    const bool shown = g_active && g_menu && !g_playing && !app::MenuOpen();
    if (!shown) {
        g_menu_rect[2] = 0;
        return;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    // in the front end's units, 480 down the screen, so it is the size of the game's own menus on any screen
    const float u = screen.y / 480.0f;
    auto col = [](int r, int g, int b, int a) { return IM_COL32(r, g, b, a); };
    const ImU32 amber = col(236, 156, 40, 255), bar = col(196, 124, 28, 165), panel = col(22, 16, 8, 190),
                stripe = col(255, 200, 120, 14), cream = col(255, 238, 208, 255), dim = col(150, 135, 110, 200),
                bracket = col(240, 228, 205, 230), hollow = col(0, 0, 0, 110), ink = col(30, 20, 8, 255),
                pick = col(196, 124, 28, 150);
    auto width = [font](float px, const std::string& s) { return font->CalcTextSizeA(px, FLT_MAX, 0, s.c_str()).x; };
    auto text = [&](float px, ImVec2 c, ImU32 colour, const std::string& s) {  // centred on c
        dl->AddText(font, px, ImVec2(c.x - width(px, s) / 2, c.y - px / 2), colour, s.c_str());
    };

    // the items: what each says, what it shows at the right, whether it can be done now
    const int keys = g_key_count, sel = g_sel;
    const bool live = g_live;
    char speed[16];
    std::snprintf(speed, sizeof speed, "%gx", double(g_play_speed.load()));
    struct Row {
        const char* label;
        std::string value;
        bool enabled;
    };
    const Row rows[kItems] = {
        {"LIVE", app::Ui(live ? "ON" : "OFF"), true},
        {"ADD KEYFRAME", "", true},
        {"UNDO KEYFRAME", "", keys > 0},
        {"CLEAR KEYFRAMES", std::to_string(keys), keys > 0},
        {"PLAY PATH", "", keys >= 2},
        {"PATH SPEED", speed, true},
        {"RESET CAMERA", "", true},
        {"HIDE MENU", "", true},
        {"EXIT", "", true},
    };
    const float label_px = 13 * u, pad = 12 * u, title_h = 26 * u, row_h = 21 * u, arrow = 9 * u;
    float w = 180 * u;
    for (const Row& r : rows) w = std::max(w, width(label_px, app::Ui(r.label)) + width(label_px, r.value) + 2 * arrow + pad * 3);
    const std::string title = app::Ui("CINEMATIC MODE");
    w = std::max(w, width(16 * u, title) + 20 * u + pad * 3);
    const float h = title_h + row_h * float(kItems) + 8 * u;
    // a margin in from the picture's own edge: the presenter letterboxes the frame to the game's aspect, and a bar
    // beside it is the window's, not the picture's
    const float aspect = Aspect(), view_w = std::min(screen.x, screen.y * aspect), view_h = view_w / aspect;
    const float view_x = (screen.x - view_w) / 2, view_y = (screen.y - view_h) / 2;
    const ImVec2 p0(view_x + 34 * u, view_y + std::max(16 * u, (view_h - h - 70 * u) / 2)), p1(p0.x + w, p0.y + h);
    const float split = p0.y + title_h;
    g_menu_rect[0] = p0.x - 6 * u, g_menu_rect[1] = p0.y - 6 * u, g_menu_rect[3] = p1.y + 6 * u;
    g_menu_rect[2] = p1.x + 6 * u;

    // the title bar: the camera's icon and the mode's name
    dl->AddRectFilled(p0, ImVec2(p1.x, split), bar);
    dl->AddLine(ImVec2(p0.x, split), ImVec2(p1.x, split), amber, 1.2f * u);
    {
        float s = 18 * u, x = p0.x + pad, y = p0.y + (title_h - s) / 2;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), amber, 3 * u);
        ImU32 icon = col(40, 26, 8, 255);
        dl->AddRectFilled(ImVec2(x + s * 0.16f, y + s * 0.34f), ImVec2(x + s * 0.84f, y + s * 0.78f), icon, 2 * u);
        dl->AddRectFilled(ImVec2(x + s * 0.34f, y + s * 0.24f), ImVec2(x + s * 0.56f, y + s * 0.36f), icon, 1 * u);
        dl->AddCircleFilled(ImVec2(x + s * 0.5f, y + s * 0.56f), s * 0.17f, amber, 20);
        dl->AddCircleFilled(ImVec2(x + s * 0.5f, y + s * 0.56f), s * 0.09f, icon, 16);
        float px = 16 * u, tx = x + s + 8 * u, ty = p0.y + (title_h - px) / 2;
        dl->AddText(font, px, ImVec2(tx + u, ty + u), col(0, 0, 0, 160), title.c_str());
        dl->AddText(font, px, ImVec2(tx, ty), cream, title.c_str());
    }

    // the items, striped behind as the game's dialogs; a mouse over one picks it, a click or a touch does it
    dl->AddRectFilled(ImVec2(p0.x, split), p1, panel);
    dl->PushClipRect(ImVec2(p0.x, split), p1, true);
    for (float x = p0.x - h; x < p1.x; x += 7 * u)
        dl->AddLine(ImVec2(x, p1.y), ImVec2(x + (p1.y - split), split), stripe, 2.5f * u);
    dl->PopClipRect();
    const ImVec2 m = io.MousePos;
    const bool mouse = io.MouseSource == ImGuiMouseSource_Mouse;
    for (int i = 0; i < kItems; i++) {
        const Row& r = rows[i];
        ImVec2 a(p0.x, split + 4 * u + row_h * float(i)), b(p1.x, a.y + row_h);
        bool over = m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
        if (over && mouse && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) g_sel = i;
        if (over && ImGui::IsMouseClicked(0)) {
            int tap = i;
            if (i == kPathSpeed && m.x < b.x - pad - arrow - width(label_px, r.value) - arrow / 2) tap |= kTapLeft;
            else if (i == kPathSpeed && m.x > b.x - pad - arrow) tap |= kTapRight;
            g_tap = tap;
        }
        const bool picked = i == sel;
        const float cy = (a.y + b.y) / 2;
        if (picked) {
            dl->AddRectFilled(ImVec2(a.x + 3 * u, a.y + u), ImVec2(b.x - 3 * u, b.y - u), pick, 2 * u);
            // the pick's marker, as the options' rows have
            dl->AddTriangleFilled(ImVec2(a.x + 6 * u, cy - 4 * u), ImVec2(a.x + 6 * u, cy + 4 * u),
                                  ImVec2(a.x + 11 * u, cy), cream);
        }
        const ImU32 ink_label = r.enabled ? cream : dim;
        const std::string label = app::Ui(r.label);
        dl->AddText(font, label_px, ImVec2(a.x + pad + 4 * u, cy - label_px / 2), ink_label, label.c_str());
        if (!r.value.empty()) {
            float vx = b.x - pad - width(label_px, r.value);
            if (i == kPathSpeed) {  // its arrows either side
                vx -= arrow;
                float lx = vx - arrow, rx = b.x - pad - arrow * 0.6f;
                dl->AddTriangleFilled(ImVec2(lx + arrow * 0.7f, cy - 4 * u), ImVec2(lx + arrow * 0.7f, cy + 4 * u),
                                      ImVec2(lx, cy), amber);
                dl->AddTriangleFilled(ImVec2(rx, cy - 4 * u), ImVec2(rx, cy + 4 * u), ImVec2(rx + arrow * 0.7f, cy),
                                      amber);
            }
            ImU32 vcol = i == kLive && live ? ink : picked ? cream : amber;
            if (i == kLive) {  // ON lit as the game's toggles, OFF hollow
                float vw = width(label_px, r.value) + 10 * u;
                ImVec2 v0(b.x - pad - vw, cy - 8 * u), v1(b.x - pad, cy + 8 * u);
                if (live) dl->AddRectFilled(v0, v1, amber, 8 * u);
                else dl->AddRect(v0, v1, cream, 8 * u, 0, 1.2f * u);
                text(label_px, ImVec2((v0.x + v1.x) / 2, cy), live ? ink : cream, r.value);
            } else {
                dl->AddText(font, label_px, ImVec2(vx, cy - label_px / 2), r.enabled ? vcol : dim, r.value.c_str());
            }
        }
    }
    // the corner brackets about it, as about the pause menu
    {
        const float o = 4 * u, l = 11 * u, bt = 1.6f * u;
        for (int i = 0; i < 4; i++) {
            float sx = i & 1 ? 1.0f : -1.0f, sy = i & 2 ? 1.0f : -1.0f;
            ImVec2 c(i & 1 ? p1.x + o : p0.x - o, i & 2 ? p1.y + o : p0.y - o);
            dl->AddLine(c, ImVec2(c.x - sx * l, c.y), bracket, bt);
            dl->AddLine(c, ImVec2(c.x, c.y - sy * l), bracket, bt);
        }
    }

    // under it, the camera's controls, which stay on the pad
    enum class Glyph { Face, Pill, Stick, Dpad };
    struct Button {
        Glyph glyph;
        const char* label;  // a D-pad's: the arms lit, U up, D down, LR left and right
    };
    struct Prompt {
        std::vector<Button> keys;
        const char* action;
    };
    static const std::vector<Prompt> hints[] = {
        {{{{Glyph::Stick, "L"}}, "MOVE"}, {{{Glyph::Stick, "R"}}, "LOOK"}},
        {{{{Glyph::Pill, "LT"}, {Glyph::Pill, "RT"}}, "ZOOM"}, {{{Glyph::Pill, "LB"}, {Glyph::Pill, "RB"}}, "HEIGHT"}},
        {{{{Glyph::Dpad, "LR"}}, "ROLL"}, {{{Glyph::Pill, "R3"}}, "LEVEL"}, {{{Glyph::Pill, "L3"}}, "FAST"}},
        {{{{Glyph::Dpad, "UD"}, {Glyph::Pill, "BACK"}}, "NEXT ITEM"}, {{{Glyph::Face, "A"}}, "SELECT"}},
        {{{{Glyph::Pill, "START"}}, "HIDE"}, {{{Glyph::Face, "B"}}, "CLOSE"}},
    };
    const float gh = 13 * u, hint_px = 11 * u, key_px = 8.5f * u, gap = 4 * u, item_gap = 11 * u, line_h = 17 * u;
    auto glyph_w = [&](const Button& k) {
        return k.glyph == Glyph::Pill ? std::max(gh * 1.75f, width(key_px, k.label) + gh * 0.8f) : gh;
    };
    auto draw_key = [&](const Button& k, ImVec2 c) {
        float r = gh / 2, th = 1.3f * u;
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
                text(key_px, c, ink, k.label);
                break;
            case Glyph::Dpad: {
                // the cross dim, the arms that do it lit
                float a = r, b = r * 0.36f;
                const ImU32 off = col(120, 84, 30, 255);
                for (ImVec4 q : {ImVec4(-a, -b, a, b), ImVec4(-b, -a, b, a)})
                    dl->AddRectFilled(ImVec2(c.x + q.x, c.y + q.y), ImVec2(c.x + q.z, c.y + q.w), off, b * 0.4f);
                std::string lit = k.label;
                if (lit.find('U') != std::string::npos)
                    dl->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y - b), amber, b * 0.4f);
                if (lit.find('D') != std::string::npos)
                    dl->AddRectFilled(ImVec2(c.x - b, c.y + b), ImVec2(c.x + b, c.y + a), amber, b * 0.4f);
                if (lit.find('L') != std::string::npos)
                    dl->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x - b, c.y + b), amber, b * 0.4f);
                if (lit.find('R') != std::string::npos)
                    dl->AddRectFilled(ImVec2(c.x + b, c.y - b), ImVec2(c.x + a, c.y + b), amber, b * 0.4f);
                dl->AddCircleFilled(c, b * 0.55f, ink, 12);
                break;
            }
        }
    };
    float hints_w = 0;
    auto line_w = [&](const std::vector<Prompt>& line) {
        float lw = -item_gap;
        for (const Prompt& pr : line) {
            for (const Button& k : pr.keys) lw += glyph_w(k) + 2.5f * u;
            lw += gap + width(hint_px, app::Ui(pr.action)) + item_gap;
        }
        return lw;
    };
    for (const auto& line : hints) hints_w = std::max(hints_w, line_w(line));
    const ImVec2 h0(p0.x, p1.y + 10 * u), h1(p0.x + std::max(w, hints_w + pad * 2), h0.y + line_h * float(std::size(hints)) + 8 * u);
    dl->AddRectFilled(h0, h1, col(10, 8, 4, 150), 3 * u);
    for (size_t li = 0; li < std::size(hints); li++) {
        float x = h0.x + pad, cy = h0.y + 4 * u + line_h * (float(li) + 0.5f);
        for (const Prompt& pr : hints[li]) {
            for (const Button& k : pr.keys) {
                draw_key(k, ImVec2(x + glyph_w(k) / 2, cy));
                x += glyph_w(k) + 2.5f * u;
            }
            x += gap;
            const std::string action = app::Ui(pr.action);
            dl->AddText(font, hint_px, ImVec2(x, cy - hint_px / 2), cream, action.c_str());
            x += width(hint_px, action) + item_gap;
        }
    }
}

bool CinematicMenuHit(float x, float y) {
    return g_menu_rect[2] > 0 && x >= g_menu_rect[0] && x < g_menu_rect[2] && y >= g_menu_rect[1] && y < g_menu_rect[3];
}

}  // namespace game

// the frame's render: the request taken, the camera stepped and set before any view's projection
X86_EXTERN_FUNC(__imp__sub_000D8720);
X86_FUNC(sub_000D8720) {
    game::ReplayFrame(ctx);  // records the race, or plays it again (replay.cpp)
    if (game::g_reopen_menu) {
        game::g_reopen_menu = false;
        if (uint32_t feng = kernel::R32(game::kFEng)) {
            uint32_t ecx = ctx.ecx;
            ctx.ecx = feng;
            kernel::CallGuest(ctx, game::kSwitchPackage, {game::kPauseMainFng, 0, 0, 0});
            ctx.ecx = ecx;
        }
    }
    if (game::g_request.exchange(false) && !game::g_active && !game::ReplayActive()) {
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
// dword at 0x427A74 (read only at 0x0D85EC, never written) is set. The replay hides them the same way
X86_EXTERN_FUNC(__imp__sub_000D82D0);
X86_FUNC(sub_000D82D0) {
    if (!game::g_active && !game::ReplayActive()) return __imp__sub_000D82D0(ctx, base);
    constexpr uint32_t kDrawFEng = 0x427A74;
    uint32_t draw = kernel::R32(kDrawFEng);
    kernel::W32(kDrawFEng, 0);
    __imp__sub_000D82D0(ctx, base);
    kernel::W32(kDrawFEng, draw);
}

X86_EXTERN_FUNC(__imp__sub_000D4860);
X86_FUNC(sub_000D4860) {
    if (!game::g_active && !game::ReplayActive()) return __imp__sub_000D4860(ctx, base);
    ctx.esp += 4;
}
