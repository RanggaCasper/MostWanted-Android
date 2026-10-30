// Android's input on port 0 beside the controllers: the touch controls, and a keyboard when one is plugged in, as an
// Xbox controller. It is src/common/mouse_keyboard.h. SDL's keys arrive as the virtual keys of common/keys.h.
//
// The touch layout is a racing game's (kRacing below): its buttons press the controller's, and the game's
// menus take a touch as FEng's mouse (src/game/fe_mouse.cpp).
#include "common/mouse_keyboard.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/aspect.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/fe_mouse.h"
#include "common/hud_layout.h"
#include "common/options.h"
#include "common/translations.h"
#include "host.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace input {

namespace {

std::atomic<uint64_t> g_capture_ms{0};  // PressedKey
std::atomic<bool> g_settle{false};
std::atomic<bool> g_keyboard{false};  // the keyboard or mouse used last
std::atomic<bool> g_touch{true};      // the touch controls used last, not a controller or the keys
constexpr uint64_t kTapMs = 80;
constexpr uint64_t kCaptureMs = 250;  // PressedKey asked this recently: a key capture is up
// The wheel binds as two keys, codes no key has. A notch holds what is bound to it for one tap.
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down
// the keys and mouse buttons held, by virtual key
std::array<std::atomic<bool>, 256> g_down{};

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys,
// 20-23 the look keys
struct Target {
    Action action;
    int button;
};
constexpr int kHostKey = -2;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kStickMax = 32767;
// the controller's buttons and sticks, each a key of its own
constexpr Target kActions[] = {
    {{"move_up", "LEFT STICK UP", "W"}, 16},
    {{"move_down", "LEFT STICK DOWN", "S"}, 17},
    {{"move_left", "LEFT STICK LEFT", "A"}, 18},
    {{"move_right", "LEFT STICK RIGHT", "D"}, 19},
    {{"button_a", "A", "Space,K"}, 0},
    {{"button_b", "B", "L"}, 1},
    {{"button_x", "X", "J"}, 2},
    {{"button_y", "Y", "I"}, 3},
    {{"button_black", "BLACK", "O"}, 4},
    {{"button_white", "WHITE", "U"}, 5},
    {{"trigger_left", "LEFT TRIGGER", "Q"}, 6},
    {{"trigger_right", "RIGHT TRIGGER", "E"}, 7},
    {{"dpad_up", "DPAD UP", "Up"}, 8},
    {{"dpad_down", "DPAD DOWN", "Down"}, 9},
    {{"dpad_left", "DPAD LEFT", "Left"}, 10},
    {{"dpad_right", "DPAD RIGHT", "Right"}, 11},
    {{"start", "START", "Enter,Escape"}, kStart},
    {{"back", "BACK", "Backspace,Tab"}, 13},
    {{"thumb_left", "LEFT THUMB", "C"}, 14},
    {{"thumb_right", "RIGHT THUMB", "V"}, 15},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey},
    {{"look_up", "RIGHT STICK UP", "Numpad8"}, 20},
    {{"look_down", "RIGHT STICK DOWN", "Numpad5"}, 21},
    {{"look_left", "RIGHT STICK LEFT", "Numpad4"}, 22},
    {{"look_right", "RIGHT STICK RIGHT", "Numpad6"}, 23},
};
constexpr size_t kCount = std::size(kActions);
constexpr size_t HostAction(std::string_view id) {
    size_t i = 0;
    while (kActions[i].action.id != id) i++;
    return i;
}
constexpr size_t kPanelAction = HostAction("settings_panel");
struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// held from before the game had the input, these wait for release as the bound keys do
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (strcasecmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

std::string Join(const std::vector<int>& vks) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += NameOf(vk);
    }
    return s;
}

bool Active() { return host::Foreground() && !app::MenuOpen(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
    return vk > 0 && vk < 256 && g_down[size_t(vk)];
}
bool Mouse(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || Wheel(vk);
}
bool KeyCaptureUp() { return NowMs() - g_capture_ms <= kCaptureMs; }

bool HostKeyBound(size_t action, int vk) {
    if (KeyCaptureUp()) return false;
    std::lock_guard lock(g_bind_lock);
    const auto& b = g_binds[action];
    return std::find(b.begin(), b.end(), vk) != b.end();
}

// keys held from before the game had the input wait for release; true once none is held
bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

struct Held {
    bool used = false;
    int stick[8] = {};  // move forward back left right, look up down left right
};

Held PressActions(const Binds& binds, Pad& p) {
    Held held;
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (b == kHostKey || !std::any_of(binds[i].begin(), binds[i].end(), Down)) continue;
        held.used = true;
        if (b >= kFirstMoveKey) held.stick[b - kFirstMoveKey] = 1;
        else if (b < kFirstButtonBit) p.analog[b] = 0xFF;
        else p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
    }
    return held;
}

// SDL's scancodes as the virtual keys (common/keys.h) the bindings name, by position as on a US keyboard
int VkOf(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        default: return 0;
    }
}

int VkOfButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
        case SDL_BUTTON_X1: return VK_XBUTTON1;
        case SDL_BUTTON_X2: return VK_XBUTTON2;
        default: return 0;
    }
}

}  // namespace

void ReleaseCursor() { g_settle = true; }

void Keyboard(Pad& p) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    Binds binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) return;
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, p);
    const int* stick = held.stick;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(x * kStickMax);
    if (int y = stick[0] - stick[1]) p.ly = int16_t(y * kStickMax);
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) g_keyboard = true;
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) g_keyboard = false;
}

bool KeyboardActive() { return g_keyboard; }

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action]) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    if (kActions[action].button == kHostKey && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        if (!slots[0] && !slots[1] && action == kPanelAction) return;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

bool PanelKey(int vk) { return HostKeyBound(kPanelAction, vk); }

}  // namespace input


