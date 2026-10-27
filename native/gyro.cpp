// Steering by tilting the phone, as mobile racing games have it: the phone held in landscape is the wheel, turned
// clockwise to steer right. The accelerometer gives the world's up in the phone's axes; its angle in the screen's
// plane against the screen's own up is the wheel's turn, whatever way the phone leans back. On while the touch
// controls' STEERING is TILT (touch.cpp) and the app is in front; the angle for a full lock is set in the settings
// panel and kept in android.toml beside the game's settings.
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

constexpr float kDeadZone = 2.0f;    // degrees of a hand held level
constexpr float kSmoothing = 0.35f;  // of the way to a new reading taken at once: the sensor's jitter evened out
constexpr uint32_t kMinAngle = 10, kMaxAngle = 60, kAngleStep = 5;

std::atomic<uint32_t> g_angle{30};  // degrees of tilt for a full lock
bool g_loaded = false;
SDL_Sensor* g_sensor = nullptr;
bool g_no_sensor = false;  // the phone has no accelerometer
std::atomic<float> g_steer{0};

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

SDL_SensorID FindAccelerometer() {
    int n = 0;
    SDL_SensorID found = 0;
    if (SDL_SensorID* ids = SDL_GetSensors(&n)) {
        for (int i = 0; i < n && !found; i++)
            if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_ACCEL) found = ids[i];
        SDL_free(ids);
    }
    return found;
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
        g_sensor = nullptr;
        g_steer = 0;
        return;
    }
    if (!SDL_WasInit(SDL_INIT_SENSOR) && !SDL_InitSubSystem(SDL_INIT_SENSOR)) return;
    SDL_SensorID id = FindAccelerometer();
    g_sensor = id ? SDL_OpenSensor(id) : nullptr;
    if (!g_sensor) {
        LOGW("gyro: no accelerometer on this device");
        g_no_sensor = true;
    }
}

// The accelerometer reads in the phone's natural (portrait) axes: x right, y up, z out of the screen, the world's up
// at rest. In landscape with the right side up the screen's up is the phone's x and its right the phone's -y; turned
// clockwise by a, the world's up lies a anticlockwise of the screen's: x = cos a, y = sin a. With the left side up
// both axes are the other way round
void OnEvent(const SDL_Event& e) {
    if (e.type != SDL_EVENT_SENSOR_UPDATE || !g_sensor || e.sensor.which != SDL_GetSensorID(g_sensor)) return;
    float ax = e.sensor.data[0], ay = e.sensor.data[1];
    if (std::hypot(ax, ay) < 2.0f) return;  // lying flat: no up in the screen's plane to steer by
    SDL_Window* w = host::Window();
    bool flipped = w && SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(w)) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    float turn = (flipped ? std::atan2(-ay, -ax) : std::atan2(ay, ax)) * (180.0f / 3.14159265f);
    float full = float(g_angle.load());
    float target = std::fabs(turn) <= kDeadZone ? 0.0f
                                                : std::clamp((turn - std::copysign(kDeadZone, turn)) / (full - kDeadZone), -1.0f, 1.0f);
    g_steer = g_steer + (target - g_steer) * kSmoothing;
}

}  // namespace gyro
