// REPLAY: the race watched again, the last minute of it or all of it, from the pause menu's icon (or the panel's
// WATCH REPLAY, once the race is over). The Xbox game has no replays: the cars are recorded here as they are drawn,
// and drawn again from the recording with the camera ours.
//
// How a car is drawn (decompiled): each car of the race has a render connection whose update (0x25CC70, thiscall:
// the sim's packet and dt, from the virtual 0x25CE20) works out what its render (0x257E70, per view) hands
// CarRenderInfo::Render (0x255490; the info at +0x44): the body's matrix at +0x330 (row vectors, row 3 the place,
// which the render takes out as the position the car is culled and its LOD picked by), and the four tires' matrices
// at +0x130 and the four brakes' at +0x230, relative to the body. So that block written back after the update draws
// the car where it was. The connections are not in a list the host can walk (the one at 0x4D5ED0, which 0x2579E0
// draws, is empty in a race): each is taken as its update runs. The body is kept as a rotation and a place, eased
// between samples; the wheels' matrices the nearest sample's (a wheel turns more than half a turn between samples;
// eased, it shrank).
//
// Recorded 30 times a second of the race's own time: only while it runs (cFEng's pause count, 0x48E954, at 0), so the
// pause menu and the replay itself are not in it; cars more than kRecordRange from the player's are left out. A
// new race (the game flow back to racing) starts a new recording.
//
// Played, the game is paused under it (as the pause menu left it) or, once the race is over, runs on underneath:
// either way each car's update is followed by the recorded block, and each frame starts with it too. A car not in the
// recording at that moment (traffic come since) is put far below the world, where it is culled. The camera is set as
// cinematic mode sets it (Camera::SetCameraMatrix on the player's view's camera), the menus and HUD hidden the same
// way (cinematic.cpp's hooks ask ReplayActive). Leaving gives back each car's block, the camera, the precull.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
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

constexpr uint32_t kConnRenderInfo = 0x44, kConnWheels = 0x130, kConnBody = 0x330;
constexpr uint32_t kConnBlock = kConnBody + 0x40 - kConnWheels;  // the wheels' and the body's matrices
constexpr int kWheels = 8;  // the four tires, then the four brakes
constexpr uint32_t kPauseDepth = 0x48E954;
constexpr uint32_t kViews = 0x48AC80, kViewSize = 0x70, kPlayerView = 1, kCubeView = 0x12, kViewCamera = 0x40;
constexpr uint32_t kCamPosition = 0x40, kCamNoise = 0x70, kCamNoiseEnd = 0xB0, kCamFov = 0xC4, kCamSaved = 0x2B0;
constexpr uint32_t kSetCameraMatrix = 0x599E0;
constexpr uint32_t kPrecullMode = 0x469740;
constexpr uint32_t kFEng = 0x48E94C, kSwitchPackage = 0xEB160, kPauseMainFng = 0x3D0618;

constexpr double kSampleRate = 30;
constexpr float kRecordRange = 300;            // metres from the player's car
constexpr size_t kMaxBytes = 64u << 20;        // the whole race's recording; the oldest go past it
constexpr double kInstantSeconds = 60;         // the last minute
constexpr float kHidden = -100000;             // a car not in the recording, put under the world
constexpr float kFovUnit = 65536.0f / 360.0f;  // the Camera's 16 bit angle
constexpr float kPi = 3.14159265f;

struct Vec {
    float x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Normal(Vec a, Vec fallback = {1, 0, 0}) {
    float l = Length(a);
    return l > 1e-6f ? a * (1.0f / l) : fallback;
}
Vec Lerp(Vec a, Vec b, float s) { return a + (b - a) * s; }

// a 4x4 of row vectors, as the game keeps them
struct Mat {
    float m[16];
};
// the 3x3 as a quaternion and back: one convention both ways, so a round trip is the matrix again
struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
};
Quat QuatOf(const Mat& a) {
    const float* m = a.m;
    float tr = m[0] + m[5] + m[10];
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1) * 2;
        q = {s / 4, (m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s};
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = std::sqrt(1 + m[0] - m[5] - m[10]) * 2;
        q = {(m[6] - m[9]) / s, s / 4, (m[4] + m[1]) / s, (m[8] + m[2]) / s};
    } else if (m[5] > m[10]) {
        float s = std::sqrt(1 + m[5] - m[0] - m[10]) * 2;
        q = {(m[8] - m[2]) / s, (m[4] + m[1]) / s, s / 4, (m[9] + m[6]) / s};
    } else {
        float s = std::sqrt(1 + m[10] - m[0] - m[5]) * 2;
        q = {(m[1] - m[4]) / s, (m[8] + m[2]) / s, (m[9] + m[6]) / s, s / 4};
    }
    return q;
}
Mat MatOf(const Quat& q, Vec t) {
    float w = q.w, x = q.x, y = q.y, z = q.z;
    Mat r{};
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + w * z);
    r.m[2] = 2 * (x * z - w * y);
    r.m[4] = 2 * (x * y - w * z);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + w * x);
    r.m[8] = 2 * (x * z + w * y);
    r.m[9] = 2 * (y * z - w * x);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = t.x, r.m[13] = t.y, r.m[14] = t.z, r.m[15] = 1;
    return r;
}
Quat Slerp(Quat a, Quat b, float s) {
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) b = {-b.w, -b.x, -b.y, -b.z}, d = -d;
    float ka = 1 - s, kb = s;
    if (d < 0.9995f) {
        float th = std::acos(d), sn = std::sin(th);
        ka = std::sin((1 - s) * th) / sn;
        kb = std::sin(s * th) / sn;
    }
    Quat q{a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb};
    float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return l > 0 ? Quat{q.w / l, q.x / l, q.y / l, q.z / l} : a;
}

