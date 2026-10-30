// Steering by tilting the phone, as mobile racing games have it: the phone held in landscape is the wheel, turned
// clockwise to steer right. The accelerometer gives the world's up in the phone's axes; its angle in the screen's
// plane against the screen's own up is the wheel's turn, whatever way the phone leans back. It also reads every bump
// of the hand, so alone it shook the steering, or smoothed lagged it; the gyroscope's turn about the screen's axis is
// clean and immediate, so the turn follows the gyroscope and the accelerometer only pulls it back towards true over
// half a second (a complementary filter), against the gyroscope's drift. A phone with no gyroscope steers by the
// accelerometer alone, smoothed over time rather than per reading, which made it twitchy on a fast sensor and slow on
// a slow one. On while the touch controls' STEERING is TILT (touch.cpp) and the app is in front; the angle for a full
// lock is set in the settings panel and kept in android.toml beside the game's settings.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

namespace gyro {

namespace {

constexpr float kDeadZone = 1.5f;  // degrees of a hand held level
// seconds the accelerometer takes to pull the turn to its reading: with the gyroscope, its drift; without, the
// sensor's jitter evened out
constexpr float kAccelPull = 0.5f, kAccelOnly = 0.07f;
constexpr float kMaxStep = 0.05f;  // seconds between readings taken at most: a gap (the app away) is no long turn
constexpr float kExpo = 0.4f;  // the share of the lock's curve that is squared: small tilts finer, a full lock as set
constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr uint32_t kMinAngle = 10, kMaxAngle = 60, kAngleStep = 5;

std::atomic<uint32_t> g_angle{30};  // degrees of tilt for a full lock
bool g_loaded = false;
SDL_Sensor* g_sensor = nullptr;  // the accelerometer
SDL_Sensor* g_gyro = nullptr;    // the gyroscope, when the phone has one
bool g_no_sensor = false;  // the phone has no accelerometer
std::atomic<float> g_steer{0};
// main thread: the wheel's turn in degrees, clockwise, and each sensor's last reading's time
float g_turn = 0;
bool g_have_turn = false;
uint64_t g_accel_ns = 0, g_gyro_ns = 0;

std::filesystem::path File() { return app::ExeDir() / "android.toml"; }

void Load() {
    g_loaded = true;
    try {
        toml::table t = toml::parse_file(File().string());
        g_angle = std::clamp<uint32_t>(t["tilt_angle"].value_or(g_angle.load()), kMinAngle, kMaxAngle);
    } catch (...) {
    }
}

void Save() {
    toml::table t{{"tilt_angle", int64_t(g_angle.load())}};
    std::ofstream(File()) << t << "\n";
}

SDL_SensorID FindSensor(SDL_SensorType type) {
    int n = 0;
    SDL_SensorID found = 0;
    if (SDL_SensorID* ids = SDL_GetSensors(&n)) {
        for (int i = 0; i < n && !found; i++)
            if (SDL_GetSensorTypeForID(ids[i]) == type) found = ids[i];
        SDL_free(ids);
    }
    return found;
}

// seconds since a sensor's last reading, by the readings' own times
float Step(uint64_t now, uint64_t& last) {
    float dt = last && now > last ? float(now - last) * 1e-9f : 0.0f;
    last = now;
    return std::min(dt, kMaxStep);
}

// the turn as the steering: past the dead zone, to a full lock at the set angle, through a gentle curve
void Publish() {
    float full = float(g_angle.load()), a = std::fabs(g_turn);
    float x = a <= kDeadZone ? 0.0f : std::min((a - kDeadZone) / (full - kDeadZone), 1.0f);
    g_steer = std::copysign(x * (1 - kExpo + kExpo * x), g_turn);
}

}  // namespace

uint32_t TiltAngle() {
    if (!g_loaded) Load();
    return g_angle;
}

// the settings panel (src/common/menu.cpp); saved at once
void StepTiltAngle(int step) {
    if (!g_loaded) Load();
    int a = int(g_angle) + step * int(kAngleStep);
    g_angle = uint32_t(std::clamp(a, int(kMinAngle), int(kMaxAngle)));
    Save();
}

float Steer() { return g_sensor ? g_steer.load() : 0.0f; }

void Pump() {
    if (!g_loaded) Load();
    bool want = touch::TiltSteering() && host::Foreground();
    if (want == (g_sensor != nullptr) || g_no_sensor) return;
    if (!want) {
        SDL_CloseSensor(g_sensor);
        if (g_gyro) SDL_CloseSensor(g_gyro);
        g_sensor = g_gyro = nullptr;
        g_steer = 0;
        return;
    }
    if (!SDL_WasInit(SDL_INIT_SENSOR) && !SDL_InitSubSystem(SDL_INIT_SENSOR)) return;
    SDL_SensorID id = FindSensor(SDL_SENSOR_ACCEL);
    g_sensor = id ? SDL_OpenSensor(id) : nullptr;
    if (!g_sensor) {
        LOGW("gyro: no accelerometer on this device");
        g_no_sensor = true;
        return;
    }
    SDL_SensorID gid = FindSensor(SDL_SENSOR_GYRO);
    g_gyro = gid ? SDL_OpenSensor(gid) : nullptr;
    static bool logged = false;
    if (!logged) LOGR("gyro: tilt by the accelerometer%s", g_gyro ? " and the gyroscope" : " alone (no gyroscope)");
    logged = true;
    // the turn starts again from the accelerometer's reading
    g_have_turn = false;
    g_accel_ns = g_gyro_ns = 0;
}

// The accelerometer reads in the phone's natural (portrait) axes: x right, y up, z out of the screen, the world's up
// at rest. In landscape with the right side up the screen's up is the phone's x and its right the phone's -y; turned
// clockwise by a, the world's up lies a anticlockwise of the screen's: x = cos a, y = sin a. With the left side up
// both axes are the other way round. The gyroscope's z is the phone's turn about the screen's axis, anticlockwise
// positive in either landscape: the wheel's clockwise turn is its negative
void OnEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_SENSOR_UPDATE || !g_sensor) return;
    if (g_gyro && e.sensor.which == SDL_GetSensorID(g_gyro)) {
        float dt = Step(e.sensor.sensor_timestamp, g_gyro_ns);
        if (!g_have_turn) return;  // the accelerometer's first reading sets where it starts
        g_turn = std::clamp(g_turn - e.sensor.data[2] * kDegrees * dt, -90.0f, 90.0f);
        Publish();
        return;
    }
    if (e.sensor.which != SDL_GetSensorID(g_sensor)) return;
    float dt = Step(e.sensor.sensor_timestamp, g_accel_ns);
    float ax = e.sensor.data[0], ay = e.sensor.data[1];
    float in_plane = std::hypot(ax, ay);
    if (in_plane < 2.0f) return;  // lying flat: no up in the screen's plane to steer by
    SDL_Window* w = host::Window();
    bool flipped = w && SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(w)) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    float turn = (flipped ? std::atan2(-ay, -ax) : std::atan2(ay, ax)) * kDegrees;
    if (!g_have_turn) {
        g_turn = turn;
        g_have_turn = true;
    } else {
        // a shake (the reading far from gravity's 9.8) is trusted less, the gyroscope carrying the turn through it
        float g = std::hypot(in_plane, e.sensor.data[2]);
        float trust = std::clamp(1.0f - std::fabs(g - SDL_STANDARD_GRAVITY) / 4.0f, 0.1f, 1.0f);
        float tau = g_gyro ? kAccelPull : kAccelOnly;
        g_turn += (turn - g_turn) * (1.0f - std::exp(-dt / tau)) * (g_gyro ? trust : 1.0f);
    }
    Publish();
}

}  // namespace gyro
