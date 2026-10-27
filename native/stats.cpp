// The performance overlay's numbers (src/common/menu.cpp): CPU and GPU load and the temperatures. An app may not
// read every source Android has, and which it may differs by maker and version: each number falls back to what it
// can read, and is -1 when nothing tells it
#include <jni.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"

namespace host {

float VulkanGpuLoad();  // native/vulkan.cpp: the frames' GPU time, from timestamps

namespace {

std::atomic<float> g_cpu{-1}, g_gpu{-1}, g_cpu_temp{-1}, g_gpu_temp{-1}, g_battery_temp{-1};

bool ReadFile(const char* path, char* buf, size_t n) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    size_t got = std::fread(buf, 1, n - 1, f);
    std::fclose(f);
    buf[got] = 0;
    return got > 0;
}

// ---- CPU load: the whole device's from /proc/stat, which Android 8 and later deny apps; else this process's share
// of all the cores, from its own /proc/self/stat

struct CpuSample {
    uint64_t busy = 0, total = 0;
};

bool SystemCpu(CpuSample& s) {
    char buf[256];
    if (!ReadFile("/proc/stat", buf, sizeof(buf))) return false;
    unsigned long long v[8] = {};
    if (std::sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                    &v[6], &v[7]) < 4)
        return false;
    s.total = 0;
    for (unsigned long long x : v) s.total += x;
    s.busy = s.total - v[3] - v[4];  // idle and iowait
    return true;
}

// in clock ticks; total is the wall time in ticks times the cores
bool ProcessCpu(CpuSample& s) {
    char buf[1024];
    if (!ReadFile("/proc/self/stat", buf, sizeof(buf))) return false;
    // the name in parentheses may hold spaces: the fields count from its closing one, utime and stime are 14 and 15
    const char* p = std::strrchr(buf, ')');
    if (!p) return false;
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) return false;
    static const long ticks = sysconf(_SC_CLK_TCK);
    static const long cores = std::max(1L, sysconf(_SC_NPROCESSORS_CONF));
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    s.busy = utime + stime;
    s.total = uint64_t(now * double(ticks) * double(cores));
    return true;
}

// ---- GPU load: Adreno's own counter when the app may read it, else the frames' time from Vulkan's timestamps

float AdrenoBusy() {
    char buf[32];
    if (!ReadFile("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", buf, sizeof(buf))) return -1;
    int v = 0;
    return std::sscanf(buf, "%d", &v) == 1 ? float(std::clamp(v, 0, 100)) : -1;
}

// ---- temperatures: the hottest CPU and GPU sensors of the thermal zones, which newer Android versions deny apps;
// the battery's from the activity always

struct Zones {
    std::vector<std::string> cpu, gpu;
};

Zones FindZones() {
    Zones z;
    for (int i = 0; i < 128; i++) {
        char path[64], type[64];
        std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
        if (!ReadFile(path, type, sizeof(type))) continue;
        std::string t = type;
        std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        // Qualcomm's cpu-1-0-0 and cpuss-0, gpuss-0; MediaTek's and Exynos' cpu and gpu zones by those names
        if (t.rfind("cpu", 0) == 0 || t.find("cpu_") != std::string::npos) z.cpu.push_back(path);
        else if (t.find("gpu") != std::string::npos) z.gpu.push_back(path);
    }
    return z;
}

// degrees, -1 when none reads; zones give millidegrees, a few whole degrees
float Hottest(const std::vector<std::string>& paths) {
    float hot = -1;
    char buf[32];
    for (const std::string& p : paths) {
        if (!ReadFile(p.c_str(), buf, sizeof(buf))) continue;
        long v = std::strtol(buf, nullptr, 10);
        float c = std::abs(v) >= 1000 ? float(v) / 1000.0f : float(v);
        if (c > 0 && c < 150) hot = std::max(hot, c);
    }
    return hot;
}

float BatteryTemperature() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return -1;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "batteryTemperature", "()I");
    jint tenths = m ? env->CallIntMethod(activity, m) : -1;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        tenths = -1;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return tenths > 0 ? float(tenths) / 10.0f : -1;
}

}  // namespace

// once a second at most, from the overlay's thread
void SamplePerf() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static CpuSample prev;
    static bool system_cpu = SystemCpu(prev) || !ProcessCpu(prev);
    static Zones zones = FindZones();
    // tried once: a denied read is logged by SELinux each time
    static bool kgsl = AdrenoBusy() >= 0;
    static bool logged = false;
    auto now = clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;

    CpuSample s;
    if (system_cpu ? SystemCpu(s) : ProcessCpu(s)) {
        if (s.total > prev.total)
            g_cpu = std::clamp(100.0f * float(s.busy - prev.busy) / float(s.total - prev.total), 0.0f, 100.0f);
        prev = s;
    }
    float adreno = kgsl ? AdrenoBusy() : -1;
    g_gpu = adreno >= 0 ? adreno : VulkanGpuLoad();
    g_cpu_temp = Hottest(zones.cpu);
    g_gpu_temp = Hottest(zones.gpu);
    g_battery_temp = BatteryTemperature();
    if (!logged) {
        logged = true;
        LOGR("perf: CPU load from %s, GPU load from %s, %zu CPU and %zu GPU thermal zones (%s), battery %.1f C",
             system_cpu ? "/proc/stat" : "this process", kgsl ? "kgsl" : "Vulkan timestamps", zones.cpu.size(),
             zones.gpu.size(), g_cpu_temp >= 0 ? "readable" : "unreadable", g_battery_temp.load());
    }
}

float CpuLoad() { return g_cpu; }
float GpuLoad() { return g_gpu; }
float CpuTemp() { return g_cpu_temp; }
float GpuTemp() { return g_gpu_temp; }
float BatteryTemp() { return g_battery_temp; }

}  // namespace host