float F(uint32_t va) { return kernel::G<float>(va); }
Vec V(uint32_t va) { return {F(va), F(va + 4), F(va + 8)}; }
void SetF(uint32_t va, float f) { kernel::G<float>(va) = f; }
Mat ReadMat(uint32_t va) {
    Mat r;
    std::memcpy(r.m, kernel::HostPtr(va), sizeof r.m);
    return r;
}
void WriteMat(uint32_t va, const Mat& m) { std::memcpy(kernel::HostPtr(va), m.m, sizeof m.m); }

// ---- the recording

struct CarSample {
    uint32_t conn, info;  // which car: its connection and its CarRenderInfo, together
    Quat rot;             // the body's matrix
    Vec place;
    float wheels[kWheels][12];  // each relative to the body, its 3 columns of 4 rows
};
struct Frame {
    double t;      // the race's seconds
    size_t first;  // its cars in g_samples
    uint16_t count;
    int16_t player;  // the player's car among them, -1 when not found
};

std::recursive_mutex g_lock;  // the recording and the playback's state: the game thread's, read by the UI thread's HUD
std::vector<Frame> g_frames;
std::vector<CarSample> g_samples;
double g_clock = 0, g_next_sample = 0;
std::chrono::steady_clock::time_point g_last_tick;
bool g_was_racing = false;
// the connections as their updates ran: the race clock then. One updated in the last half second is a car
struct Seen {
    uint32_t info;
    double clock;
};
std::unordered_map<uint32_t, Seen> g_conns;
constexpr double kAlive = 0.5;

void Clear() {
    g_frames.clear();
    g_samples.clear();
    g_clock = g_next_sample = 0;
    g_conns.clear();
}

bool Committed(uint32_t va, uint32_t size) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1); }

// each car's connection, updated lately and still its car's
template <class Fn>
void ForEachConn(Fn&& fn) {
    for (const auto& [conn, seen] : g_conns)
        if (seen.clock >= g_clock - kAlive && Committed(conn, kConnBody + 0x40) &&
            kernel::R32(conn + kConnRenderInfo) == seen.info)
            fn(conn, seen.info);
}

// the player's car's middle: the reflection cube's camera, drawn about it (as cinematic.cpp's CarMiddle)
bool PlayerMiddle(Vec& out) {
    uint32_t cam = kernel::R32(kViews + kCubeView * kViewSize + kViewCamera);
    if (!Committed(cam, kCamPosition + 12)) return false;
    out = V(cam + kCamPosition);
    return true;
}

void Sample() {
    Vec player;
    bool have_player = PlayerMiddle(player);
    Frame f{g_clock, g_samples.size(), 0, -1};
    float nearest = 1e9f;
    for (auto it = g_conns.begin(); it != g_conns.end();)  // gone a while: forgotten
        it = it->second.clock < g_clock - 2 ? g_conns.erase(it) : std::next(it);
    ForEachConn([&](uint32_t conn, uint32_t info) {
        Mat body = ReadMat(conn + kConnBody);
        Vec place{body.m[12], body.m[13], body.m[14]};
        float far = have_player ? Length(place - player) : 0;
        if (far > kRecordRange) return;
        CarSample s;
        s.conn = conn;
        s.info = info;
        s.rot = QuatOf(body);
        s.place = place;
        for (int w = 0; w < kWheels; w++) {
            Mat local = ReadMat(conn + kConnWheels + uint32_t(w) * 0x40);
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 3; c++) s.wheels[w][r * 3 + c] = local.m[r * 4 + c];
        }
        if (have_player && far < nearest && far < 6) nearest = far, f.player = int16_t(f.count);
        g_samples.push_back(s);
        f.count++;
    });
    g_frames.push_back(f);
    // past the budget: the oldest tenth goes
    if (g_samples.size() * sizeof(CarSample) > kMaxBytes && g_frames.size() > 10) {
        size_t drop = g_frames.size() / 10, gone = g_frames[drop].first;
        g_frames.erase(g_frames.begin(), g_frames.begin() + long(drop));
        g_samples.erase(g_samples.begin(), g_samples.begin() + long(gone));
        for (Frame& fr : g_frames) fr.first -= gone;
    }
}

const CarSample* Find(const Frame& f, uint32_t conn, uint32_t info) {
    for (size_t i = f.first; i < f.first + f.count; i++)
        if (g_samples[i].conn == conn && g_samples[i].info == info) return &g_samples[i];
    return nullptr;
}

// the frame at or before t, and how far on to the next
size_t FrameAt(double t, float& s) {
    auto it = std::upper_bound(g_frames.begin(), g_frames.end(), t, [](double v, const Frame& f) { return v < f.t; });
    size_t i = it == g_frames.begin() ? 0 : size_t(it - g_frames.begin()) - 1;
    s = 0;
    if (i + 1 < g_frames.size()) {
        double span = g_frames[i + 1].t - g_frames[i].t;
        s = span > 0 ? float(std::clamp((t - g_frames[i].t) / span, 0.0, 1.0)) : 0;
    }
    return i;
}

// a car's pose at t, eased between the samples about it
struct Pose {
    Mat body;
    const CarSample* wheels;  // the nearest sample's
};
bool PoseAt(double t, uint32_t conn, uint32_t info, Pose& out) {
    if (g_frames.empty()) return false;
    float s;
    size_t i = FrameAt(t, s);
    const CarSample* a = Find(g_frames[i], conn, info);
    const CarSample* b = i + 1 < g_frames.size() ? Find(g_frames[i + 1], conn, info) : nullptr;
    if (!a && !b) return false;
    if (!a) a = b, s = 0;
    if (!b) b = a, s = 0;
    out.body = MatOf(Slerp(a->rot, b->rot, s), Lerp(a->place, b->place, s));
    out.wheels = s < 0.5f ? a : b;
    return true;
}

void WritePose(uint32_t conn, const Pose& p) {
    WriteMat(conn + kConnBody, p.body);
    for (int w = 0; w < kWheels; w++) {
        Mat local{};
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 3; c++) local.m[r * 4 + c] = p.wheels->wheels[w][r * 3 + c];
            local.m[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
        }
        WriteMat(conn + kConnWheels + uint32_t(w) * 0x40, local);
    }
}