// the touch controls
namespace touch {

namespace {

using input::NowMs;

// what a finger on the screen does
// FeTap: a finger in a menu prompting the right stick, FEng's pointer not given yet: a tap or the stick
enum class Role : uint8_t { None, Stick, Look, Button, Drag, Pinch, FeMouse, Wheel, HudDrag, FeTap };

// a control of the touch layout: what it shows and presses, on the controller (analog 0-7, button bits from 8) or on
// the host. Where it sits is the layout's, which the player edits
enum : int { kResetCar = -6, kSteerWheel = -5, kSteerRight = -4, kSteerLeft = -3, kPanel = -1, kAnalogA = 0, kAnalogB = 1, kAnalogX = 2, kAnalogY = 3, kAnalogLT = 6, kAnalogRT = 7 };
constexpr int kBit = 8;  // kBit + n: buttons bit n (dpad up down left right, start, back, left thumb, right thumb)
enum class Icon : uint8_t { Text, Pause, Gear, Back, Gyro, Up, Down, Left, Right, Wheel };
// how a control is drawn and touched: a circle, a shoulder's tab, a small pill, the dpad's rounded square, a racing
// game's tall pedal (the triggers). Its place's r is a circle's radius, the others' half height
enum class Shape : uint8_t { Round, Tab, Pill, Square, Pedal };
struct Button {
    const char* id;     // its key in layout.toml
    const char* label;  // in the layout editor
    Icon icon;
    int press;
    Shape shape = Shape::Round;
    const char* caption = nullptr;  // what it does in the game, under a face button
    bool keep = false;  // never hidden: the way back out of the menus
};
// the layout is set in the settings panel (TOUCH CONTROLS): its steering, opacity, size, and the editor that moves the
// controls, see the functions at the end. A Text control shows its label.
// the face buttons' captions are Most Wanted's own controls, as its loading screen shows them
constexpr Button kButtons[] = {
    {"a", "A", Icon::Text, kAnalogA, Shape::Round, "E-BRAKE"},
    {"b", "B", Icon::Text, kAnalogB, Shape::Round, "NITROUS"},
    {"x", "X", Icon::Text, kAnalogX, Shape::Round, "SPEEDBREAKER"},
    {"y", "Y", Icon::Text, kAnalogY, Shape::Round, "LOOK BACK"},
    {"black", "BLACK", Icon::Text, 4, Shape::Tab, "CAMERA"},
    {"white", "WHITE", Icon::Text, 5, Shape::Tab},
    {"lt", "LT", Icon::Text, kAnalogLT, Shape::Tab},
    {"rt", "RT", Icon::Text, kAnalogRT, Shape::Tab},
    {"start", "START", Icon::Text, kBit + 4, Shape::Pill},
    {"back", "BACK", Icon::Text, kBit + 5, Shape::Pill},
    {"panel", "Settings", Icon::Gear, kPanel, Shape::Round, nullptr, true},
    {"dpad_up", "Up", Icon::Up, kBit + 0, Shape::Square},
    {"dpad_down", "Down", Icon::Down, kBit + 1, Shape::Square},
    {"dpad_left", "Left", Icon::Left, kBit + 2, Shape::Square},
    {"dpad_right", "Right", Icon::Right, kBit + 3, Shape::Square},
    // the left stick pushed to a side, as a racing game's arrows steer; shown when STEERING is ARROWS
    {"steer_left", "Steer left", Icon::Left, kSteerLeft, Shape::Square},
    {"steer_right", "Steer right", Icon::Right, kSteerRight, Shape::Square},
    // a steering wheel turned by a finger dragged round it; shown when STEERING is WHEEL
    {"wheel", "Wheel", Icon::Wheel, kSteerWheel, Shape::Round},
    // the player's car put back on the road (src/game/game.cpp's ResetCarFrame): the console has no button for it
    {"reset_car", "RESET", Icon::Text, kResetCar, Shape::Round},
};
constexpr size_t kButtonCount = std::size(kButtons);
constexpr size_t kStick = kButtonCount;  // the sticks' places, after the buttons'
constexpr size_t kRStick = kButtonCount + 1;
constexpr size_t kPlaces = kButtonCount + 2;
bool IsStick(size_t i) { return i >= kStick; }
const char* PlaceId(size_t i) { return i == kStick ? "stick" : i == kRStick ? "rstick" : kButtons[i].id; }

// a control the layout may hide: a button but the settings gear, the way back out of the menus, and the sticks (their
// drags then move nothing; the layout steers with arrows instead)
bool Hideable(int i) {
    if (i < 0) return false;
    return IsStick(size_t(i)) || !kButtons[size_t(i)].keep;
}

// where a control sits: its centre as shares of the screen's width and height, its radius in screen heights
struct Place {
    float x, y, r;
    bool hidden = false;
};
using Layout = std::array<Place, kPlaces>;
// The layout's places, as the editor's reset puts them: a b x y, black white, lt rt, start back, the settings, the
// dpad's up down left right, the steering arrows, the wheel, RESET, the left stick and the right stick.
// As a mobile racing game: steering arrows under the left thumb, the gas and brake pedals (RT, LT) under the right,
// Nitrous over the gas, the e-brake over the brake, Speedbreaker and Look Back near them; a small dpad for the menus
// high on the left, Back, the settings and Start along the top, Black (the camera) in the top right corner, the gear
// lever (the right stick) low in the middle. No left stick, no other shoulders
constexpr Layout kRacing = {{
    {0.80f, 0.50f, 0.06f}, {0.92f, 0.38f, 0.068f}, {0.665f, 0.60f, 0.052f}, {0.80f, 0.24f, 0.045f},
    {0.932f, 0.085f, 0.048f}, {0.068f, 0.085f, 0.048f, true},
    {0.765f, 0.85f, 0.085f}, {0.915f, 0.74f, 0.17f},
    {0.585f, 0.075f, 0.036f}, {0.415f, 0.075f, 0.036f}, {0.50f, 0.075f, 0.045f},
    {0.07f, 0.28f, 0.038f}, {0.07f, 0.46f, 0.038f}, {0.03f, 0.37f, 0.038f}, {0.11f, 0.37f, 0.038f},
    {0.08f, 0.76f, 0.11f}, {0.25f, 0.76f, 0.11f}, {0.15f, 0.70f, 0.19f}, {0.33f, 0.075f, 0.036f},
    {0.17f, 0.76f, 0.12f, true}, {0.55f, 0.80f, 0.10f},
}};

constexpr float kStickZone = 0.42f;    // share of the width from the left a touch takes the stick in
constexpr float kFixedReach = 1.5f;    // FIXED sticks: their radii from the centre a touch takes them in
constexpr uint64_t kShownMs = 8000;     // the controls hide this long after the last touch while a pad is plugged in

struct Finger {
    SDL_FingerID id = 0;
    Role role = Role::None;
    size_t button = 0;     // or kStick, dragged in the editor
    float x0 = 0, y0 = 0;  // where it went down, pixels; in the editor, its offset from the control's centre
    float x = 0, y = 0;    // where it is
    uint64_t down_ms = 0;
    float angle = 0;    // Wheel: the finger's angle round the wheel's centre at its last move, degrees
};

std::mutex g_lock;
std::vector<Finger> g_fingers;
bool g_pressed[kButtonCount] = {};
float g_stick_x = 0, g_stick_y = 0;  // -1 to 1, y up
float g_rstick_x = 0, g_rstick_y = 0;  // the right stick, a drag on the right of the screen
std::atomic<uint64_t> g_touch_ms{0};
std::atomic<uint64_t> g_back_until{0};  // the back key's tap of Start or B lasts until then
constexpr uint64_t kBackTapMs = 120;

// the layout in use, under g_lock
Layout g_layout = kRacing;
float g_opacity = 0.85f;
float g_scale = 1.0f;  // every control's size, on top of its own
// STICK MODE: floating, the left stick's side of the screen the left stick and a drag on the other the right stick
// from wherever the thumb lands; fixed, each stick where it is drawn alone, so a stray touch elsewhere moves nothing
bool g_fixed_sticks = false;
// STEERING, what steers in the RACING layout: its arrows, its wheel, or the phone tilted (gyro.cpp), neither shown
enum class Steering : uint8_t { Arrows, Wheel, Tilt };
constexpr const char* kSteeringIds[] = {"arrows", "wheel", "tilt"};
constexpr const char* kSteeringNames[] = {"ARROWS", "WHEEL", "TILT"};
std::atomic<Steering> g_steering{Steering::Arrows};
// the wheel's turn, degrees clockwise, and whether a finger holds it; let go it comes back to the middle
constexpr float kWheelLock = 90.0f;  // degrees for a full lock
float g_wheel_turn = 0;
bool g_wheel_held = false;
bool g_loaded = false;
// the editor: the layout before it opened (for Cancel), the control picked
std::atomic<bool> g_editing{false};
Layout g_before = kRacing;
float g_before_opacity = 0.85f;
int g_selected = -1;

// The HUD's speedometer, minimap, race information and EA Trax banner, moved in the editor too (src/game/hud_layout.cpp moves them): their offsets from
// where the game puts them, FEng units (480 to the frame's height), in layout.toml's [hud]
constexpr const char* kHudIds[game::kHudItems] = {"speedometer", "minimap", "race_info", "ea_trax"};
constexpr const char* kHudLabels[game::kHudItems] = {"SPEEDOMETER", "MINIMAP", "RACE INFO", "EA TRAX"};
struct HudPlace {
    float x = 0, y = 0;
};
using HudLayout = std::array<HudPlace, game::kHudItems>;
HudLayout g_hud{}, g_hud_before{};
int g_hud_selected = -1;
HudPlace g_hud_drag_from;  // the dragged item's place as the drag began

void PublishHud() {
    for (size_t i = 0; i < game::kHudItems; i++) {
        game::g_hud_x[i] = g_hud[i].x;
        game::g_hud_y[i] = g_hud[i].y;
    }
}

float Width() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(w, 1));
}
float Height() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(h, 1));
}

std::filesystem::path LayoutFile() { return app::ExeDir() / "layout.toml"; }
// the layout's places changed: a layout saved before keeps its opacity and size but not its places
constexpr int64_t kLayoutVersion = 5;
constexpr int64_t kOldestLayout = 4;  // an older saved layout's places are kRacing's

void LoadLayout() {
    g_loaded = true;
    try {
        toml::table t = toml::parse_file(LayoutFile().string());
        g_layout = kRacing;
        for (size_t i = 0; i < game::kHudItems; i++)
            if (const toml::table* p = t["hud"][kHudIds[i]].as_table()) {
                g_hud[i].x = float((*p)["x"].value_or(0.0));
                g_hud[i].y = float((*p)["y"].value_or(0.0));
            }
        PublishHud();
        g_opacity = std::clamp(float(t["opacity"].value_or(double(g_opacity))), 0.2f, 1.0f);
        g_scale = std::clamp(float(t["scale"].value_or(double(g_scale))), 0.5f, 1.5f);
        g_fixed_sticks = t["stick_mode"].value_or(std::string("floating")) == "fixed";
        std::string steering = t["steering"].value_or(std::string(kSteeringIds[0]));
        for (size_t i = 0; i < std::size(kSteeringIds); i++)
            if (steering == kSteeringIds[i]) g_steering = Steering(i);
        int64_t version = t["version"].value_or(int64_t(1));
        // the places of the presets there were besides RACING are not its
        if (version < kOldestLayout || t["preset"].value_or(std::string("racing")) != "racing") return;
        for (size_t i = 0; i < kPlaces; i++) {
            // Black (the camera) and the gear lever were hidden before: kRacing's until saved again
            std::string_view id = PlaceId(i);
            if (version < kLayoutVersion && (id == "black" || id == "rstick")) continue;
            const toml::table* p = t["controls"][PlaceId(i)].as_table();
            if (!p) continue;
            Place& pl = g_layout[i];
            pl.x = std::clamp(float((*p)["x"].value_or(double(pl.x))), 0.0f, 1.0f);
            pl.y = std::clamp(float((*p)["y"].value_or(double(pl.y))), 0.0f, 1.0f);
            pl.r = std::clamp(float((*p)["r"].value_or(double(pl.r))), 0.02f, 0.25f);
            pl.hidden = (*p)["hidden"].value_or(false) && Hideable(int(i));
        }
    } catch (...) {
    }
}

void SaveLayout() {
    toml::table controls;
    for (size_t i = 0; i < kPlaces; i++) {
        const Place& p = g_layout[i];
        controls.insert(PlaceId(i),
                        toml::table{{"x", double(p.x)}, {"y", double(p.y)}, {"r", double(p.r)}, {"hidden", p.hidden}});
    }
    toml::table hud;
    for (size_t i = 0; i < game::kHudItems; i++)
        hud.insert(kHudIds[i], toml::table{{"x", double(g_hud[i].x)}, {"y", double(g_hud[i].y)}});
    toml::table t{{"version", kLayoutVersion}, {"preset", "racing"}, {"opacity", double(g_opacity)},
                  {"scale", double(g_scale)}, {"stick_mode", g_fixed_sticks ? "fixed" : "floating"},
                  {"steering", kSteeringIds[size_t(g_steering.load())]},
                  {"controls", controls}, {"hud", hud}};
    std::ofstream(LayoutFile()) << t << "\n";
}

void EnsureLayout() {
    if (!g_loaded) LoadLayout();
}

void OpenEditor() {
    g_before = g_layout;
    g_before_opacity = g_opacity;
    g_hud_before = g_hud;
    g_selected = -1;
    g_hud_selected = -1;
    g_editing = true;
}

// a menu of Most Wanted's that takes the FEng mouse (src/game/fe_mouse.cpp): a touch there is its pointer, and the
// pad's controls give way to it but for the settings gear
bool FeMenu() { return !g_editing && game::FeMenuShown(); }

constexpr float kTabAspect = 2.3f, kPillAspect = 2.1f, kPedalAspect = 0.6f;

