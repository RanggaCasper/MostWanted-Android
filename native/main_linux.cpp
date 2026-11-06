// The Linux host: SDL's window and event loop, the settings and folders, crash logs; the game is described by
// app::kGame (src/common/game_info.cpp). The rest of native/ is shared with Android: the Vulkan backend, controllers,
// keyboard and mouse (touch.cpp, which also takes a touch screen), audio and ImGui. Everything the game writes
// (settings, saves in hdd, logs, the shader cache) goes beside the executable, as on Black's Linux build.
#include <SDL3/SDL.h>
#include <signal.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/options.h"
#include "common/translations.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

SDL_Window* g_window = nullptr;
std::atomic<bool> g_foreground{true};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;
// kept by the main thread for the render and game threads: the window in pixels, the desktop's mode
std::atomic<int> g_pixel_w{0}, g_pixel_h{0};
std::atomic<uint32_t> g_desktop_w{0}, g_desktop_h{0};
std::atomic<float> g_refresh{60};
bool g_mode_changed = false;  // a fullscreen mode other than the desktop's is set
uint64_t g_mouse_ms = 0;      // the mouse last moved: the cursor hides a while after, out of the panel
bool g_cursor_shown = true;

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

// UTF-8 from a wide string: wchar_t is UTF-32 here
std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        auto u = uint32_t(*w);
        if (u < 0x80) {
            s += char(u);
        } else if (u < 0x800) {
            s += char(0xC0 | (u >> 6));
            s += char(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            s += char(0xE0 | (u >> 12));
            s += char(0x80 | ((u >> 6) & 0x3F));
            s += char(0x80 | (u & 0x3F));
        } else {
            s += char(0xF0 | (u >> 18));
            s += char(0x80 | ((u >> 12) & 0x3F));
            s += char(0x80 | ((u >> 6) & 0x3F));
            s += char(0x80 | (u & 0x3F));
        }
    }
    return s;
}

// the crash's guest thread, its registers and return addresses to the log; then the default action, a core dump
void OnCrash(int sig, siginfo_t* info, void*) {
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("signal %d (%s) at %p on guest thread %X", sig, strsignal(sig), info ? info->si_addr : nullptr, t->id);
        if (info && (sig == SIGSEGV || sig == SIGBUS)) {
            uint64_t va = uint64_t(reinterpret_cast<uintptr_t>(info->si_addr));
            uint64_t g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  access %llX, guest %08llX", (unsigned long long)va, (unsigned long long)(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 256);
    } else {
        LOGE("signal %d (%s) at %p", sig, strsignal(sig), info ? info->si_addr : nullptr);
    }
    logx::Flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

void SetForeground(bool on) {
    if (g_foreground == on) return;
    {
        std::lock_guard lock(g_foreground_lock);
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "shown" : "minimized");
}

SDL_DisplayID WindowDisplay() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
    return d ? d : SDL_GetPrimaryDisplay();
}

// main thread: what other threads read of the window and its display
void UpdateSizes() {
    if (g_window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        g_pixel_w = w;
        g_pixel_h = h;
    }
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(WindowDisplay())) {
        float scale = m->pixel_density > 0 ? m->pixel_density : 1.0f;
        g_desktop_w = uint32_t(std::lround(m->w * scale));
        g_desktop_h = uint32_t(std::lround(m->h * scale));
    }
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(WindowDisplay()))
        if (m->refresh_rate > 1) g_refresh = m->refresh_rate;
}

// main thread: the window as the settings' display mode, width and height have it
void ApplyNow() {
    const app::Settings& s = app::Get();
    SDL_DisplayID display = WindowDisplay();
    if (s.display_mode == app::DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, true);
        SDL_SetWindowResizable(g_window, true);
        if (g_mode_changed) SDL_SetWindowFullscreenMode(g_window, nullptr);
        g_mode_changed = false;
        float density = SDL_GetWindowPixelDensity(g_window);
        if (density <= 0) density = 1;
        uint32_t w = s.width ? s.width : 1280, h = s.height ? s.height : 720;
        SDL_SetWindowSize(g_window, int(std::lround(w / density)), int(std::lround(h / density)));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    } else {
        // borderless is the desktop's own mode on that display; fullscreen takes the closest to width x height
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
        SDL_DisplayMode mode{};
        bool found = false;
        if (s.display_mode == app::DisplayMode::Fullscreen && s.width && s.height) {
            float scale = desktop && desktop->pixel_density > 0 ? desktop->pixel_density : 1.0f;
            found = SDL_GetClosestFullscreenDisplayMode(display, int(s.width / scale), int(s.height / scale), 0.0f,
                                                        true, &mode);
        }
        g_mode_changed = found && desktop && (mode.w != desktop->w || mode.h != desktop->h);
        // null: fullscreen at the desktop's mode, which Wayland compositors take without a mode change
        SDL_SetWindowFullscreenMode(g_window, found ? &mode : nullptr);
        SDL_SetWindowFullscreen(g_window, true);
    }
    SDL_SyncWindow(g_window);
    UpdateSizes();
}

// F11 and Alt+Enter: windowed, or back to the full screen the settings had (borderless if they had none)
void ToggleFullscreen() {
    app::Settings& s = app::Get();
    static app::DisplayMode last_full = app::DisplayMode::Borderless;
    if (s.display_mode == app::DisplayMode::Windowed) {
        s.display_mode = last_full;
    } else {
        last_full = s.display_mode;
        s.display_mode = app::DisplayMode::Windowed;
    }
    ApplyNow();
    app::Save();
}