void HideConn(uint32_t conn) { SetF(conn + kConnBody + 0x38, kHidden); }

// ---- the playback

enum CameraMode { kChase, kTv, kFree, kCameraModes };
const char* const kCameraNames[] = {"CHASE", "TV", "FREE"};
enum Item { kPlay, kRestart, kSpeed, kCameraItem, kTarget, kLength, kHideMenu, kExit, kItems };
constexpr float kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

std::atomic<bool> g_request{false}, g_request_menu{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_drain{false};  // left: the game sees no pad until all of it is let go
std::atomic<double> g_drain_until{0};
bool g_from_menu = false, g_reopen_menu = false;
uint32_t g_camera = 0;
uint8_t g_saved_camera[kCamSaved];
uint32_t g_saved_precull = 0;
uint32_t g_matrix = 0;  // guest memory for the matrix handed to SetCameraMatrix
struct SavedConn {
    uint32_t conn, info;
    uint8_t block[kConnBlock];
};
std::vector<SavedConn> g_saved_conns;

// the cars to follow: each one recorded, the player's first
struct Target {
    uint32_t conn, info;
};
std::vector<Target> g_targets;
int g_fwd_row = 0;  // which of the body's rows is the car's forward, and which way
float g_fwd_sign = 1;

double g_t = 0, g_begin = 0, g_end = 0;  // the replay's place in the race's seconds, and its span
std::atomic<bool> g_playing{true};
std::atomic<int> g_speed{2};  // in kSpeeds
std::atomic<int> g_cam_mode{kChase};
std::atomic<int> g_target{0}, g_target_count{0};
std::atomic<bool> g_whole{false};  // the whole race, or the last minute
std::atomic<bool> g_menu{true};
std::atomic<int> g_sel{kPlay};
std::atomic<int> g_tap{-1};  // a row touched or clicked; with kTapLeft or kTapRight, its arrows
constexpr int kTapLeft = 0x100, kTapRight = 0x200;
std::atomic<float> g_seek{-1};  // the timeline touched: where, 0 to 1
std::atomic<float> g_drag_x{0}, g_drag_y{0};  // a drag on the picture, pixels, the free camera's turn
std::atomic<float> g_shown_t{0}, g_shown_len{0};  // for the HUD: seconds in, and the span
std::atomic<float> g_menu_rect[4] = {0, 0, 0, 0};
std::atomic<float> g_screen_h{720};  // the HUD's, for the free camera's drag
bool g_snap = true;  // the camera jumps to where it should be, not eased there

input::Pad g_pad;  // the latest pad
uint16_t g_prev_buttons = 0;
uint8_t g_prev_analog[8] = {};
bool g_first_pad = true;
std::chrono::steady_clock::time_point g_last;

// the camera between frames
Vec g_cam_pos, g_cam_look;
float g_cam_fov = 70;
Vec g_tv_pos;
bool g_tv_valid = false;
double g_tv_since = 0;          // the replay's time the TV camera was put there
constexpr float kTvAhead = 32;  // metres along the car's way
float g_tv_side = 1;
float g_orbit_yaw = 0, g_orbit_pitch = 0.25f, g_orbit_dist = 7;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t PlayerCamera() {
    uint32_t cam = kernel::R32(kViews + kPlayerView * kViewSize + kViewCamera);
    return Committed(cam, kCamSaved) ? cam : 0;
}

uint32_t CallThis(X86Context& ctx, uint32_t self, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

// the world to view matrix of a camera at pos looking along fwd, level, handed to the game as a camera mover does
void WriteCamera(X86Context& ctx, Vec pos, Vec fwd, float fov, float dt) {
    if (!g_matrix) g_matrix = mem::AllocSystem(64);
    if (!g_matrix || !g_camera) return;
    fwd = Normal(fwd);
    Vec right = Normal(Cross(fwd, {0, 0, 1}), {0, 1, 0});
    Vec down = Cross(right, fwd) * -1.0f;
    const Vec cols[3] = {right, down, fwd};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            const Vec& c = cols[col];
            SetF(g_matrix + uint32_t(row * 16 + col * 4), row == 0 ? c.x : row == 1 ? c.y : c.z);
        }
        SetF(g_matrix + uint32_t(row * 16 + 12), 0);
    }
    SetF(g_matrix + 0x30, -Dot(pos, right));
    SetF(g_matrix + 0x34, -Dot(pos, down));
    SetF(g_matrix + 0x38, -Dot(pos, fwd));
    SetF(g_matrix + 0x3C, 1);
    for (uint32_t o = kCamNoise + 0x10; o < kCamNoiseEnd; o += 0x20)
        for (uint32_t k = 0; k < 16; k += 4) SetF(g_camera + o + k, 0);
    kernel::G<uint16_t>(g_camera + kCamFov) = uint16_t(std::lround(std::clamp(fov, 8.0f, 110.0f) * kFovUnit));
    float step = std::max(dt, 1.0f / 240);
    uint32_t bits;
    std::memcpy(&bits, &step, 4);
    CallThis(ctx, g_camera, kSetCameraMatrix, {g_matrix, bits});
}

Vec Forward(const Mat& body) {
    const float* r = body.m + g_fwd_row * 4;
    return Normal(Vec{r[0], r[1], r[2]} * g_fwd_sign);
}

// which row of the body's matrix points the way the player's car went
void FindForward() {
    float sum[3] = {};
    const CarSample* last = nullptr;
    const Target& player = g_targets.front();
    for (const Frame& f : g_frames) {
        const CarSample* s = Find(f, player.conn, player.info);
        if (!s) continue;
        if (last) {
            Vec v = s->place - last->place;
            Mat m = MatOf(s->rot, {});
            for (int r = 0; r < 3; r++) sum[r] += Dot(v, Vec{m.m[r * 4], m.m[r * 4 + 1], m.m[r * 4 + 2]});
        }
        last = s;
    }
    g_fwd_row = 0;
    for (int r = 1; r < 3; r++)
        if (std::fabs(sum[r]) > std::fabs(sum[g_fwd_row])) g_fwd_row = r;
    g_fwd_sign = sum[g_fwd_row] < 0 ? -1.0f : 1.0f;
}