// In a FEng menu the face buttons give way to the menu's own, labelled pills stacked low on the right, the dpad low
// on the left: each pill only while the screen shows its prompt, with its label ("DEFAULTS", the showcase's
// "ZOOM IN"); a screen prompting nothing keeps ENTER (A) and CLOSE (B), its way in and out when no tap reaches its
// accept (no highlighted item, a prompt that is not a mouse region)
int FePrompt(int press) {
    if (press >= kAnalogA && press <= kAnalogY) return press;
    if (press == kAnalogLT) return game::kFePromptLT;
    if (press == kAnalogRT) return game::kFePromptRT;
    return -1;
}
std::string FeLabel(size_t i) {
    if (i >= kButtonCount) return {};
    const Button& b = kButtons[i];
    int prompt = FePrompt(b.press);
    if (!FeMenu() || prompt < 0) return {};
    game::FePrompts p = game::CurrentFePrompts();
    if (b.press == kAnalogA || b.press == kAnalogB) {
        bool any = std::any_of(std::begin(p.shown), std::end(p.shown), [](bool s) { return s; });
        if (any && !p.shown[prompt]) return {};
        if (!p.label[prompt].empty()) return p.label[prompt];
        return app::Ui(b.press == kAnalogA ? "ENTER" : "CLOSE");
    }
    if (!p.shown[prompt]) return {};
    // triggers prompted with no label beside either are no prompt (the options screens' L1 and R1 images): the map's
    // and the showcase's have one at least
    if ((prompt == game::kFePromptLT || prompt == game::kFePromptRT) && p.label[game::kFePromptLT].empty() &&
        p.label[game::kFePromptRT].empty())
        return {};
    // the triggers zoom wherever they are prompted (the showcase, the world map): a label that is no action (the
    // map's L is followed by its zoom, "ZOOM 1.5X", its R by nothing) gives way to ZOOM OUT and ZOOM IN
    if (prompt == game::kFePromptLT || prompt == game::kFePromptRT) {
        auto action = [&](int i) {
            const std::string& l = p.label[i];
            return !l.empty() && std::none_of(l.begin(), l.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        if (!action(game::kFePromptLT) || !action(game::kFePromptRT))
            return app::Ui(prompt == game::kFePromptLT ? "ZOOM OUT" : "ZOOM IN");
    }
    return p.label[prompt].empty() ? std::string(b.label) : p.label[prompt];
}
bool FeFace(size_t i) { return !FeLabel(i).empty(); }
// A, B, X and Y in a FEng menu: their art as a pad's diamond low on the right, the prompt's label under each; the
// triggers, rarer (the map's and the showcase's zoom), stay labelled pills side by side above it
bool FeIcon(size_t i) { return FeFace(i) && kButtons[i].press >= kAnalogA && kButtons[i].press <= kAnalogY; }
// the dpad in a FEng menu: its middle and the buttons' steps from it, in screen heights from the left and the top
constexpr float kFeDpadX = 0.15f, kFeDpadY = 0.74f, kFeDpadStep = 0.095f, kFeDpadR = 0.045f;
bool FeDpad(size_t i) { return i < kButtonCount && FeMenu() && kButtons[i].press >= kBit && kButtons[i].press <= kBit + 3; }
// the diamond's middle in screen heights from the right and the top, its buttons' steps from it and radius; the
// triggers' pills a step and a half above it, either side of its middle
constexpr float kFeDiamondX = 0.19f, kFeDiamondY = 0.70f, kFeDiamondStep = 0.12f, kFeDiamondR = 0.05f;
constexpr float kFeTriggerR = 0.036f;
constexpr float kFeTextSize = 0.8f;  // the label's height over the pill's half height
// a pill's half width: its label's (ImGui's default font is monospaced, 7 of 13 pixels a letter) and some
float FeHalfWidth(const std::string& label, float hh) {
    return std::max(hh * kPillAspect, float(label.size()) * hh * kFeTextSize * 7.0f / 13.0f / 2.0f + hh * 0.7f);
}

// the RACING layout's arrows and wheel show as STEERING picks them, in the editor too
bool SteeringShown(int press) {
    Steering s = g_steering;
    if (press == kSteerLeft || press == kSteerRight) return s == Steering::Arrows;
    if (press == kSteerWheel) return s == Steering::Wheel;
    return true;
}

bool ButtonShown(size_t i) {
    const Button& b = kButtons[i];
    if (!SteeringShown(b.press)) return false;
    if (g_editing) return true;
    if (FeFace(i) || FeDpad(i)) return true;
    if (g_layout[i].hidden) return false;
    if (FeMenu()) return b.keep;  // the settings gear alone
    if (!game::Racing()) return b.keep;  // no pad out of a race: the front end's screens, the loading screens
    return true;
}

// ---- the controls' art (tools/touch_art.py, the APK's assets/touch): white on clear, in ImGui's font atlas, so the
// draw lists draw it with no texture of their own. A face button's and a pedal's carry the pad's letter in a badge
// over the body
struct Art {
    const char* name;
    float body;  // the body's height over the image's width, the badge left out
    int rect = -1;  // the atlas' custom rect, -1 while not loaded
    float aspect = 1;  // the image's height over its width
    ImVec2 uv0, uv1;
};
Art g_art[] = {{"a", 1.0f},     {"b", 1.0f},     {"x", 1.0f},          {"y", 1.0f},
               {"lt", 0.698f},  {"rt", 1.2345f}, {"black", 1.0f},      {"start", 1.0f},
               {"back", 1.0f},  {"up", 1.0f},    {"down", 1.0f},       {"left", 1.0f},
               {"right", 1.0f}, {"wheel", 1.0f}, {"stick_ring", 1.0f}, {"stick_knob", 1.0f},
               {"stick_shift", 1.66f}, {"reset_car", 1.0f}};  // reset_car: tools/reset_icon.py

const Art* FindArt(std::string_view name) {
    for (const Art& a : g_art)
        if (a.rect >= 0 && name == a.name) return &a;
    return nullptr;
}

// a control's art: by its id, the dpad and the steering arrows sharing the arrows; nullptr to draw it as before
const Art* ArtOf(size_t i) {
    if (i >= kButtonCount || (FeFace(i) && !FeIcon(i))) return nullptr;
    std::string_view id = kButtons[i].id;
    if (id.starts_with("dpad_")) id.remove_prefix(5);
    else if (id.starts_with("steer_")) id.remove_prefix(6);
    return FindArt(id);
}

// the art's body centred on c, hh its half height, the badge above it; turned by angle radians about c
void DrawArt(ImDrawList* dl, const Art& a, ImVec2 c, float hh, ImU32 tint, float angle = 0) {
    float hw = hh / a.body;
    float top = hh - 2 * hw * a.aspect;  // from c, the badge's top
    ImVec2 corner[4] = {{-hw, top}, {hw, top}, {hw, hh}, {-hw, hh}};
    float s = std::sin(angle), k = std::cos(angle);
    for (ImVec2& p : corner) p = ImVec2(c.x + p.x * k - p.y * s, c.y + p.x * s + p.y * k);
    dl->PrimReserve(6, 4);
    dl->PrimQuadUV(corner[0], corner[1], corner[2], corner[3], a.uv0, ImVec2(a.uv1.x, a.uv0.y), a.uv1,
                   ImVec2(a.uv0.x, a.uv1.y), tint);
}

// a control's place: its layout's, or the FEng menus' own for a face button, a trigger and the dpad
Place PlaceOf(size_t i) {
    float h = Height(), w = std::max(Width(), 1.0f);
    if (i < kButtonCount && FeFace(i)) {
        // A bottom, B right, X left, Y top, as on the pad; LT left and RT right above
        constexpr float kStep[game::kFePromptCount][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}, {-0.9f, -2.3f}, {0.9f, -2.3f}};
        const float* s = kStep[FePrompt(kButtons[i].press)];
        float x = w - (kFeDiamondX - s[0] * kFeDiamondStep) * h;
        return {x / w, kFeDiamondY + s[1] * kFeDiamondStep, FeIcon(i) ? kFeDiamondR : kFeTriggerR};
    }
    if (FeDpad(i)) {
        constexpr float kStep[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};  // up, down, left, right
        const float* s = kStep[kButtons[i].press - kBit];
        return {(kFeDpadX + s[0] * kFeDpadStep) * h / w, kFeDpadY + s[1] * kFeDpadStep, kFeDpadR};
    }
    return g_layout[i];
}

void Centre(size_t i, float& cx, float& cy) {
    Place p = PlaceOf(i);
    cx = p.x * Width();
    cy = p.y * Height();
}

float Radius(size_t i) { return PlaceOf(i).r * g_scale * Height(); }

bool Pedal(size_t i) { return kButtons[i].press == kAnalogLT || kButtons[i].press == kAnalogRT; }

// a control's shape: the triggers are pedals
Shape ShapeOf(size_t i) {
    if (FeIcon(i)) return Shape::Round;
    if (FeFace(i)) return Shape::Pill;
    if (const Art* a = ArtOf(i)) return a->body == 1.0f ? Shape::Round : Shape::Pedal;  // a pedal's art: its own size
    return Pedal(i) ? Shape::Pedal : kButtons[i].shape;
}

// a control's half width and half height, pixels
void Extent(size_t i, float& hw, float& hh) {
    hh = Radius(i);
    if (FeIcon(i)) {
        hw = hh;
        return;
    }
    if (std::string label = FeLabel(i); !label.empty()) {
        hw = FeHalfWidth(label, hh);
        return;
    }
    if (const Art* a = ArtOf(i)) {
        hw = hh / a->body;
        return;
    }
    Shape s = i < kButtonCount ? ShapeOf(i) : Shape::Round;
    hw = hh * (s == Shape::Tab ? kTabAspect : s == Shape::Pill ? kPillAspect : s == Shape::Pedal ? kPedalAspect : 1.0f);
}

// the editor's bar at the top: Reset, smaller and bigger for the picked control, hide or show it, Cancel, Done, in
// screen shares
constexpr const char* kBarLabels[] = {"RESET", "-", "+", "HIDE", "CANCEL", "DONE"};
enum { kBarReset, kBarSmaller, kBarBigger, kBarHide, kBarCancel, kBarDone, kBarCount };
constexpr float kBarY = 0.17f, kBarH = 0.09f, kBarW = 0.13f, kBarGap = 0.02f;
constexpr float kBarSizeW = 0.07f;                       // the - and + buttons
constexpr float kMinRadius = 0.02f, kMaxRadius = 0.25f;  // a control's own radius, in screen heights
constexpr float kSizeStep = 1.1f;                        // a tap on - or +

float BarWidth(int i) { return i == kBarSmaller || i == kBarBigger ? kBarSizeW : kBarW; }

// the bar's button under a point, -1 for none
int BarAt(float x, float y, float rect[4] = nullptr, int which = -1) {
    float w = Width(), h = Height();
    float total = (kBarCount - 1) * kBarGap;
    for (int i = 0; i < kBarCount; i++) total += BarWidth(i);
    float left = 0.5f - total / 2;
    for (int i = 0; i < kBarCount; i++) {
        float x0 = left * w, y0 = kBarY * h;
        float x1 = x0 + BarWidth(i) * w, y1 = y0 + kBarH * h;
        left += BarWidth(i) + kBarGap;
        if (i == which && rect) {
            rect[0] = x0, rect[1] = y0, rect[2] = x1, rect[3] = y1;
            return i;
        }
        if (which < 0 && x >= x0 && x <= x1 && y >= y0 && y <= y1) return i;
    }
    return -1;
}

void ResetLayout() {
    g_layout = kRacing;
    g_hud = {};
    PublishHud();
    g_selected = -1;
    g_hud_selected = -1;
}

void CloseEditor(bool keep) {
    if (keep) {
        SaveLayout();
    } else {
        g_layout = g_before;
        g_opacity = g_before_opacity;
        g_hud = g_hud_before;
        PublishHud();
    }
    g_selected = -1;
    g_hud_selected = -1;
    g_editing = false;
}

// the button under a point, kButtonCount for none
size_t HitButton(float x, float y) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!ButtonShown(i)) continue;
        float cx, cy, hw, hh;
        Centre(i, cx, cy);
        Extent(i, hw, hh);
        if (ShapeOf(i) == Shape::Round) {
            float r = hh * 1.25f;  // a little slack around the drawn circle
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        } else if (std::fabs(x - cx) <= hw + hh * 0.2f && std::fabs(y - cy) <= hh * 1.2f) {
            return i;
        }
    }
    return kButtonCount;
}