void UpdateCursor() {
    // shown while it moves and while the settings panel is open; a game window has no use for an idle arrow
    constexpr uint64_t kHideMs = 2500;
    bool show = app::MenuOpen() || NowMs() - g_mouse_ms < kHideMs;
    if (show == g_cursor_shown) return;
    g_cursor_shown = show;
    if (show) SDL_ShowCursor();
    else SDL_HideCursor();
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() { return g_foreground; }
// the surface stays the window's for its whole life on a PC
uint32_t SurfaceEpoch() { return 0; }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load(); });
}

void WindowPixels(int& w, int& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
constexpr int kDefaults = kSettingsVersion;

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"defaults", int64_t(kDefaults)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

std::string OsName() {
    std::ifstream f("/etc/os-release");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    return "Linux";
}

std::string CpuName() {
    std::ifstream f("/proc/cpuinfo");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("model name", 0) == 0) {
            size_t c = line.find(':');
            if (c != std::string::npos) return line.substr(line.find_first_not_of(' ', c + 1));
        }
    return "unknown CPU";
}

}  // namespace

// the settings a PC starts with: the whole window's resolution, the console's 60 fps with vsync
Settings DefaultSettings() { return Settings{}; }

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, BUILD_COMMIT);
    LOGR("Path: %s", ExeDir().c_str());
    utsname u{};
    uname(&u);
    LOGR("OS: %s, kernel %s %s, SDL video %s", OsName().c_str(), u.release, u.machine,
         SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    LOGR("CPU: %s (%d threads)", CpuName().c_str(), SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, double(g_refresh.load()));
    LogSettings();
}

void ApplyDisplay() {
    SDL_RunOnMainThread([](void*) { ApplyNow(); }, nullptr, false);
}

Mode Desktop() {
    uint32_t w = g_desktop_w, h = g_desktop_h;
    return w && h ? Mode{w, h} : Mode{1920, 1080};
}

// the window's pixels: the picture is drawn at the window's size, render resolution a share of it
Mode Output() {
    int w = g_pixel_w, h = g_pixel_h;
    return w > 0 && h > 0 ? Mode{uint32_t(w), uint32_t(h)} : Desktop();
}

bool Widescreen() {
    // the frame's shape as the aspect ratio setting has it: the game reads this at boot
    return game::Aspect() >= 1.5f;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    if (game::Opt().anti_aliasing == game::kAaSsaa) h *= 2.0f;  // the presenter averages it back down
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        return ec ? std::filesystem::current_path() : self.parent_path();
    }();
    return dir;
}

int Instance() { return 0; }

void* Window() { return g_window; }

// no splash and no relaunch into another executable
void SplashFirstFrame() {}
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(1) < next) std::this_thread::sleep_for(std::chrono::microseconds(500));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "linux_sdl3";
    touch::LoadArt();
}

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            app::Save();
            logx::Flush();
            if (g_mode_changed) SDL_SetWindowFullscreenMode(g_window, nullptr);
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
            SetForeground(false);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            SetForeground(true);
            UpdateSizes();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            UpdateSizes();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // a changed display mode steps aside for the desktop, and comes back with the focus
            if (g_mode_changed) {
                SDL_SetWindowFullscreenMode(g_window, nullptr);
                g_mode_changed = false;
                SDL_MinimizeWindow(g_window);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (app::Get().display_mode == app::DisplayMode::Fullscreen && app::Get().width && !g_mode_changed)
                ApplyNow();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat &&
                (e.key.scancode == SDL_SCANCODE_F11 ||
                 (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                ToggleFullscreen();
                return;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which != SDL_TOUCH_MOUSEID) g_mouse_ms = NowMs();
            break;
        case SDL_EVENT_SENSOR_UPDATE:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            gyro::OnEvent(e);
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            break;
        default:
            break;
    }
    if (touch::OnEvent(e)) return;
    std::lock_guard lock(app::UiLock());
    ui::OnEvent(e);
}

}  // namespace

int main(int, char**) {
    InstallCrashHandler();
    // XBE_PERF in the environment, or a file named perf beside the executable, as on Android
    if (std::error_code ec; std::filesystem::exists(app::ExeDir() / "perf", ec)) setenv("XBE_PERF", "1", 1);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    LOGR("app: log %s", logx::Path().c_str());

    std::string title = Utf8(app::kGame.title);
    SDL_SetAppMetadata(title.c_str(), nullptr, "com.racas.mostwanted");
    // a touch screen's touches are the touch controls' and FEng's, not a mouse; the mouse is not a touch
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    // the game keeps playing while another window has the focus; minimized it waits
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOGE("SDL_Init: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    // sensors only where SDL has them (a controller's gyro comes through the gamepad subsystem)
    SDL_InitSubSystem(SDL_INIT_SENSOR);
    UpdateSizes();
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) {
        logx::Flush();
        return 1;
    }

    g_window = SDL_CreateWindow(title.c_str(), 1280, 720,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIDDEN);
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 "The game window could not be opened, see the log in the logs folder.", nullptr);
        return 1;
    }
    ApplyNow();
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    SDL_SyncWindow(g_window);
    UpdateSizes();
    constexpr const char* kModeNames[] = {"fullscreen", "borderless", "windowed"};
    LOGR("app: window %dx%d pixels, %s", g_pixel_w.load(), g_pixel_h.load(),
         kModeNames[uint32_t(app::Get().display_mode)]);

    InitUi();
    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    std::filesystem::path hdd = app::ExeDir() / "hdd";
    LOGR("app: saves in %s", hdd.c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), hdd.string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 app::Ui("The game failed to start, see the log in the logs folder.", false).c_str(),
                                 g_window);
        return 1;
    }

    for (;;) {
        SDL_Event e;
        // a few milliseconds: HIDAPI controllers are read in the pump
        if (SDL_WaitEventTimeout(&e, 4)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        touch::Pump();
        UpdateCursor();
    }
}