void SetSpan() {
    g_end = g_frames.back().t;
    g_begin = g_whole ? g_frames.front().t : std::max(g_frames.front().t, g_end - kInstantSeconds);
}

void Enter() {
    uint32_t cam = PlayerCamera();
    std::lock_guard lock(g_lock);
    if (!cam || !Racing() || g_frames.size() < size_t(kSampleRate)) {
        LOGR("replay: not entered, camera %08X racing %d, %zu frames", cam, Racing(), g_frames.size());
        return;
    }
    // the cars to follow: the player's (the last frame's), then the others the last frame has
    g_targets.clear();
    const Frame& last = g_frames.back();
    if (last.player >= 0) {
        const CarSample& s = g_samples[last.first + size_t(last.player)];
        g_targets.push_back({s.conn, s.info});
    }
    for (size_t i = last.first; i < last.first + last.count; i++)
        if (g_targets.empty() || g_samples[i].conn != g_targets.front().conn)
            g_targets.push_back({g_samples[i].conn, g_samples[i].info});
    if (g_targets.empty()) {
        LOGR("replay: not entered, no car recorded");
        return;
    }
    FindForward();
    g_camera = cam;
    std::memcpy(g_saved_camera, kernel::HostPtr(cam), kCamSaved);
    g_saved_precull = kernel::R32(kPrecullMode);
    kernel::W32(kPrecullMode, 0);
    g_saved_conns.clear();
    ForEachConn([](uint32_t conn, uint32_t info) {
        SavedConn s{conn, info, {}};
        std::memcpy(s.block, kernel::HostPtr(conn + kConnWheels), kConnBlock);
        g_saved_conns.push_back(s);
    });
    g_target = 0;
    g_target_count = int(g_targets.size());
    SetSpan();
    g_t = g_begin;
    g_playing = true;
    g_menu = true;
    g_sel = kPlay;
    g_snap = true;
    g_tv_valid = false;
    g_first_pad = true;
    g_last = std::chrono::steady_clock::now();
    g_from_menu = g_request_menu;
    g_active = true;
    LOGR("replay: on, %.1f s of %.1f, %d cars, forward row %d%s", g_end - g_begin, g_end - g_frames.front().t,
         int(g_targets.size()), g_fwd_row, g_fwd_sign < 0 ? " backward" : "");
}

void Leave(const char* why) {
    if (!g_active) return;
    // each car as it was, where it still is the same car
    ForEachConn([](uint32_t conn, uint32_t info) {
        for (const SavedConn& s : g_saved_conns)
            if (s.conn == conn && s.info == info) std::memcpy(kernel::HostPtr(conn + kConnWheels), s.block, kConnBlock);
    });
    g_saved_conns.clear();
    if (g_camera && Committed(g_camera, kCamSaved)) std::memcpy(kernel::HostPtr(g_camera), g_saved_camera, kCamSaved);
    kernel::W32(kPrecullMode, g_saved_precull);
    g_active = false;
    g_reopen_menu = g_from_menu && Racing();
    g_from_menu = false;
    g_drain_until = Now() + 1.5;
    g_drain = true;
    if (why) LOGR("replay: off, %s", why);
}

// every car drawn as the recording has it at g_t: paused, no update runs to do it (and none frees a car); running,
// each update does it (ReplayConnUpdated), and a car freed since is not written
void ApplyCars() {
    if (kernel::R32(kPauseDepth) == 0) return;
    ForEachConn([](uint32_t conn, uint32_t info) {
        Pose p;
        if (PoseAt(g_t, conn, info, p)) WritePose(conn, p);
        else HideConn(conn);
    });
}

float Trigger(uint8_t v) { return v > 24 ? float(v - 24) / 231.0f : 0.0f; }
float Stick(int16_t v) {
    float f = std::clamp(float(v) / 32767.0f, -1.0f, 1.0f);
    float a = std::fabs(f);
    if (a < 0.16f) return 0;
    a = (a - 0.16f) / 0.84f;
    return std::copysign(a * a, f);
}
float Ease(float cur, float want, float rate, float dt) { return cur + (want - cur) * (1 - std::exp(-rate * dt)); }
Vec Ease(Vec cur, Vec want, float rate, float dt) {
    return {Ease(cur.x, want.x, rate, dt), Ease(cur.y, want.y, rate, dt), Ease(cur.z, want.z, rate, dt)};
}

// a row's value stepped by dir (A: on, wrapping round)
void Step(int item, int dir) {
    auto wrap = [](int v, int n) { return (v % n + n) % n; };
    int d = dir ? dir : 1;
    switch (item) {
        case kSpeed: {
            constexpr int n = int(std::size(kSpeeds));
            g_speed = dir ? std::clamp(g_speed + dir, 0, n - 1) : wrap(g_speed + 1, n);
            break;
        }
        case kCameraItem:
            g_cam_mode = wrap(g_cam_mode + d, kCameraModes);
            g_snap = true;
            g_tv_valid = false;
            break;
        case kTarget:
            g_target = wrap(g_target + d, std::max(int(g_targets.size()), 1));
            g_snap = true;
            g_tv_valid = false;
            break;
        case kLength:
            g_whole = !g_whole;
            SetSpan();
            g_t = g_begin;
            g_snap = true;
            g_tv_valid = false;
            break;
        default: break;
    }
}

// a row done: dir 0 its A or a touch, -1 or 1 its arrows. True to leave
bool Act(int item, int dir) {
    switch (item) {
        case kPlay:
            if (g_t >= g_end) g_t = g_begin, g_snap = true;  // at the end, it plays again
            g_playing = !g_playing;
            break;
        case kRestart:
            g_t = g_begin;
            g_playing = true;
            g_snap = true;
            g_tv_valid = false;
            break;
        case kSpeed:
        case kCameraItem:
        case kTarget:
        case kLength: Step(item, dir); break;
        case kHideMenu: g_menu = false; break;
        case kExit: return true;
        default: break;
    }
    return false;
}