// The steering arrows are one strip under the thumb, a racing game's: a finger down anywhere along it, the gap between
// them too, takes the nearer arrow, and rolled across it steers the other way. Held to the arrow it came down on, a
// thumb rolled from left to right still steered left until lifted, and one down in the gap steered nothing
size_t SteerArrowAt(float x, float y) {
    size_t left = kButtonCount, right = kButtonCount;
    for (size_t i = 0; i < kButtonCount; i++) {
        if (kButtons[i].press == kSteerLeft) left = i;
        if (kButtons[i].press == kSteerRight) right = i;
    }
    if (left == kButtonCount || right == kButtonCount || !ButtonShown(left) || !ButtonShown(right)) return kButtonCount;
    float lx, ly, rx, ry, lw, lh, rw, rh;
    Centre(left, lx, ly);
    Centre(right, rx, ry);
    Extent(left, lw, lh);
    Extent(right, rw, rh);
    if (lx > rx) return kButtonCount;  // the editor put them the other way round: each its own button
    constexpr float kSlack = 1.4f;  // the strip's reach past the arrows, in their half sizes
    float hh = std::max(lh, rh) * kSlack, my = (ly + ry) / 2;
    if (x < lx - lw * kSlack || x > rx + rw * kSlack || std::fabs(y - my) > hh + std::fabs(ly - ry) / 2) return kButtonCount;
    return x - lx < rx - x ? left : right;
}

// a stick's deflection from where the finger came down, its radius the stick's
void StickOf(const Finger& f, float& x, float& y, size_t stick = kStick) {
    float r = Radius(stick);
    float dx = (f.x - f.x0) / r, dy = (f.y - f.y0) / r;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1) {
        dx /= len;
        dy /= len;
    }
    x = dx;
    y = -dy;
}

void UpdateStick(const Finger& f) { StickOf(f, g_stick_x, g_stick_y); }

// FLOATING: the left stick's zone is the side of the screen it is placed on, kStickZone of the width at least and out
// to its place's reach, so a stick moved in the editor still takes the touches where it is drawn
bool InStickZone(float x) {
    float w = Width(), cx = g_layout[kStick].x * w, reach = Radius(kStick) * kFixedReach;
    if (g_layout[kStick].x <= 0.5f) return x < std::max(w * kStickZone, cx + reach);
    return x > std::min(w * (1 - kStickZone), cx - reach);
}

// FIXED sticks: the one a touch came down near, its deflection from the centre; kPlaces for none
size_t FixedStickAt(float x, float y) {
    for (size_t s : {kStick, kRStick}) {
        if (g_layout[s].hidden) continue;
        float cx = g_layout[s].x * Width(), cy = g_layout[s].y * Height(), r = Radius(s) * kFixedReach;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return s;
    }
    return kPlaces;
}

// a control held past its going (a menu's ENTER into the loading screen that follows) is let go: an A landing on a
// loading screen crashed it
void Recount() {
    std::fill(std::begin(g_pressed), std::end(g_pressed), false);
    bool stick = false, rstick = false;
    for (const Finger& f : g_fingers) {
        if (f.role == Role::Button && ButtonShown(f.button)) g_pressed[f.button] = true;
        if (f.role == Role::Stick) stick = true;
        if (f.role == Role::Look) rstick = true;
    }
    if (!stick) g_stick_x = g_stick_y = 0;
    if (!rstick) g_rstick_x = g_rstick_y = 0;
}

// Pad presses the FEng menus get from touches, one after another: a swipe's dpad steps (the list follows the finger:
// a swipe left or up goes to the next item, right or down; an arrow per kSwipeStep of the swipe), a tap on the
// highlighted item's A (src/game/fe_mouse.cpp)
constexpr float kSwipe = 0.06f, kSwipeStep = 0.12f;  // screen heights
constexpr int kMaxSwipeSteps = 6;
constexpr uint64_t kFePressMs = 60, kFeGapMs = 60;
struct FeQueue {
    int press = -1;  // kBit + dpad bit or kAnalogA
    int left = 0;
    uint64_t until = 0;  // the press is held until then, then let go until a gap later
    bool down = false;
} g_fe_queue;

// a tap's A waits this long, for FEng's mouse to have answered it if it does (src/game/fe_mouse.cpp's FeTakeAccept)
constexpr uint64_t kFeAcceptMs = 150;
uint64_t g_fe_accept_at = 0;
bool g_panel_after_pause = false;  // the gear's Start is queued: the settings panel opens when it is out

void FeQueuePresses(int press, int count) {
    if (g_fe_queue.left && g_fe_queue.press != press) g_fe_queue.left = 0;
    g_fe_queue.press = press;
    g_fe_queue.left += count;
}

// the press held now, -1 for none
int FeQueuePress() {
    FeQueue& q = g_fe_queue;
    uint64_t now = NowMs();
    if (now < q.until) return q.down ? q.press : -1;
    if (q.down) {
        q.down = false;
        q.until = now + kFeGapMs;
        return -1;
    }
    if (q.left <= 0) return -1;
    q.left--;
    q.down = true;
    q.until = now + kFePressMs;
    return q.press;
}

// a finger lifted from a FEng menu: a swipe steps the dpad, a tap that came down on the highlighted item presses A
void FeSwipe(const Finger& f) {
    float h = Height(), dx = f.x - f.x0, dy = f.y - f.y0;
    float along = std::max(std::fabs(dx), std::fabs(dy));
    if (along < kSwipe * h) {
        g_fe_accept_at = NowMs() + kFeAcceptMs;
        return;
    }
    game::FeTakeAccept();  // a swipe accepts nothing
    int steps = std::clamp(int(along / (kSwipeStep * h) + 0.5f), 1, kMaxSwipeSteps);
    int dpad = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? 3 : 2) : (dy < 0 ? 1 : 0);  // up down left right
    FeQueuePresses(kBit + dpad, steps);
}

// the FEng mouse at a finger, in shares of the frame as the presenter letterboxes it
void FePointer(const Finger& f, bool down) {
    float w = Width(), h = Height(), aspect = game::Aspect();
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    game::FeTouch(down, (f.x - (w - vw) / 2) / vw, (f.y - (h - vh) / 2) / vh);
}

// FEng's screen in the frame as the presenter letterboxes it: 480 high, and 480 times the frame's aspect wide with
// the console's widescreen flag (3:2 and wider), else the 640 wide layout stretched (src/game/fe_mouse.cpp)
struct FeFrame {
    float x0, y0, w, h;  // the frame on the screen, pixels
    float wide;          // FEng's width
};
FeFrame FeScreen() {
    float w = Width(), h = Height(), aspect = game::Aspect();
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    return {(w - vw) / 2, (h - vh) / 2, vw, vh, aspect >= 1.5f ? 480.0f * aspect : 640.0f};
}

// a HUD item's box on the screen where it is moved to: its middle and half sizes, pixels
void HudRect(size_t i, float& cx, float& cy, float& hw, float& hh) {
    FeFrame f = FeScreen();
    const game::HudBox& b = game::kHudBoxes[i];
    cx = f.x0 + (0.5f + (b.x + g_hud[i].x) / f.wide) * f.w;
    cy = f.y0 + (0.5f + (b.y + g_hud[i].y) / 480.0f) * f.h;
    hw = b.hw / f.wide * f.w;
    hh = b.hh / 480.0f * f.h;
}

// the HUD item under a point, -1 for none
int HitHud(float x, float y) {
    for (size_t i = 0; i < game::kHudItems; i++) {
        float cx, cy, hw, hh;
        HudRect(i, cx, cy, hw, hh);
        if (std::fabs(x - cx) <= hw && std::fabs(y - cy) <= hh) return int(i);
    }
    return -1;
}

// a HUD item dragged by a finger from where it was as the drag began; its box kept in the frame
void DragHud(size_t i, float dx, float dy) {
    FeFrame f = FeScreen();
    const game::HudBox& b = game::kHudBoxes[i];
    float lim_x = f.wide / 2 - b.hw, lim_y = 240.0f - b.hh;
    g_hud[i].x = std::clamp(b.x + g_hud_drag_from.x + dx / f.w * f.wide, -lim_x, lim_x) - b.x;
    g_hud[i].y = std::clamp(b.y + g_hud_drag_from.y + dy / f.h * 480.0f, -lim_y, lim_y) - b.y;
    PublishHud();
}

void Resize(size_t i, float r) { g_layout[i].r = std::clamp(r, kMinRadius, kMaxRadius); }