bool Pressed(int analog) { return !g_first_pad && g_pad.analog[analog] > 0x40 && g_prev_analog[analog] <= 0x40; }

// the camera, by its mode, about the followed car
void Camera(X86Context& ctx, float dt) {
    const Target& tg = g_targets[size_t(std::clamp(int(g_target), 0, int(g_targets.size()) - 1))];
    Pose p;
    if (!PoseAt(g_t, tg.conn, tg.info, p)) {
        // the car followed is not recorded here: the player's, else the camera stays
        if (!PoseAt(g_t, g_targets.front().conn, g_targets.front().info, p)) return;
    }
    Vec car = Vec{p.body.m[12], p.body.m[13], p.body.m[14]};
    Vec fwd = Forward(p.body);
    Vec flat = Normal({fwd.x, fwd.y, 0}, Normal(fwd));
    Vec up{0, 0, 1};
    Vec pos, look;
    float fov = 70;
    if (g_cam_mode != kFree) g_drag_x = 0, g_drag_y = 0;  // a drag turns the free camera alone
    switch (g_cam_mode) {
        case kChase: {
            pos = car - flat * 6.2f + up * 2.0f;
            look = car + flat * 3.0f + up * 0.7f;
            if (!g_snap) pos = Ease(g_cam_pos, pos, 7.0f, dt);
            look = g_snap ? look : Ease(g_cam_look, look, 12.0f, dt);
            fov = 68;
            break;
        }
        case kTv: {
            // a camera beside the road kTvAhead along the car's way, kept till the car has passed it and gone on,
            // then the next. Picked 2.5 s ahead, a fast car's spot was further than the switch's own distance and the
            // camera jumped every frame
            Vec to = car - g_tv_pos;
            bool past = Dot(to, flat) > 0 && Length(to) > 25.0f;
            bool lost = Length(to) > 120.0f;  // a seek or a wind back
            if (!g_tv_valid || lost || (past && g_t - g_tv_since > 1.5) || g_t < g_tv_since) {
                Pose ahead = p;
                for (double t = g_t; t <= std::min(g_t + 8.0, g_end); t += 0.1) {
                    Pose q;
                    if (!PoseAt(t, tg.conn, tg.info, q)) break;
                    ahead = q;
                    if (Length(Vec{q.body.m[12], q.body.m[13], q.body.m[14]} - car) >= kTvAhead) break;
                }
                Vec at{ahead.body.m[12], ahead.body.m[13], ahead.body.m[14]};
                Vec af = Forward(ahead.body);
                Vec side = Normal(Cross(Normal({af.x, af.y, 0}, flat), up), {0, 1, 0});
                g_tv_side = -g_tv_side;
                float high = 2.0f + float(std::fmod(g_t * 3.1, 1.5));
                g_tv_pos = at + side * (5.5f * g_tv_side) + up * high;
                g_tv_since = g_t;
                g_tv_valid = true;
                g_snap = true;
            }
            pos = g_tv_pos;
            look = car + up * 0.6f;
            float dist = std::max(Length(look - pos), 1.0f);
            // the car about the same size as it nears and goes
            fov = std::clamp(2 * std::atan(12.0f / dist) * 180 / kPi, 30.0f, 70.0f);
            if (!g_snap) {
                look = Ease(g_cam_look, look, 10.0f, dt);
                fov = Ease(g_cam_fov, fov, 6.0f, dt);
            }
            break;
        }
        default: {  // free: turned about the car by the right stick or a drag, nearer and further by the left stick
            const input::Pad& pd = g_pad;
            float dx = g_drag_x.exchange(0), dy = g_drag_y.exchange(0);
            float h = std::max(g_screen_h.load(), 1.0f);
            g_orbit_yaw += -Stick(pd.rx) * 2.2f * dt - dx / h * 3.0f;
            g_orbit_pitch = std::clamp(g_orbit_pitch + Stick(pd.ry) * 1.5f * dt + dy / h * 2.0f, -0.15f, 1.4f);
            g_orbit_dist = std::clamp(g_orbit_dist * std::exp(-Stick(pd.ly) * 1.2f * dt), 2.5f, 25.0f);
            float heading = std::atan2(flat.y, flat.x) + kPi + g_orbit_yaw;
            float cp = std::cos(g_orbit_pitch);
            Vec target = car + up * 0.7f;
            pos = target + Vec{cp * std::cos(heading), cp * std::sin(heading), std::sin(g_orbit_pitch)} * g_orbit_dist;
            look = target;
            fov = 65;
            break;
        }
    }
    g_snap = false;
    g_cam_pos = pos;
    g_cam_look = look;
    g_cam_fov = fov;
    WriteCamera(ctx, pos, look - pos, fov, dt);
}