// the editor: a touch on the bar resets, resizes the picked control, cancels or saves; a second finger while one
// drags a control pinches it bigger or smaller; on a control picks it up to move it, anywhere else lets go of the pick
void EditDown(Finger& f, float x, float y) {
    int bar = BarAt(x, y);
    switch (bar) {
        case kBarReset:
            ResetLayout();
            return;
        case kBarSmaller:
        case kBarBigger:
            if (g_selected >= 0) {
                float r = g_layout[size_t(g_selected)].r;
                Resize(size_t(g_selected), bar == kBarBigger ? r * kSizeStep : r / kSizeStep);
            }
            return;
        case kBarHide:
            if (Hideable(g_selected)) g_layout[size_t(g_selected)].hidden = !g_layout[size_t(g_selected)].hidden;
            return;
        case kBarCancel:
            CloseEditor(false);
            return;
        case kBarDone:
            CloseEditor(true);
            return;
        default:
            break;
    }
    for (const Finger& d : g_fingers) {
        if (d.role != Role::Drag) continue;
        // the span between the fingers and the control's size as the pinch began
        f.role = Role::Pinch;
        f.button = d.button;
        f.x0 = std::max(std::hypot(x - d.x, y - d.y), 1.0f);
        f.y0 = g_layout[d.button].r;
        return;
    }
    // kPlaces for none: kButtonCount, HitButton's none, is the left stick's place
    size_t b = HitButton(x, y);
    if (b == kButtonCount) {
        b = kPlaces;
        for (size_t s : {kStick, kRStick}) {
            float sx = g_layout[s].x * Width(), sy = g_layout[s].y * Height(), sr = Radius(s);
            if (b == kPlaces && (x - sx) * (x - sx) + (y - sy) * (y - sy) <= sr * sr) b = s;
        }
    }
    // else a HUD item, under the controls
    if (b == kPlaces) {
        g_selected = -1;
        g_hud_selected = HitHud(x, y);
        if (g_hud_selected >= 0) {
            f.role = Role::HudDrag;
            f.button = size_t(g_hud_selected);
            g_hud_drag_from = g_hud[f.button];
        }
        return;
    }
    g_hud_selected = -1;
    g_selected = int(b);
    f.role = Role::Drag;
    f.button = b;
    f.x0 = x - g_layout[b].x * Width();
    f.y0 = y - g_layout[b].y * Height();
}

// a finger's angle round the wheel's centre, degrees clockwise from the right
float WheelAngle(size_t wheel, float x, float y) {
    float cx, cy;
    Centre(wheel, cx, cy);
    return std::atan2(y - cy, x - cx) * 57.2957795f;
}

// the wheel turns as the finger goes round it, by the angle it went since its last move, up to a lock either way
void TurnWheel(Finger& f) {
    float a = WheelAngle(f.button, f.x, f.y);
    float d = a - f.angle;
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    f.angle = a;
    // near the centre a small move swings the angle round: those are let go
    float cx, cy;
    Centre(f.button, cx, cy);
    if (std::hypot(f.x - cx, f.y - cy) < Radius(f.button) * 0.2f) return;
    g_wheel_turn = std::clamp(g_wheel_turn + d, -kWheelLock, kWheelLock);
}

void FingerDown(SDL_FingerID id, float x, float y) {
    // Android gives a new finger the id of one lifted: one still here lost its lift, and kept, it would move with the
    // new finger (a stick pushed by a touch on the other side)
    std::erase_if(g_fingers, [id](const Finger& o) { return o.id == id; });
    Finger f;
    f.id = id;
    f.x0 = f.x = x;
    f.y0 = f.y = y;
    if (g_editing) {
        EditDown(f, x, y);
        g_fingers.push_back(f);
        return;
    }
    input::g_touch = true;
    size_t b = HitButton(x, y);
    if (b == kButtonCount) b = SteerArrowAt(x, y);
    if (b < kButtonCount && kButtons[b].press == kSteerWheel) {
        f.role = Role::Wheel;
        f.button = b;
        f.angle = WheelAngle(b, x, y);
        g_wheel_held = true;
    } else if (b < kButtonCount) {
        f.role = Role::Button;
        f.button = b;
        f.down_ms = NowMs();
        // the gear opens and closes the settings panel; in a race it pauses first (Start), as the panel blanks the
        // pad and the race would go on under it, and opens once the press is out (Apply)
        if (kButtons[b].press == kPanel) {
            if (app::MenuOpen()) {
                app::ToggleMenu();
            } else if (game::Racing() && !FeMenu()) {
                FeQueuePresses(kBit + 4, 1);
                g_panel_after_pause = true;
            } else {
                app::ToggleMenu();
            }
        }
        if (kButtons[b].press == kResetCar) game::g_reset_car = true;
    } else if (FeMenu() && game::CurrentFePrompts().right_stick) {
        f.role = Role::FeTap;  // FEng's pointer would click whatever the drag began on (the map's GPS)
    } else if (FeMenu()) {
        f.role = Role::FeMouse;
        FePointer(f, true);
    } else if (g_fixed_sticks || g_layout[kStick].hidden) {
        // with no left stick (RACING) the gear lever is taken where it sits: floating, a thumb off a pedal shifted
        size_t s = FixedStickAt(x, y);
        f.role = s == kStick ? Role::Stick : s == kRStick ? Role::Look : Role::None;
        if (s != kPlaces) {
            f.x0 = g_layout[s].x * Width();
            f.y0 = g_layout[s].y * Height();
            if (s == kStick) UpdateStick(f);
            else StickOf(f, g_rstick_x, g_rstick_y, kRStick);
        }
    } else if (!g_layout[kStick].hidden && InStickZone(x)) {
        f.role = Role::Stick;
    } else {
        f.role = g_layout[kRStick].hidden ? Role::None : Role::Look;
    }
    g_fingers.push_back(f);
    Recount();
}

void FingerMove(SDL_FingerID id, float x, float y) {
    for (Finger& f : g_fingers) {
        if (f.id != id) continue;
        f.x = x;
        f.y = y;
        if (f.role == Role::HudDrag) DragHud(f.button, f.x - f.x0, f.y - f.y0);
        if (f.role == Role::Drag) {
            Place& p = g_layout[f.button];
            p.x = std::clamp((x - f.x0) / Width(), 0.0f, 1.0f);
            p.y = std::clamp((y - f.y0) / Height(), 0.0f, 1.0f);
        }
        // a pinch sizes the control by the span between its two fingers, whichever moved
        if (f.role == Role::Drag || f.role == Role::Pinch) {
            for (const Finger& o : g_fingers) {
                const Finger& pinch = f.role == Role::Pinch ? f : o;
                const Finger& drag = f.role == Role::Pinch ? o : f;
                if (&o == &f || pinch.role != Role::Pinch || drag.role != Role::Drag || pinch.button != drag.button) continue;
                Resize(f.button, pinch.y0 * std::hypot(pinch.x - drag.x, pinch.y - drag.y) / pinch.x0);
            }
        }
        if (f.role == Role::Stick) UpdateStick(f);
        // a thumb rolled across the steering arrows steers the way it is now, the turn coming in again from the start
        if (f.role == Role::Button && (kButtons[f.button].press == kSteerLeft || kButtons[f.button].press == kSteerRight))
            if (size_t s = SteerArrowAt(x, y); s != kButtonCount && s != f.button) {
                f.button = s;
                f.down_ms = NowMs();
            }
        // a menu prompting the right stick (the showcase turning the car, the map's cursor): a drag past a tap's reach
        // is the stick from where it came down
        if (f.role == Role::FeTap && std::hypot(f.x - f.x0, f.y - f.y0) >= kSwipe * Height()) f.role = Role::Look;
        if (f.role == Role::FeMouse) FePointer(f, true);
        if (f.role == Role::Look) StickOf(f, g_rstick_x, g_rstick_y, kRStick);
        if (f.role == Role::Wheel) TurnWheel(f);
        break;
    }
}

void FingerUp(SDL_FingerID id) {
    for (const Finger& f : g_fingers)
        if (f.id == id && f.role == Role::Wheel) g_wheel_held = false;
    for (const Finger& f : g_fingers)
        if (f.id == id && f.role == Role::FeMouse) {
            FePointer(f, false);
            FeSwipe(f);
        } else if (f.id == id && f.role == Role::FeTap) {  // a tap after all: FEng's pointer pressed there and let go
            FePointer(f, true);
            FePointer(f, false);
            FeSwipe(f);
        }
    std::erase_if(g_fingers, [id](const Finger& f) { return f.id == id; });
    Recount();
}

bool PadPlugged() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) SDL_free(ids);
    return n > 0;
}

// verbose builds: F9 writes the image and the physical memory to dump_N.bin beside the settings, for diffing
void DumpGuest() {
    static int n = 0;
    std::filesystem::path p = app::ExeDir() / ("dump_" + std::to_string(n++) + ".bin");
    std::ofstream f(p, std::ios::binary);
    // the image, 0x10000 to 0x508040 (default.xbe's base and size), then the physical memory
    f.write(static_cast<const char*>(kernel::HostPtr(0x10000)), 0x508040 - 0x10000);
    f.write(static_cast<const char*>(kernel::HostPtr(0x80000000u)), 64u << 20);
    LOGI("dump: %s", p.c_str());
}