void Tick(X86Context& ctx) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
    g_last = now;
    std::lock_guard lock(g_lock);
    if (!Racing() || PlayerCamera() != g_camera) {
        g_camera = 0;
        Leave(Racing() ? "the camera changed" : "the race ended");
        return;
    }
    const input::Pad& p = g_pad;
    if (g_first_pad) {
        g_prev_buttons = p.buttons;
        std::memcpy(g_prev_analog, p.analog, 8);
        bool held = p.buttons || std::any_of(std::begin(p.analog), std::end(p.analog), [](uint8_t a) { return a > 0x40; });
        if (!held) g_first_pad = false;
    }
    const bool settled = !g_first_pad;
    auto edge = [&](uint16_t bit) { return settled && (p.buttons & bit) && !(g_prev_buttons & bit); };
    bool leave = false;
    if (edge(0x10)) g_menu = !g_menu.load();  // Start
    if (Pressed(2)) Act(kPlay, 0);            // X
    if (Pressed(3)) Step(kTarget, 1);         // Y
    if (Pressed(4)) Step(kCameraItem, -1);    // the shoulders: the camera
    if (Pressed(5)) Step(kCameraItem, 1);
    int tap = g_tap.exchange(-1);
    if (g_menu) {
        int sel = g_sel;
        if (edge(0x01)) sel = (sel + kItems - 1) % kItems;
        if (edge(0x02) || edge(0x20)) sel = (sel + 1) % kItems;
        g_sel = sel;
        // left and right step a row's value
        bool arrows = sel == kSpeed || sel == kCameraItem || sel == kTarget || sel == kLength;
        if (arrows && edge(0x04)) Act(sel, -1);
        if (arrows && edge(0x08)) Act(sel, 1);
        if (Pressed(0)) leave |= Act(sel, 0);
        if (tap >= 0) {
            g_sel = tap & 0xFF;
            leave |= Act(tap & 0xFF, tap & kTapLeft ? -1 : tap & kTapRight ? 1 : 0);
        }
        if (Pressed(1)) g_menu = false;
    } else {
        if (tap >= 0) g_menu = true;  // a touch on the picture brings the menu back
        leave |= Pressed(1);
    }
    g_prev_buttons = p.buttons;
    std::memcpy(g_prev_analog, p.analog, 8);
    if (leave) {
        Leave(nullptr);
        return;
    }

    // the time: played at its speed, LT and RT wind back and on
    float seek = g_seek.exchange(-1);
    if (seek >= 0) {
        g_t = g_begin + double(seek) * (g_end - g_begin);
        g_snap = true;
        g_tv_valid = false;
    }
    float wind = Trigger(p.analog[7]) - Trigger(p.analog[6]);
    if (std::fabs(wind) > 0.05f) {
        g_t += double(wind * 4.0f * dt);
        g_tv_valid = g_tv_valid && g_cam_mode != kTv;
    } else if (g_playing) {
        g_t += double(dt * kSpeeds[std::clamp(int(g_speed), 0, int(std::size(kSpeeds)) - 1)]);
    }
    if (g_t >= g_end) g_t = g_end, g_playing = false;
    if (g_t < g_begin) g_t = g_begin;
    g_shown_t = float(g_t - g_begin);
    g_shown_len = float(g_end - g_begin);

    ApplyCars();
    Camera(ctx, dt);
}

}  // namespace

void ReplayRequest(bool from_pause_menu) {
    g_request_menu = from_pause_menu;
    g_request = true;
}
bool ReplayActive() { return g_active; }

// game thread, the frame's start (cinematic.cpp's 0x0D8720): recording, or the replay stepped
void ReplayFrame(X86Context& ctx) {
    if (g_reopen_menu) {
        g_reopen_menu = false;
        if (uint32_t feng = kernel::R32(kFEng)) CallThis(ctx, feng, kSwitchPackage, {kPauseMainFng, 0, 0, 0});
    }
    bool racing = Racing();
    if (racing != g_was_racing) {
        std::lock_guard lock(g_lock);
        Clear();  // a new race, or none
        g_was_racing = racing;
    }
    if (g_request.exchange(false) && !g_active && !CinematicActive()) {
        Enter();
        if (!g_active && g_request_menu) g_reopen_menu = true;  // nothing to play: the menu back next frame
    }
    if (g_active) {
        Tick(ctx);
        g_last_tick = std::chrono::steady_clock::now();
        return;
    }
    auto now = std::chrono::steady_clock::now();
    double dt = std::clamp(std::chrono::duration<double>(now - g_last_tick).count(), 0.0, 0.1);
    g_last_tick = now;
    if (!racing || kernel::R32(kPauseDepth) > 0) return;  // the race's own time only
    std::lock_guard lock(g_lock);
    g_clock += dt;
    if (g_clock >= g_next_sample) {
        Sample();
        g_next_sample = std::max(g_next_sample + 1 / kSampleRate, g_clock);
    }
}

// a car's update done (0x25CC70): taken as a car of the race, and while the replay plays drawn as the recording has it
void ReplayConnUpdated(uint32_t conn) {
    if (!Committed(conn, kConnBody + 0x40)) return;
    std::lock_guard lock(g_lock);
    uint32_t info = kernel::R32(conn + kConnRenderInfo);
    if (!info) return;
    if (!g_active) {
        g_conns[conn] = {info, g_clock};
        return;
    }
    Pose p;
    if (PoseAt(g_t, conn, info, p)) WritePose(conn, p);
    else HideConn(conn);
}

// the pad filter: true while the replay has the pad (or it is still held after leaving)
bool ReplayPad(int port, const input::Pad& pad) {
    if (port != 0) return g_active || g_drain;
    {
        std::lock_guard lock(g_lock);
        g_pad = pad;
    }
    if (g_active) return true;
    if (g_drain) {
        constexpr int kRest = 8000;
        auto moved = [](int16_t v) { return v > kRest || v < -kRest; };
        bool held = pad.buttons || moved(pad.lx) || moved(pad.ly) || moved(pad.rx) || moved(pad.ry) ||
                    std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t a) { return a > 0x30; });
        if (!held || Now() > g_drain_until) g_drain = false;
        return g_drain;
    }
    return false;
}