void KeyEvent(const SDL_KeyboardEvent& k) {
    // Android's back key: out of the layout editor or the settings panel, else the game's pause menu (its options
    // are the settings), in the game's menus back out of them
    if (k.scancode == SDL_SCANCODE_AC_BACK) {
        if (!k.down || k.repeat) return;
        std::lock_guard lock(g_lock);
        if (g_editing) {
            CloseEditor(false);
        } else if (app::MenuOpen()) {
            app::ToggleMenu();
        } else {
            g_back_until = NowMs() + kBackTapMs;
        }
        return;
    }
    int vk = input::VkOf(k.scancode);
    if (!vk) return;
    input::g_down[size_t(vk)] = k.down;
    if (!k.down) return;
    input::g_keyboard = true;
    input::g_touch = false;
    if (logx::kVerbose && !k.repeat && vk == VK_F9) DumpGuest();
    if (!k.repeat && input::PanelKey(vk)) app::ToggleMenu();
    else if (vk == VK_ESCAPE && app::MenuOpen() && !app::Capturing()) app::ToggleMenu();
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(e.key);
            return app::MenuOpen() ? false : true;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) return true;
            input::g_keyboard = true;
            input::g_touch = false;
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) return true;
            if (int vk = input::VkOfButton(e.button.button)) input::g_down[size_t(vk)] = e.button.down;
            input::g_keyboard = true;
            input::g_touch = false;
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y != 0) input::g_wheel_ms[e.wheel.y < 0] = NowMs();
            return !app::MenuOpen();
        case SDL_EVENT_WILL_ENTER_BACKGROUND: {
            // the fingers' lifts may not come while the app is away: none is held when it is back
            std::lock_guard lock(g_lock);
            g_fingers.clear();
            Recount();
            return false;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            g_touch_ms = NowMs();
            float x = e.tfinger.x * Width(), y = e.tfinger.y * Height();
            std::lock_guard lock(g_lock);
            EnsureLayout();
            if (e.type == SDL_EVENT_FINGER_DOWN) {
                // the settings panel takes the touches, but for the button that closes it
                if (app::MenuOpen() && !g_editing) {
                    size_t b = HitButton(x, y);
                    if (b == kButtonCount || kButtons[b].press != kPanel) return false;
                }
                FingerDown(e.tfinger.fingerID, x, y);
                return true;
            }
            bool ours = std::any_of(g_fingers.begin(), g_fingers.end(),
                                    [&](const Finger& f) { return f.id == e.tfinger.fingerID; });
            if (!ours) return false;
            if (e.type == SDL_EVENT_FINGER_MOTION) FingerMove(e.tfinger.fingerID, x, y);
            else FingerUp(e.tfinger.fingerID);
            return true;
        }
        default:
            return false;
    }
}

void Apply(int port, input::Pad& pad) {
    if (port != 0 || g_editing) return;
    std::lock_guard lock(g_lock);
    Recount();  // a control the screen no longer shows is let go
    bool used = false;
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!g_pressed[i]) continue;
        int p = kButtons[i].press;
        if (p < 0) continue;
        used = true;
        if (p < kBit) pad.analog[p] = 0xFF;
        else pad.buttons |= uint16_t(1u << (p - kBit));
    }
    if (g_fe_accept_at && NowMs() >= g_fe_accept_at) {
        g_fe_accept_at = 0;
        if (game::FeTakeAccept()) FeQueuePresses(kAnalogA, 1);
    }
    if (int p = FeQueuePress(); p >= 0) {
        used = true;
        if (p < kBit) pad.analog[p] = 0xFF;
        else pad.buttons |= uint16_t(1u << (p - kBit));
    } else if (g_panel_after_pause && !g_fe_queue.left) {
        g_panel_after_pause = false;
        if (!app::MenuOpen()) app::ToggleMenu();
    }
    if (NowMs() < g_back_until) {
        used = true;
        if (FeMenu()) pad.analog[kAnalogB] = 0xFF;
        else pad.buttons |= uint16_t(1u << 4);  // Start
    }
    // a steering arrow turns in over kSteerRampMs from kSteerStart of a full lock, as a racing game's do: full at
    // once twitched the car on a tap
    constexpr float kSteerStart = 0.4f, kSteerRampMs = 250.0f;
    float steer = 0;
    for (const Finger& f : g_fingers) {
        if (f.role != Role::Button || (kButtons[f.button].press != kSteerLeft && kButtons[f.button].press != kSteerRight))
            continue;
        float k = std::min(1.0f, kSteerStart + (1 - kSteerStart) * float(NowMs() - f.down_ms) / kSteerRampMs);
        steer += kButtons[f.button].press == kSteerRight ? k : -k;
    }
    // the wheel let go comes back to the middle in about a fifth of a second; the tilt steers out of the menus
    constexpr float kWheelReturn = 0.6f;
    if (!g_wheel_held) g_wheel_turn = std::fabs(g_wheel_turn) < 1.0f ? 0.0f : g_wheel_turn * kWheelReturn;
    if (g_steering == Steering::Wheel) steer += g_wheel_turn / kWheelLock;
    if (g_steering == Steering::Tilt && !FeMenu() && !app::MenuOpen()) steer += gyro::Steer();
    float sx = std::clamp(g_stick_x + steer, -1.0f, 1.0f);
    if (sx || g_stick_y) {
        used = true;
        pad.lx = int16_t(sx * 32767.0f);
        pad.ly = int16_t(std::clamp(g_stick_y, -1.0f, 1.0f) * 32767.0f);
    }
    if (g_rstick_x || g_rstick_y) {
        used = true;
        pad.rx = int16_t(std::clamp(g_rstick_x, -1.0f, 1.0f) * 32767.0f);
        pad.ry = int16_t(std::clamp(g_rstick_y, -1.0f, 1.0f) * 32767.0f);
    }
    if (used) pad.connected = true;
}

bool Shown() { return !PadPlugged() || NowMs() - g_touch_ms < kShownMs; }

namespace {

constexpr float kPi = 3.14159265f;

// an icon in white line art inside a button of radius r: points in units of r, from its centre, y down
struct Pen {
    ImDrawList* dl;
    ImVec2 c;
    float r;
    ImU32 col;
    float th;  // line thickness
    ImVec2 P(float x, float y) const { return ImVec2(c.x + x * r, c.y + y * r); }
    // a point turned by angle a about the centre
    ImVec2 R(float x, float y, float a) const {
        float s = std::sin(a), k = std::cos(a);
        return P(x * k - y * s, x * s + y * k);
    }
    void Line(float x0, float y0, float x1, float y1) const { dl->AddLine(P(x0, y0), P(x1, y1), col, th); }
    void Dot(float x, float y, float rad) const { dl->AddCircleFilled(P(x, y), rad * r, col, 24); }
    void Box(float x0, float y0, float x1, float y1, float round = 0) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), col, round * r);
    }
};

void DrawIcon(const Pen& p, Icon icon) {
    switch (icon) {
        case Icon::Text:  // its label, Draw writes it
            break;
        case Icon::Pause:
            p.Box(-0.28f, -0.36f, -0.08f, 0.36f, 0.03f);
            p.Box(0.08f, -0.36f, 0.28f, 0.36f, 0.03f);
            break;
        case Icon::Gear:
            for (int i = 0; i < 8; i++) {
                float a = i * kPi / 4;
                p.dl->AddLine(p.R(0.3f, 0, a), p.R(0.52f, 0, a), p.col, p.th * 2.4f);
            }
            p.Dot(0, 0, 0.36f);
            p.dl->AddCircleFilled(p.c, 0.15f * p.r, IM_COL32(0, 0, 0, 200), 20);
            break;
        case Icon::Gyro: {  // a phone on its side between two turning arrows
            p.dl->AddRect(p.P(-0.32f, -0.2f), p.P(0.32f, 0.2f), p.col, 0.06f * p.r, 0, p.th);
            p.Dot(0.24f, 0, 0.035f);
            for (float side : {-1.0f, 1.0f}) {
                float a0 = side < 0 ? 0.62f * kPi : -0.38f * kPi, a1 = a0 + 0.76f * kPi * 0.5f;
                p.dl->PathArcTo(p.c, 0.5f * p.r, a0, a1, 12);
                p.dl->PathStroke(p.col, 0, p.th);
                ImVec2 tip = p.P(std::cos(a1) * 0.5f, std::sin(a1) * 0.5f);
                float t = a1 + kPi / 2;
                p.dl->AddTriangleFilled(tip, ImVec2(tip.x - std::cos(t - 0.5f) * 0.16f * p.r, tip.y - std::sin(t - 0.5f) * 0.16f * p.r),
                                        ImVec2(tip.x - std::cos(t + 0.5f) * 0.16f * p.r, tip.y - std::sin(t + 0.5f) * 0.16f * p.r), p.col);
            }
            break;
        }
        case Icon::Back:
        case Icon::Left:
            p.Line(0.14f, -0.36f, -0.22f, 0);
            p.Line(-0.22f, 0, 0.14f, 0.36f);
            break;
        case Icon::Right:
            p.Line(-0.14f, -0.36f, 0.22f, 0);
            p.Line(0.22f, 0, -0.14f, 0.36f);
            break;
        case Icon::Up:
            p.Line(-0.36f, 0.14f, 0, -0.22f);
            p.Line(0, -0.22f, 0.36f, 0.14f);
            break;
        case Icon::Down:
            p.Line(-0.36f, -0.14f, 0, 0.22f);
            p.Line(0, 0.22f, 0.36f, -0.14f);
            break;
        case Icon::Wheel:  // drawn whole by DrawWheel
            break;
    }
}

// a colour at the layout's opacity
ImU32 Fade(int r, int g, int b, int a) { return IM_COL32(r, g, b, int(float(a) * g_opacity)); }

// the controls' look: dark glass edged in warm parchment, saffron when pressed, the face buttons rimmed in their colour
struct Rgb {
    int r, g, b;
};
constexpr Rgb kGlass = {28, 22, 16}, kEdge = {235, 205, 150}, kSaffron = {224, 138, 44}, kJade = {62, 158, 124},
              kCream = {240, 228, 205};
ImU32 Fade(Rgb c, int a) { return Fade(c.r, c.g, c.b, a); }

// the Xbox controller's colour of a button: A green, B red, X blue, Y yellow, the triggers saffron, Black and White
// jade, the rest the edge's
Rgb ButtonColour(int press) {
    switch (press) {
        case kAnalogA: return {95, 196, 110};
        case kAnalogB: return {224, 86, 76};
        case kAnalogX: return {111, 159, 224};
        case kAnalogY: return {232, 190, 72};
        case kAnalogLT:
        case kAnalogRT: return kSaffron;
        case 4:
        case 5: return kJade;
        default: return kEdge;
    }
}

// the art white at the layout's opacity, warm while pressed
ImU32 ArtTint(bool on, int fade) { return on ? Fade(255, 190, 120, 255) : Fade(255, 255, 255, 255 * fade / 100); }

// a stick with no art: glass, its edge, the four directions and its knob
void DrawStickShapes(ImDrawList* dl, float th, ImVec2 at, float r, float sx, float sy, bool held) {
    dl->AddCircleFilled(at, r, Fade(kGlass, held ? 165 : 120), 64);
    dl->AddCircle(at, r, Fade(kEdge, held ? 110 : 70), 64, th * 0.7f);
    dl->AddCircle(at, r * 0.7f, Fade(kEdge, 30), 64, th * 0.5f);
    for (int i = 0; i < 4; i++) {  // the four directions
        float a = i * kPi / 2;
        ImVec2 tip(at.x + std::cos(a) * r * 0.88f, at.y + std::sin(a) * r * 0.88f);
        ImVec2 l(at.x + std::cos(a + 0.12f) * r * 0.76f, at.y + std::sin(a + 0.12f) * r * 0.76f);
        ImVec2 rr(at.x + std::cos(a - 0.12f) * r * 0.76f, at.y + std::sin(a - 0.12f) * r * 0.76f);
        dl->AddTriangleFilled(tip, l, rr, Fade(kEdge, held ? 150 : 95));
    }
    ImVec2 knob(at.x + sx * r, at.y - sy * r);
    dl->AddCircleFilled(knob, r * 0.42f, held ? Fade(kSaffron, 150) : Fade(60, 48, 36, 170), 48);
    dl->AddCircle(knob, r * 0.42f, Fade(kEdge, held ? 200 : 120), 48, th * 0.8f);
}

void DrawStick(ImDrawList* dl, float th, size_t stick) {
    // a hidden right stick: faint and crossed out in the editor, else not drawn
    bool hidden = g_layout[stick].hidden;
    if (hidden && !g_editing) return;
    float opacity = g_opacity;
    if (hidden) g_opacity *= 0.4f;
    // at rest in its place, else where the thumb came down
    float r = Radius(stick);
    ImVec2 at(g_layout[stick].x * Width(), g_layout[stick].y * Height());
    Role role = stick == kStick ? Role::Stick : Role::Look;
    float sx = stick == kStick ? g_stick_x : g_rstick_x, sy = stick == kStick ? g_stick_y : g_rstick_y;
    bool held = false;
    for (const Finger& f : g_fingers)
        if (f.role == role && !g_editing) {
            at = ImVec2(f.x0, f.y0);
            held = true;
        }
    const Art* ring = FindArt("stick_ring");
    const Art* knob_art = FindArt(stick == kStick ? "stick_knob" : "stick_shift");
    if (ring && knob_art) {
        // the left stick a ring with a knob; the right one, which shifts gears, the gear lever moved by the thumb
        ImVec2 knob(at.x + sx * r * 0.45f, at.y - sy * r * 0.45f);
        if (stick == kStick) {
            DrawArt(dl, *ring, at, r, ArtTint(false, 100));
            DrawArt(dl, *knob_art, knob, r * 0.42f, ArtTint(held, 100));
        } else {
            DrawArt(dl, *knob_art, knob, r, ArtTint(held, 100));
        }
    } else {
        DrawStickShapes(dl, th, at, r, sx, sy, held);
    }
    g_opacity = opacity;
    if (hidden) {
        float k = r * 0.35f;
        dl->AddLine(ImVec2(at.x - k, at.y - k), ImVec2(at.x + k, at.y + k), IM_COL32(255, 80, 60, 220), th);
        dl->AddLine(ImVec2(at.x - k, at.y + k), ImVec2(at.x + k, at.y - k), IM_COL32(255, 80, 60, 220), th);
    }
}

// a text centred on a point, its height a share of the screen's
void CentredText(ImDrawList* dl, ImVec2 at, float size, ImU32 colour, const char* text) {
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x - ts.x / 2, at.y - ts.y / 2), colour, text);
}

// a control in its shape: glass, its edge, its icon or label, a face button's caption under it. fade is the share
// (of 100) a hidden control shows at in the editor
// the steering wheel, turned as it steers: a rim, three spokes to a hub and a saffron mark at its top
void DrawWheel(ImDrawList* dl, ImVec2 c, float r, bool on, int fade, float th) {
    float turn = g_wheel_turn * kPi / 180.0f;
    auto at = [&](float deg, float k) {
        float a = deg * kPi / 180.0f + turn;
        return ImVec2(c.x + std::cos(a) * r * k, c.y + std::sin(a) * r * k);
    };
    float rim = r * 0.17f;
    dl->AddCircleFilled(c, r, Fade(kGlass, 70 * fade / 100), 64);
    dl->AddCircle(c, r - rim / 2, on ? Fade(kSaffron, 200) : Fade(kGlass, 210 * fade / 100), 64, rim);
    dl->AddCircle(c, r, Fade(kEdge, 120 * fade / 100), 64, th * 0.8f);
    dl->AddCircle(c, r - rim, Fade(kEdge, 70 * fade / 100), 64, th * 0.6f);
    for (float deg : {0.0f, 180.0f, 90.0f})  // right, left and down, the wheel level
        dl->AddLine(at(deg, 0.2f), at(deg, 1.0f - 0.17f), Fade(kEdge, 150 * fade / 100), r * 0.11f);
    dl->AddCircleFilled(c, r * 0.24f, Fade(kGlass, 230 * fade / 100), 32);
    dl->AddCircle(c, r * 0.24f, Fade(kEdge, 140 * fade / 100), 32, th * 0.7f);
    dl->AddCircleFilled(at(-90.0f, 1.0f - rim / (2 * r)), rim * 0.55f, Fade(kSaffron, 230 * fade / 100), 16);
}

// a face button's move under it, in a dark band to read over the game
void DrawCaption(ImDrawList* dl, size_t i, float cx, float cy, int fade, const char* caption) {
    float size = Radius(i) * 0.42f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, caption);
    ImVec2 at(cx, cy + Radius(i) + size * 0.75f);
    dl->AddRectFilled(ImVec2(at.x - ts.x / 2 - size * 0.35f, at.y - ts.y / 2 - size * 0.05f),
                      ImVec2(at.x + ts.x / 2 + size * 0.35f, at.y + ts.y / 2 + size * 0.05f), Fade(kGlass, 130 * fade / 100),
                      size * 0.3f);
    CentredText(dl, at, size, Fade(kCream, 215 * fade / 100), caption);
}

void DrawControl(ImDrawList* dl, size_t i, bool on, int fade, float th) {
    const Button& b = kButtons[i];
    // a menu's A, B, X or Y: as in a race, its prompt's label for its caption
    std::string menu_caption = FeIcon(i) ? FeLabel(i) : std::string();
    if (std::string label = FeLabel(i); !label.empty() && menu_caption.empty()) {  // a menu's pill: glass rimmed in the button's colour
        float cx, cy, hw, hh;
        Centre(i, cx, cy);
        Extent(i, hw, hh);
        ImVec2 p0(cx - hw, cy - hh), p1(cx + hw, cy + hh);
        dl->AddRectFilled(p0, p1, on ? Fade(kSaffron, 150) : Fade(kGlass, 175), hh);
        dl->AddRect(p0, p1, Fade(ButtonColour(b.press), 220), hh, 0, th);
        CentredText(dl, ImVec2(cx, cy), hh * kFeTextSize, on ? Fade(255, 255, 255, 255) : Fade(kCream, 235), label.c_str());
        return;
    }
    if (const Art* art = ArtOf(i)) {  // its look and its name are the art's: no caption but a menu's
        float cx, cy;
        Centre(i, cx, cy);
        float hh = Radius(i);
        if (b.press == kSteerWheel) DrawArt(dl, *art, ImVec2(cx, cy), hh, ArtTint(on, fade), g_wheel_turn * kPi / 180.0f);
        else DrawArt(dl, *art, ImVec2(cx, cy), on ? hh * 0.92f : hh, ArtTint(on, fade));
        if (!menu_caption.empty()) DrawCaption(dl, i, cx, cy, fade, menu_caption.c_str());
        return;
    }
    if (b.press == kSteerWheel) {
        float cx, cy;
        Centre(i, cx, cy);
        DrawWheel(dl, ImVec2(cx, cy), Radius(i), on, fade, th);
        return;
    }
    float cx, cy, hw, hh;
    Centre(i, cx, cy);
    Extent(i, hw, hh);
    if (on) hw *= 0.9f, hh *= 0.9f;  // pressed in
    ImVec2 c(cx, cy);
    Shape shape = ShapeOf(i);
    Rgb colour = ButtonColour(b.press);
    ImU32 fill = on ? Fade(kSaffron, 120) : Fade(kGlass, 155 * fade / 100);
    switch (shape) {
        case Shape::Round: {
            bool face = b.caption != nullptr;
            dl->AddCircleFilled(c, hh, on && face ? Fade(colour, 110) : fill, 48);
            dl->AddCircle(c, hh, face ? Fade(colour, 200 * fade / 100) : Fade(kEdge, 70 * fade / 100), 48,
                          th * (face ? 1.1f : 0.7f));
            if (face) dl->AddCircle(c, hh * 0.82f, Fade(kEdge, 28 * fade / 100), 48, th * 0.5f);
            break;
        }
        case Shape::Tab:
        case Shape::Pill: {
            float round = shape == Shape::Pill ? hh : hh * 0.45f;
            ImVec2 p0(cx - hw, cy - hh), p1(cx + hw, cy + hh);
            dl->AddRectFilled(p0, p1, fill, round);
            dl->AddRect(p0, p1, Fade(kEdge, 70 * fade / 100), round, 0, th * 0.7f);
            if (shape == Shape::Tab)  // the trigger's or the button's colour along its foot
                dl->AddRectFilled(ImVec2(cx - hw * 0.55f, cy + hh * 0.62f), ImVec2(cx + hw * 0.55f, cy + hh * 0.62f + th * 1.3f),
                                  Fade(colour, 210 * fade / 100), th);
            break;
        }
        case Shape::Square: {
            float round = hh * 0.3f;
            dl->AddRectFilled(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), fill, round);
            dl->AddRect(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), Fade(kEdge, 60 * fade / 100), round, 0,
                        th * 0.6f);
            break;
        }
        case Shape::Pedal: {  // a tall pedal rimmed green for the gas, red for the brake, its grip ridges, its name low
            Rgb pedal = b.press == kAnalogRT ? Rgb{95, 196, 110} : Rgb{224, 86, 76};
            float round = hw * 0.4f;
            ImVec2 p0(cx - hw, cy - hh), p1(cx + hw, cy + hh);
            dl->AddRectFilled(p0, p1, on ? Fade(pedal, 130) : fill, round);
            dl->AddRect(p0, p1, Fade(pedal, 210 * fade / 100), round, 0, th * 1.1f);
            for (int r = 0; r < 5; r++) {
                float y = cy - hh * 0.62f + float(r) * hh * 0.24f;
                dl->AddLine(ImVec2(cx - hw * 0.55f, y), ImVec2(cx + hw * 0.55f, y), Fade(kEdge, (on ? 150 : 70) * fade / 100), th);
            }
            CentredText(dl, ImVec2(cx, cy + hh * 0.72f), hw * 0.5f, on ? Fade(255, 255, 255, 255) : Fade(pedal, 235 * fade / 100),
                        b.press == kAnalogRT ? "GAS" : "BRAKE");
            return;
        }
    }
    float icon = shape == Shape::Square ? hh * 0.85f : hh;
    DrawIcon({dl, c, icon, on ? Fade(255, 255, 255, 240) : Fade(kCream, 200 * fade / 100), th}, b.icon);
    if (b.icon == Icon::Text) {
        ImU32 text = on ? Fade(255, 255, 255, 255) : shape == Shape::Round ? Fade(colour, 235 * fade / 100)
                                                                             : Fade(kCream, 225 * fade / 100);
        float size = shape == Shape::Round ? hh * 1.05f : shape == Shape::Pill ? hh * 0.78f : hh * 0.8f;
        CentredText(dl, ImVec2(cx, cy - (shape == Shape::Tab ? hh * 0.08f : 0)), size, text, b.label);
    }
    if (!menu_caption.empty()) DrawCaption(dl, i, cx, cy, fade, menu_caption.c_str());
    else if (b.caption) DrawCaption(dl, i, cx, cy, fade, b.caption);
}