// the menu at the left in the pause menu's colours, the timeline along the foot; hidden, the picture alone, and a
// touch or click on it brings the menu back (a drag turns the free camera)
void DrawReplayHud() {
    if (!g_active || app::MenuOpen()) {
        g_menu_rect[2] = 0;
        return;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    g_screen_h = screen.y;
    const float u = screen.y / 480.0f;
    auto col = [](int r, int g, int b, int a) { return IM_COL32(r, g, b, a); };
    const ImU32 amber = col(236, 156, 40, 255), bar = col(196, 124, 28, 165), panel = col(22, 16, 8, 190),
                stripe = col(255, 200, 120, 14), cream = col(255, 238, 208, 255), bracket = col(240, 228, 205, 230),
                pick = col(196, 124, 28, 150), red = col(225, 40, 30, 255);
    auto width = [font](float px, const std::string& s) { return font->CalcTextSizeA(px, FLT_MAX, 0, s.c_str()).x; };
    const ImVec2 m = io.MousePos;
    const float aspect = Aspect(), view_w = std::min(screen.x, screen.y * aspect), view_h = view_w / aspect;
    const float view_x = (screen.x - view_w) / 2, view_y = (screen.y - view_h) / 2;
    const bool menu = g_menu;

    // the timeline along the foot, shown with the menu: a touch or a drag on it seeks
    const float len = g_shown_len, at = g_shown_t;
    const float tl_x0 = view_x + 40 * u, tl_x1 = view_x + view_w - 40 * u, tl_y = view_y + view_h - 26 * u;
    bool on_timeline = false;
    if (menu) {
        const float th = 5 * u;
        dl->AddRectFilled(ImVec2(tl_x0 - 10 * u, tl_y - 18 * u), ImVec2(tl_x1 + 10 * u, tl_y + 14 * u), col(10, 8, 4, 150), 4 * u);
        dl->AddRectFilled(ImVec2(tl_x0, tl_y - th / 2), ImVec2(tl_x1, tl_y + th / 2), col(90, 70, 40, 200), th / 2);
        float fx = tl_x0 + (tl_x1 - tl_x0) * (len > 0 ? std::clamp(at / len, 0.0f, 1.0f) : 0.0f);
        dl->AddRectFilled(ImVec2(tl_x0, tl_y - th / 2), ImVec2(fx, tl_y + th / 2), amber, th / 2);
        dl->AddCircleFilled(ImVec2(fx, tl_y), 7 * u, cream, 24);
        char text[48];
        std::snprintf(text, sizeof text, "%d:%02d / %d:%02d", int(at) / 60, int(at) % 60, int(len) / 60, int(len) % 60);
        dl->AddText(font, 11 * u, ImVec2(tl_x0, tl_y - 16 * u), cream, text);
        on_timeline = m.x >= tl_x0 - 10 * u && m.x <= tl_x1 + 10 * u && m.y >= tl_y - 18 * u && m.y <= tl_y + 14 * u;
        if (on_timeline && ImGui::IsMouseDown(0) && (ImGui::IsMouseClicked(0) || io.MouseDelta.x != 0))
            g_seek = std::clamp((m.x - tl_x0) / (tl_x1 - tl_x0), 0.0f, 1.0f);
    }

    // the blinking REPLAY at the top right, always
    {
        const std::string label = app::Ui("REPLAY");
        float px = 15 * u, w = width(px, label);
        ImVec2 p(view_x + view_w - w - 30 * u, view_y + 18 * u);
        if (std::fmod(ImGui::GetTime(), 1.2) < 0.8) dl->AddCircleFilled(ImVec2(p.x - 10 * u, p.y + px / 2), 5 * u, red, 16);
        dl->AddText(font, px, ImVec2(p.x + u, p.y + u), col(0, 0, 0, 160), label.c_str());
        dl->AddText(font, px, p, cream, label.c_str());
    }

    bool on_menu = false;
    if (menu) {
        const int sel = g_sel, tgt = g_target, tgts = g_target_count;
        char speed[16], target[32];
        std::snprintf(speed, sizeof speed, "%gx", double(kSpeeds[std::clamp(int(g_speed), 0, int(std::size(kSpeeds)) - 1)]));
        if (tgt == 0) std::snprintf(target, sizeof target, "%s", app::Ui("YOU").c_str());
        else std::snprintf(target, sizeof target, "%d / %d", tgt + 1, tgts);
        struct Row {
            const char* label;
            std::string value;
            bool arrows;
        };
        const Row rows[kItems] = {
            {g_playing ? "PAUSE" : "PLAY", "", false},
            {"RESTART", "", false},
            {"SPEED", speed, true},
            {"CAMERA", app::Ui(kCameraNames[std::clamp(int(g_cam_mode), 0, kCameraModes - 1)]), true},
            {"CAR", target, true},
            {"LENGTH", app::Ui(g_whole ? "WHOLE RACE" : "LAST MINUTE"), true},
            {"HIDE MENU", "", false},
            {"EXIT", "", false},
        };
        const float label_px = 13 * u, pad = 12 * u, title_h = 26 * u, row_h = 21 * u, arrow = 9 * u;
        float w = 200 * u;
        for (const Row& r : rows)
            w = std::max(w, width(label_px, app::Ui(r.label)) + width(label_px, r.value) + 2 * arrow + pad * 4);
        const std::string title = app::Ui("REPLAY");
        const float h = title_h + row_h * float(kItems) + 8 * u;
        const ImVec2 p0(view_x + 34 * u, view_y + std::max(16 * u, (view_h - h - 60 * u) / 2)), p1(p0.x + w, p0.y + h);
        const float split = p0.y + title_h;
        g_menu_rect[0] = p0.x - 6 * u, g_menu_rect[1] = p0.y - 6 * u, g_menu_rect[3] = p1.y + 6 * u;
        g_menu_rect[2] = p1.x + 6 * u;
        on_menu = m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y;

        dl->AddRectFilled(p0, ImVec2(p1.x, split), bar);
        dl->AddLine(ImVec2(p0.x, split), ImVec2(p1.x, split), amber, 1.2f * u);
        {
            // the icon: a play mark in a ring, as a replay's
            float s = 18 * u, x = p0.x + pad, y = p0.y + (title_h - s) / 2;
            ImVec2 c(x + s / 2, y + s / 2);
            dl->AddCircleFilled(c, s / 2, amber, 24);
            ImU32 ink = col(40, 26, 8, 255);
            dl->AddTriangleFilled(ImVec2(c.x - s * 0.16f, c.y - s * 0.24f), ImVec2(c.x - s * 0.16f, c.y + s * 0.24f),
                                  ImVec2(c.x + s * 0.26f, c.y), ink);
            float px = 16 * u, tx = x + s + 8 * u, ty = p0.y + (title_h - px) / 2;
            dl->AddText(font, px, ImVec2(tx + u, ty + u), col(0, 0, 0, 160), title.c_str());
            dl->AddText(font, px, ImVec2(tx, ty), cream, title.c_str());
        }
        dl->AddRectFilled(ImVec2(p0.x, split), p1, panel);
        dl->PushClipRect(ImVec2(p0.x, split), p1, true);
        for (float x = p0.x - h; x < p1.x; x += 7 * u)
            dl->AddLine(ImVec2(x, p1.y), ImVec2(x + (p1.y - split), split), stripe, 2.5f * u);
        dl->PopClipRect();
        const bool mouse = io.MouseSource == ImGuiMouseSource_Mouse;
        for (int i = 0; i < kItems; i++) {
            const Row& r = rows[i];
            ImVec2 a(p0.x, split + 4 * u + row_h * float(i)), b(p1.x, a.y + row_h);
            bool over = m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
            if (over && mouse && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) g_sel = i;
            if (over && ImGui::IsMouseClicked(0)) {
                int tap = i;
                if (r.arrows && m.x < b.x - pad - arrow - width(label_px, r.value) - arrow / 2) tap |= kTapLeft;
                else if (r.arrows && m.x > b.x - pad - arrow) tap |= kTapRight;
                g_tap = tap;
            }
            const bool picked = i == sel;
            const float cy = (a.y + b.y) / 2;
            if (picked) {
                dl->AddRectFilled(ImVec2(a.x + 3 * u, a.y + u), ImVec2(b.x - 3 * u, b.y - u), pick, 2 * u);
                dl->AddTriangleFilled(ImVec2(a.x + 6 * u, cy - 4 * u), ImVec2(a.x + 6 * u, cy + 4 * u),
                                      ImVec2(a.x + 11 * u, cy), cream);
            }
            const std::string label = app::Ui(r.label);
            dl->AddText(font, label_px, ImVec2(a.x + pad + 4 * u, cy - label_px / 2), cream, label.c_str());
            if (!r.value.empty()) {
                float vx = b.x - pad - width(label_px, r.value) - (r.arrows ? arrow : 0);
                if (r.arrows) {
                    float lx = vx - arrow, rx = b.x - pad - arrow * 0.6f;
                    dl->AddTriangleFilled(ImVec2(lx + arrow * 0.7f, cy - 4 * u), ImVec2(lx + arrow * 0.7f, cy + 4 * u),
                                          ImVec2(lx, cy), amber);
                    dl->AddTriangleFilled(ImVec2(rx, cy - 4 * u), ImVec2(rx, cy + 4 * u), ImVec2(rx + arrow * 0.7f, cy),
                                          amber);
                }
                dl->AddText(font, label_px, ImVec2(vx, cy - label_px / 2), picked ? cream : amber, r.value.c_str());
            }
        }
        const float o = 4 * u, l = 11 * u, bt = 1.6f * u;
        for (int i = 0; i < 4; i++) {
            float sx = i & 1 ? 1.0f : -1.0f, sy = i & 2 ? 1.0f : -1.0f;
            ImVec2 c(i & 1 ? p1.x + o : p0.x - o, i & 2 ? p1.y + o : p0.y - o);
            dl->AddLine(c, ImVec2(c.x - sx * l, c.y), bracket, bt);
            dl->AddLine(c, ImVec2(c.x, c.y - sy * l), bracket, bt);
        }
        // the pad's own keys, under the menu
        struct Hint {
            const char *keys, *action;
        };
        const Hint hints[] = {{"X", "PLAY / PAUSE"}, {"LT RT", "WIND"}, {"LB RB", "CAMERA"}, {"Y", "CAR"}, {"B", "CLOSE"}};
        const float hy = p1.y + 10 * u, hx = p0.x, hpx = 10 * u, key_w = 44 * u;
        float hw = 0;
        for (const Hint& t : hints) hw = std::max(hw, key_w + width(hpx, app::Ui(t.action)));
        dl->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + std::max(w, hw + pad * 2), hy + 15 * u * float(std::size(hints)) + 6 * u),
                          col(10, 8, 4, 150), 3 * u);
        for (size_t i = 0; i < std::size(hints); i++) {
            float y = hy + 3 * u + 15 * u * float(i);
            dl->AddText(font, hpx, ImVec2(hx + pad, y), amber, hints[i].keys);
            dl->AddText(font, hpx, ImVec2(hx + pad + key_w, y), cream, app::Ui(hints[i].action).c_str());
        }
    } else {
        g_menu_rect[2] = 0;
    }

    // the picture itself: a drag turns the free camera, a touch or click with the menu hidden brings it back
    // only a touch that began with the menu hidden brings it back: HIDE MENU's own touch was let go after it hid
    static bool began_hidden = false;
    if (ImGui::IsMouseClicked(0)) began_hidden = !menu;
    if (!on_menu && !on_timeline) {
        if (ImGui::IsMouseDragging(0, 4 * u)) {
            g_drag_x = g_drag_x + io.MouseDelta.x;
            g_drag_y = g_drag_y + io.MouseDelta.y;
        } else if (!menu && began_hidden && ImGui::IsMouseReleased(0) &&
                   io.MouseDragMaxDistanceSqr[0] < 16 * u * u) {
            g_tap = 0;  // hidden, Tick takes any tap as the menu back
            began_hidden = false;
        }
    }
}

bool ReplayMenuHit(float x, float y) {
    return g_menu_rect[2] > 0 && x >= g_menu_rect[0] && x < g_menu_rect[2] && y >= g_menu_rect[1] && y < g_menu_rect[3];
}

}  // namespace game

// a car's update (thiscall: the sim's packet, dt; ret 8): while the replay plays, the recording's place after it
X86_EXTERN_FUNC(__imp__sub_0025CC70);
X86_FUNC(sub_0025CC70) {
    uint32_t conn = ctx.ecx;
    __imp__sub_0025CC70(ctx, base);
    game::ReplayConnUpdated(conn);
}