// the editor's boxes for the HUD's items, under the controls
void DrawHudBoxes(ImDrawList* dl, float th) {
    for (size_t i = 0; i < game::kHudItems; i++) {
        float cx, cy, hw, hh;
        HudRect(i, cx, cy, hw, hh);
        bool picked = int(i) == g_hud_selected;
        ImU32 edge = picked ? IM_COL32(255, 200, 60, 255) : IM_COL32(240, 228, 205, 200);
        dl->AddRectFilled(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), IM_COL32(20, 18, 14, 150), hh * 0.15f);
        dl->AddRect(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), edge, hh * 0.15f, 0, th * (picked ? 1.5f : 1.0f));
        CentredText(dl, ImVec2(cx, cy), std::min(hh * 0.3f, Height() * 0.036f), IM_COL32(255, 255, 255, 230),
                    app::Ui(kHudLabels[i]).c_str());
    }
}

// the editor's mark around a control
void DrawOutline(ImDrawList* dl, size_t i, ImU32 colour, float th) {
    float cx, cy, hw, hh;
    Centre(i, cx, cy);
    Extent(i, hw, hh);
    float k = th * 2;
    if (ShapeOf(i) == Shape::Round) dl->AddCircle(ImVec2(cx, cy), hh + k, colour, 48, th * 1.5f);
    else dl->AddRect(ImVec2(cx - hw - k, cy - hh - k), ImVec2(cx + hw + k, cy + hh + k), colour, hh * 0.4f, 0, th * 1.5f);
}

// the editor's bar, drawn as the controls are: Reset, Cancel, Done, and what to do under it
void DrawEditorBar(ImDrawList* dl, float th) {
    float h = Height();
    for (int i = 0; i < kBarCount; i++) {
        float r[4];
        BarAt(0, 0, r, i);
        bool done = i == kBarDone;
        bool size = i == kBarSmaller || i == kBarBigger;
        // - and + wait for a control to be picked, Hide for one that can be hidden
        int alpha = (size && g_selected < 0) || (i == kBarHide && !Hideable(g_selected)) ? 90 : 240;
        const char* label = i == kBarHide && Hideable(g_selected) && g_layout[size_t(g_selected)].hidden ? "SHOW"
                                                                                                         : kBarLabels[i];
        dl->AddRectFilled(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]),
                          done ? IM_COL32(150, 115, 45, 235) : IM_COL32(20, 18, 14, 225), h * 0.015f);
        dl->AddRect(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]), IM_COL32(220, 180, 100, alpha), h * 0.015f, 0, th * 0.7f);
        CentredText(dl, ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2), h * (size ? 0.06f : 0.042f),
                    IM_COL32(255, 255, 255, alpha), app::Ui(label).c_str());
    }
    CentredText(dl, ImVec2(Width() / 2, (kBarY + kBarH + 0.05f) * h), h * 0.036f, IM_COL32(255, 255, 255, 220),
                app::Ui("DRAG TO MOVE, PINCH OR - + TO RESIZE").c_str());
}


}  // namespace

void Draw() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    bool editing = g_editing;
    float h = Height();
    float th = std::max(2.0f, h / 360.0f);
    if (!editing && !Shown()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (editing) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(Width(), h), IM_COL32(0, 0, 0, 90));
        DrawHudBoxes(dl, th);
    }
    for (size_t i = 0; i < kButtonCount; i++) {
        const Button& b = kButtons[i];
        if (!ButtonShown(i) || (app::MenuOpen() && b.press != kPanel && !editing)) continue;
        float cx, cy;
        Centre(i, cx, cy);
        float r = Radius(i);
        bool hidden = g_layout[i].hidden;
        bool on = !editing && (g_pressed[i] || (b.press == kSteerWheel && g_wheel_held));
        int fade = hidden ? 40 : 100;  // a hidden control shows faint in the editor
        DrawControl(dl, i, on, fade, th);
        if (editing && int(i) == g_selected) DrawOutline(dl, i, IM_COL32(255, 200, 60, 255), th);
        if (editing && hidden) {
            float k = r * 0.6f;
            dl->AddLine(ImVec2(cx - k, cy - k), ImVec2(cx + k, cy + k), IM_COL32(255, 80, 60, 220), th);
            dl->AddLine(ImVec2(cx - k, cy + k), ImVec2(cx + k, cy - k), IM_COL32(255, 80, 60, 220), th);
        }
    }
    if (editing || (!FeMenu() && !app::MenuOpen() && game::Racing()))
        for (size_t s : {kStick, kRStick}) DrawStick(dl, th, s);
    if (editing) {
        if (g_selected >= 0 && IsStick(size_t(g_selected))) {
            size_t s = size_t(g_selected);
            ImVec2 at(g_layout[s].x * Width(), g_layout[s].y * h);
            dl->AddCircle(at, Radius(s) + th * 2, IM_COL32(255, 200, 60, 255), 64, th * 1.5f);
        }
        DrawEditorBar(dl, th);
    }
}

// ---- the layout's settings, for the game's options (Recomp Settings, TOUCH); game thread, each saved at once

namespace {

uint32_t Percent(float f) { return uint32_t(std::lround(f * 100.0f)); }

}  // namespace

uint32_t ControlsOpacity() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_opacity);
}

// 20% to 100% in steps of 5
void StepOpacity(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_opacity = float(std::clamp(int(Percent(g_opacity)) + step * 5, 20, 100)) / 100.0f;
    SaveLayout();
}

uint32_t ControlsSize() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_scale);
}

// 50% to 150% in steps of 5
void StepSize(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_scale = float(std::clamp(int(Percent(g_scale)) + step * 5, 50, 150)) / 100.0f;
    SaveLayout();
}

const char* SteeringName() { return kSteeringNames[size_t(g_steering.load())]; }

// arrows, wheel, tilt
void StepSteering(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    int n = int(std::size(kSteeringIds));
    g_steering = Steering(((int(g_steering.load()) + (step < 0 ? n - 1 : 1)) % n));
    g_wheel_turn = 0;
    SaveLayout();
}

bool TiltSteering() { return g_steering == Steering::Tilt; }

void LoadArt() {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    SDL_Surface* loaded[std::size(g_art)] = {};
    for (size_t i = 0; i < std::size(g_art); i++) {
        std::string file = std::string("touch/") + g_art[i].name + ".png";  // the APK's assets
        SDL_Surface* s = SDL_LoadPNG(file.c_str());
        if (s) {
            loaded[i] = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(s);
        }
        if (!loaded[i]) {
            LOGW("touch: no art %s: %s", file.c_str(), SDL_GetError());
            continue;
        }
        g_art[i].rect = atlas->AddCustomRectRegular(loaded[i]->w, loaded[i]->h);
        g_art[i].aspect = float(loaded[i]->h) / float(loaded[i]->w);
    }
    // built now, with the art: the backend uploads the atlas as it is. Its width is otherwise the glyphs' alone, too
    // narrow for the brake pedal
    atlas->TexDesiredWidth = 2048;
    unsigned char* pixels;
    int width, height;
    atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (size_t i = 0; i < std::size(g_art); i++) {
        SDL_Surface* s = loaded[i];
        if (!s) continue;
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(g_art[i].rect);
        for (int y = 0; y < s->h; y++)
            std::memcpy(pixels + (size_t(r->Y + y) * width + r->X) * 4, static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch,
                        size_t(s->w) * 4);
        atlas->CalcCustomRectUV(r, &g_art[i].uv0, &g_art[i].uv1);
        SDL_DestroySurface(s);
    }
}

const char* StickModeName() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return g_fixed_sticks ? "FIXED" : "FLOATING";
}

void StepStickMode(int) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_fixed_sticks = !g_fixed_sticks;
    SaveLayout();
}

// the controls over the screen to drag where they go, Done or Cancel in the bar at the top, Android's back cancels
void EditLayout() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    // the touch that picked the row is the menu's, not the editor's
    g_fingers.clear();
    Recount();
    OpenEditor();
}

void PadUsed() { input::g_touch = false; }

// main thread, each turn of the event loop
void Pump() { gyro::Pump(); }

}  // namespace touch

namespace input {

bool TouchActive() { return g_touch; }

}  // namespace input
