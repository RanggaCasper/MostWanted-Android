#include <atomic>
// controllers through SDL on Android: as the SDK's runtime/core/input.cpp reads them, but the plug events come from
// the main thread's event loop (Android's input reaches SDL there), and port 0 also takes the touch controls. The
// CONTROLLER page's bindings (common/pad_binds.h) go over SDL's mapping.
#include "core/input.h"

#include <jni.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
SDL_Gamepad* g_pads[4] = {};

// ---- the bindings: an action of the Xbox pad, what presses it. Codes as the keyboard's (native/touch.cpp): 0-7
// analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the left stick up down left right, 20-23 the right's
constexpr int kFirstBit = 8;
constexpr int kFirstStick = 16;
// what each code is on a controller as SDL maps it; L1 / R1 the game's LT / RT, L2 / R2 its White / Black
constexpr const char* kCodeNames[] = {
    "A", "B", "X", "Y", "R2", "L2", "L1", "R1", "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "START", "SELECT",
    "L3", "R3", "LEFT STICK UP", "LEFT STICK DOWN", "LEFT STICK LEFT", "LEFT STICK RIGHT", "RIGHT STICK UP",
    "RIGHT STICK DOWN", "RIGHT STICK LEFT", "RIGHT STICK RIGHT"};
struct PadTarget {
    Action action;
    int code;
};
constexpr PadTarget kPadActions[] = {
    {{"button_a", "A", "A"}, 0},
    {{"button_b", "B", "B"}, 1},
    {{"button_x", "X", "X"}, 2},
    {{"button_y", "Y", "Y"}, 3},
    {{"button_black", "BLACK", "R2"}, 4},
    {{"button_white", "WHITE", "L2"}, 5},
    {{"trigger_left", "LEFT TRIGGER", "L1"}, 6},
    {{"trigger_right", "RIGHT TRIGGER", "R1"}, 7},
    {{"dpad_up", "DPAD UP", "DPAD UP"}, 8},
    {{"dpad_down", "DPAD DOWN", "DPAD DOWN"}, 9},
    {{"dpad_left", "DPAD LEFT", "DPAD LEFT"}, 10},
    {{"dpad_right", "DPAD RIGHT", "DPAD RIGHT"}, 11},
    {{"start", "START", "START"}, 12},
    {{"back", "BACK", "SELECT"}, 13},
    {{"thumb_left", "LEFT THUMB", "L3"}, 14},
    {{"thumb_right", "RIGHT THUMB", "R3"}, 15},
    {{"move_up", "LEFT STICK UP", "LEFT STICK UP"}, 16},
    {{"move_down", "LEFT STICK DOWN", "LEFT STICK DOWN"}, 17},
    {{"move_left", "LEFT STICK LEFT", "LEFT STICK LEFT"}, 18},
    {{"move_right", "LEFT STICK RIGHT", "LEFT STICK RIGHT"}, 19},
    {{"look_up", "RIGHT STICK UP", "RIGHT STICK UP"}, 20},
    {{"look_down", "RIGHT STICK DOWN", "RIGHT STICK DOWN"}, 21},
    {{"look_left", "RIGHT STICK LEFT", "RIGHT STICK LEFT"}, 22},
    {{"look_right", "RIGHT STICK RIGHT", "RIGHT STICK RIGHT"}, 23},
};
constexpr size_t kPadCount = std::size(kPadActions);

// an input of a controller: SDL's joystick button, hat direction or axis side, or Android's key code of a controller
// SDL does not have (the loose one below); kind 0 for none. Saved as "button 3", "hat 0 up", "axis 2+", "key 188"
struct Source {
    char kind = 0;  // 'b' 'h' 'a' 'k'
    int index = 0;
    int dir = 0;  // a hat's SDL_HAT_ bit, an axis's side +1 or -1
};
using PadBinds = std::array<Source, kPadCount>;
std::mutex g_pad_bind_lock;
PadBinds g_pad_binds;
std::atomic<bool> g_pad_menu_mode{true};

struct HatDir {
    int bit;
    const char* name;
};
constexpr HatDir kHatDirs[] = {{SDL_HAT_UP, "up"}, {SDL_HAT_DOWN, "down"}, {SDL_HAT_LEFT, "left"}, {SDL_HAT_RIGHT, "right"}};

Source ParseSource(const std::string& text) {
    if (text == "trigger left") return Source{'t', 5};
    if (text == "trigger right") return Source{'t', 4};
    Source s;
    if (text == "unbound") return Source{'u'};
    char word[8] = "", sign = 0;
    if (std::sscanf(text.c_str(), "button %d", &s.index) == 1) s.kind = 'b';
    else if (std::sscanf(text.c_str(), "key %d", &s.index) == 1) s.kind = 'k';
    else if (std::sscanf(text.c_str(), "axis %d%c", &s.index, &sign) == 2 && (sign == '+' || sign == '-'))
        s.kind = 'a', s.dir = sign == '+' ? 1 : -1;
    else if (std::sscanf(text.c_str(), "hat %d %7s", &s.index, word) == 2)
        for (const HatDir& d : kHatDirs)
            if (std::string(word) == d.name) s.kind = 'h', s.dir = d.bit;
    if (s.index < 0) s.kind = 0;
    return s;
}

std::string SourceText(const Source& s) {
    if (s.kind == 't')
        return s.index == 5 ? "trigger left" : "trigger right";
    if (s.kind == 'u') return "unbound";
    char text[32] = "";
    if (s.kind == 'b') std::snprintf(text, sizeof(text), "button %d", s.index);
    else if (s.kind == 'k') std::snprintf(text, sizeof(text), "key %d", s.index);
    else if (s.kind == 'a') std::snprintf(text, sizeof(text), "axis %d%c", s.index, s.dir > 0 ? '+' : '-');
    else if (s.kind == 'h')
        for (const HatDir& d : kHatDirs)
            if (d.bit == s.dir) std::snprintf(text, sizeof(text), "hat %d %s", s.index, d.name);
    return text;
}

PadBinds PadBindsNow() {
    std::lock_guard lock(g_pad_bind_lock);
    return g_pad_binds;
}

bool Matches(const SDL_GamepadBinding& b, const Source& s) {
    switch (s.kind) {
        case 'b': return b.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON && b.input.button == s.index;
        case 'h':
            return b.input_type == SDL_GAMEPAD_BINDTYPE_HAT && b.input.hat.hat == s.index && b.input.hat.hat_mask == s.dir;
        case 'a': {
            if (b.input_type != SDL_GAMEPAD_BINDTYPE_AXIS || b.input.axis.axis != s.index) return false;
            int lo = std::min(b.input.axis.axis_min, b.input.axis.axis_max);
            int hi = std::max(b.input.axis.axis_min, b.input.axis.axis_max);
            return s.dir > 0 ? hi > 0 : lo < 0;
        }
        default: return false;
    }
}

// the code the mapping gives an input it maps, the input fully pressed; -1 for none of the Xbox pad's
int OutputCode(const SDL_GamepadBinding& b, const Source& s) {
    if (b.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
        switch (b.output.button) {
            case SDL_GAMEPAD_BUTTON_SOUTH: return 0;
            case SDL_GAMEPAD_BUTTON_EAST: return 1;
            case SDL_GAMEPAD_BUTTON_WEST: return 2;
            case SDL_GAMEPAD_BUTTON_NORTH: return 3;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 6;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 7;
            case SDL_GAMEPAD_BUTTON_DPAD_UP: return 8;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 9;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 10;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 11;
            case SDL_GAMEPAD_BUTTON_START: return 12;
            case SDL_GAMEPAD_BUTTON_BACK: return 13;
            case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 14;
            case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 15;
            default: return -1;
        }
    }
    if (b.output_type != SDL_GAMEPAD_BINDTYPE_AXIS) return -1;
    // the output's side: a button or hat gives the range's far end, an axis its side's end mapped across
    float t = 1;
    if (b.input_type == SDL_GAMEPAD_BINDTYPE_AXIS && b.input.axis.axis_max != b.input.axis.axis_min) {
        float v = s.dir > 0 ? 32767.0f : -32768.0f;
        t = std::clamp((v - float(b.input.axis.axis_min)) / float(b.input.axis.axis_max - b.input.axis.axis_min), 0.0f, 1.0f);
    }
    float out = float(b.output.axis.axis_min) + t * float(b.output.axis.axis_max - b.output.axis.axis_min);
    bool plus = out >= 0;
    switch (b.output.axis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: return plus ? 19 : 18;
        case SDL_GAMEPAD_AXIS_LEFTY: return plus ? 17 : 16;  // SDL's y is down
        case SDL_GAMEPAD_AXIS_RIGHTX: return plus ? 23 : 22;
        case SDL_GAMEPAD_AXIS_RIGHTY: return plus ? 21 : 20;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return 5;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return 4;
        default: return -1;
    }
}

// what the mapping read for a code let go: a stick's whole axis
void Clear(Pad& p, int code) {
    if (code < 0 || code >= 24) return;
    if (code < kFirstBit) {
        p.analog[code] = 0;
    } else if (code < kFirstStick) {
        p.buttons &= uint16_t(~(1u << (code - kFirstBit)));
    } else {
        int16_t* axis = code < 18 ? &p.ly :
                        code < 20 ? &p.lx :
                        code < 22 ? &p.ry : &p.rx;
        const bool positive =
            code == 16 || code == 19 || code == 20 || code == 23;
        if ((positive && *axis > 0) || (!positive && *axis < 0))
            *axis = 0;
    }
}

// an input's press, 0 to 1
float Value(SDL_Joystick* joy, const Source& s) {
    switch (s.kind) {
        case 'b': return SDL_GetJoystickButton(joy, s.index) ? 1.0f : 0.0f;
        case 'h': return SDL_GetJoystickHat(joy, s.index) & s.dir ? 1.0f : 0.0f;
        case 'a': {
            float v = SDL_GetJoystickAxis(joy, s.index);
            // a trigger resting at one end runs the whole range
            Sint16 rest = 0;
            SDL_GetJoystickAxisInitialState(joy, s.index, &rest);
            if (s.dir > 0) return rest < -16000 ? (v + 32768.0f) / 65535.0f : std::max(v, 0.0f) / 32767.0f;
            return rest > 16000 ? (32767.0f - v) / 65535.0f : std::max(-v, 0.0f) / 32768.0f;
        }
        default: return 0;
    }
}

// a bound input's press onto its action; the sticks' directions gathered in dirs
void Press(Pad& p, float (&dirs)[8], int code, float v) {
    if (v <= 0) return;
    if (code < kFirstBit) p.analog[code] = std::max(p.analog[code], uint8_t(std::min(v, 1.0f) * 255.0f));
    else if (code < kFirstStick) p.buttons |= uint16_t((v >= 0.5f) << (code - kFirstBit));
    else dirs[code - kFirstStick] = std::max(dirs[code - kFirstStick], std::min(v, 1.0f));
}

void PressSticks(Pad& p, const float (&dirs)[8]) {
    auto add = [](int16_t& axis, float v) {
        if (v) axis = int16_t(std::clamp(float(axis) + v * 32767.0f, -32768.0f, 32767.0f));
    };
    add(p.ly, dirs[0] - dirs[1]);
    add(p.lx, dirs[3] - dirs[2]);
    add(p.ry, dirs[4] - dirs[5]);
    add(p.rx, dirs[7] - dirs[6]);
}

// a bound input presses its action alone, no longer what the mapping gave it
void ApplyBinds(SDL_Gamepad* pad, Pad& p,
                const PadBinds& binds, bool loose);

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

void Read(SDL_Gamepad* pad, Pad& p) {
    p.connected = true;
    constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
    // A B X Y
    constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                             SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    // the shoulders the game's LT / RT, the triggers its White / Black; the triggers' 0 to 32767 into 0 to 255
    p.analog[4] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);  // black
    p.analog[5] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);   // white
    p.analog[6] = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 0xFF : 0;   // LT
    p.analog[7] = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 0xFF : 0;  // RT
    // SDL's sticks have y down; not plain negation: -32768 has no positive
    p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
}

// A controller Android hands over without its device: some phones' game services (MIUI's on a Redmi Note 8 Pro)
// take a DualShock 4 and pass its buttons and sticks on as a virtual device, which SDL takes for no controller.
// MostWantedActivity sends those here as Android's key codes and axes; they play port 0.
constexpr int kLooseCodes = 512;  // Android's key codes held, the controller's all under 320
struct Loose {
    std::bitset<kLooseCodes> keys;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, hx = 0, hy = 0;
    bool seen = false;
};
std::mutex g_loose_lock;
Loose g_loose;

// Android's key code as the pad's: a button bit (dpad, start, back, thumbs) or an analog button, -1 for neither
void LooseKey(int code, int& bit, int& analog) {
    bit = analog = -1;
    switch (code) {
        case 19: bit = 0; break;    // DPAD_UP
        case 20: bit = 1; break;    // DPAD_DOWN
        case 21: bit = 2; break;    // DPAD_LEFT
        case 22: bit = 3; break;    // DPAD_RIGHT
        case 108: bit = 4; break;   // BUTTON_START (Options)
        case 109: bit = 5; break;   // BUTTON_SELECT (Share)
        case 106: bit = 6; break;   // BUTTON_THUMBL
        case 107: bit = 7; break;   // BUTTON_THUMBR
        case 96: analog = 0; break;   // BUTTON_A (cross)
        case 97: analog = 1; break;   // BUTTON_B (circle)
        case 99: analog = 2; break;   // BUTTON_X (square)
        case 100: analog = 3; break;  // BUTTON_Y (triangle)
        case 102: analog = 6; break;  // BUTTON_L1: LT
        case 103: analog = 7; break;  // BUTTON_R1: RT
        case 104: analog = 5; break;  // BUTTON_L2: white
        case 105: analog = 4; break;  // BUTTON_R2: black
        default: break;
    }
}

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
uint8_t Trigger(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// the loose controller's state on top of what SDL read: buttons added, the stick pushed further taken; a key bound
// on the CONTROLLER page presses its action alone
void MergeLoose(Pad& p, const PadBinds& binds) {
    std::lock_guard lock(g_loose_lock);
    const Loose& l = g_loose;
    if (!l.seen) return;
    p.connected = true;
    float dirs[8] = {};
    for (int code = 0; code < kLooseCodes; code++) {
        if (!l.keys[code]) continue;
        bool bound = false;
        for (size_t a = 0; a < kPadCount; a++) {
            if (binds[a].kind != 'k' || binds[a].index != code) continue;
            bound = true;
            Press(p, dirs, kPadActions[a].code, 1);
        }
        int bit, analog;
        LooseKey(code, bit, analog);
        if (bound) continue;
        if (bit >= 0) p.buttons |= uint16_t(1u << bit);
        if (analog >= 0) p.analog[analog] = 0xFF;
    }
    PressSticks(p, dirs);
    // the hat as the dpad: x left -1 right 1, y up -1 down 1
    if (l.hy < -0.5f) p.buttons |= 1 << 0;
    if (l.hy > 0.5f) p.buttons |= 1 << 1;
    if (l.hx < -0.5f) p.buttons |= 1 << 2;
    if (l.hx > 0.5f) p.buttons |= 1 << 3;
    // the triggers' axes are L2 / R2: white and black
    p.analog[5] = std::max(p.analog[5], Trigger(l.lt));
    p.analog[4] = std::max(p.analog[4], Trigger(l.rt));
    // Android's y is down, the pad's up
    auto further = [](int16_t& to, int16_t v) { if (std::abs(int(v)) > std::abs(int(to))) to = v; };
    further(p.lx, Axis(l.lx));
    further(p.ly, Axis(-l.ly));
    further(p.rx, Axis(l.rx));
    further(p.ry, Axis(-l.ry));
}

int LooseCode(const Source& source) {
    if (source.kind != 'k') return -1;
    int bit, analog;
    LooseKey(source.index, bit, analog);
    return analog >= 0 ? analog :
           bit >= 0 ? bit + kFirstBit : -1;
}

// Caller holds g_lock. Recognize trigger aliases only from known
// Android key meanings or SDL's mapping, never raw button numbers.
int LogicalTriggerCode(const Source& source) {
    if (source.kind == 't')
        return source.index == 4 || source.index == 5
            ? source.index : -1;

    if (source.kind == 'k') {
        if (source.index == 104) return 5;
        if (source.index == 105) return 4;
        return -1;
    }

    if (source.kind != 'b' && source.kind != 'h' &&
        source.kind != 'a')
        return -1;

    for (SDL_Gamepad* pad : g_pads) {
        if (!pad) continue;
        int count = 0;
        SDL_GamepadBinding** list =
            SDL_GetGamepadBindings(pad, &count);
        int result = -1;

        for (int i = 0; list && i < count; ++i) {
            if (!Matches(*list[i], source)) continue;
            const int code = OutputCode(*list[i], source);
            if (code == 4 || code == 5) {
                result = code;
                break;
            }
        }

        SDL_free(list);
        if (result >= 0) return result;
    }
    return -1;
}

// Caller holds g_lock and g_loose_lock.
// Only port 0 receives the existing Android loose-controller fallback.
float LogicalTriggerValue(SDL_Gamepad* pad, int code, bool loose) {
    const bool left = code == 5;
    float value = 0.0f;

    if (pad) {
        const auto axis = left ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                               : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        value = std::max(
            0.0f, float(SDL_GetGamepadAxis(pad, axis)) / 32767.0f
        );
    }

    if (loose && g_loose.seen) {
        value = std::max(value, left ? g_loose.lt : g_loose.rt);
        if (g_loose.keys[size_t(left ? 104 : 105)])
            value = 1.0f;
    }

    return std::clamp(value, 0.0f, 1.0f);
}

// Called while g_lock is held, after default SDL/loose inputs are merged.
void ApplyBinds(SDL_Gamepad* pad, Pad& p,
                const PadBinds& binds, bool loose) {
    int count = 0;
    SDL_GamepadBinding** list =
        pad ? SDL_GetGamepadBindings(pad, &count) : nullptr;

    // An override removes the destination's old default input.
    for (size_t a = 0; a < kPadCount; ++a)
        if (binds[a].kind) Clear(p, kPadActions[a].code);

    // A claimed physical input stops pressing its original action.
    for (const Source& source : binds) {
        if (!source.kind || source.kind == 'u') continue;
        if (source.kind == 't') {
            Clear(p, source.index);
        } else if (source.kind == 'k') {
            if (loose) Clear(p, LooseCode(source));
        } else {
            for (int i = 0; i < count; ++i)
                if (Matches(*list[i], source))
                    Clear(p, OutputCode(*list[i], source));
        }
    }
    SDL_free(list);

    SDL_Joystick* joy = pad ? SDL_GetGamepadJoystick(pad) : nullptr;
    float directions[8] = {};
    std::lock_guard loose_lock(g_loose_lock);
    for (size_t a = 0; a < kPadCount; ++a) {
        const Source& source = binds[a];
        float value = 0;
        if (source.kind == 't') {
            value = LogicalTriggerValue(pad, source.index, loose);
        } else if (source.kind == 'k') {
            if (loose && source.index >= 0 &&
                source.index < kLooseCodes)
                value = g_loose.keys[size_t(source.index)] ? 1.0f : 0.0f;
        } else if (joy && (source.kind == 'b' ||
                          source.kind == 'h' || source.kind == 'a')) {
            value = Value(joy, source);
        }
        Press(p, directions, kPadActions[a].code, value);
    }
    PressSticks(p, directions);
}

}  // namespace

// MostWantedActivity: the loose controller's buttons, sticks and triggers, and its letting go when the app loses the focus
extern "C" JNIEXPORT void JNICALL Java_com_racas_mostwanted_MostWantedActivity_nativeLooseKey(JNIEnv*, jclass, jint code,
                                                                                     jboolean down) {
    // every button kept, a generic controller's BUTTON_1 to 16 too: the CONTROLLER page can bind them
    if (code < 0 || code >= kLooseCodes) return;
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    g_loose.keys[size_t(code)] = down;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_mostwanted_MostWantedActivity_nativeLooseAxes(JNIEnv*, jclass, jfloat lx,
                                                                                      jfloat ly, jfloat rx, jfloat ry,
                                                                                      jfloat lt, jfloat rt, jfloat hx,
                                                                                      jfloat hy) {
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    g_loose.lx = lx, g_loose.ly = ly, g_loose.rx = rx, g_loose.ry = ry;
    g_loose.lt = lt, g_loose.rt = rt, g_loose.hx = hx, g_loose.hy = hy;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_mostwanted_MostWantedActivity_nativeLooseRelease(JNIEnv*, jclass) {
    std::lock_guard lock(g_loose_lock);
    bool seen = g_loose.seen;
    g_loose = {};
    g_loose.seen = seen;
}

void SetPadMenuMode(bool menu) {
    g_pad_menu_mode.store(menu, std::memory_order_relaxed);
}

void Init() {
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

void OnPadEvent(const SDL_Event& e) {
    std::lock_guard lock(g_lock);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
}

Pad Poll(int port) {
    Pad p;
    const bool menu = g_pad_menu_mode.load(std::memory_order_relaxed);
    const PadBinds binds = menu ? PadBinds{} : PadBindsNow();
    {
        std::lock_guard lock(g_lock);
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            Read(pad, p);
        }
    }
    // a controller pushed past a rest, not just plugged in
    constexpr int16_t kStickPushed = 16000;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    if (port == 0) MergeLoose(p, PadBinds{});
    if (!menu) {
        std::lock_guard lock(g_lock);
        SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr;
        ApplyBinds(pad, p, binds, port == 0);
    }
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), [](uint8_t a) { return a > 0x20; });
    if (p.buttons || analog || pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry)) touch::PadUsed();
    touch::Apply(port, p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

namespace {

// Android's name of a controller's key code
std::string KeyName(int code) {
    switch (code) {
        case 19: return "DPAD UP";
        case 20: return "DPAD DOWN";
        case 21: return "DPAD LEFT";
        case 22: return "DPAD RIGHT";
        case 96: return "A";
        case 97: return "B";
        case 98: return "C";
        case 99: return "X";
        case 100: return "Y";
        case 101: return "Z";
        case 102: return "L1";
        case 103: return "R1";
        case 104: return "L2";
        case 105: return "R2";
        case 106: return "L3";
        case 107: return "R3";
        case 108: return "START";
        case 109: return "SELECT";
        case 110: return "MODE";
        default: break;
    }
    if (code >= 188 && code <= 203) return "BUTTON " + std::to_string(code - 187);  // BUTTON_1 to 16
    return "KEY " + std::to_string(code);
}

// an input's name: what the first controller's mapping makes it, else its number; under g_lock
std::string SourceName(const Source& s) {
    if (s.kind == 't') return s.index == 5 ? "L2" : "R2";
    if (s.kind == 'u') return "UNBOUND";
    if (s.kind == 'k') return KeyName(s.index);
    for (SDL_Gamepad* pad : g_pads) {
        if (!pad) continue;
        int n = 0, code = -1;
        SDL_GamepadBinding** list = SDL_GetGamepadBindings(pad, &n);
        for (int i = 0; i < n && code < 0; i++)
            if (Matches(*list[i], s)) code = OutputCode(*list[i], s);
        SDL_free(list);
        if (code >= 0) return kCodeNames[code];
        break;
    }
    char text[32] = "";
    if (s.kind == 'b') std::snprintf(text, sizeof(text), "BUTTON %d", s.index + 1);
    else if (s.kind == 'a') std::snprintf(text, sizeof(text), "AXIS %d%c", s.index + 1, s.dir > 0 ? '+' : '-');
    else if (s.kind == 'h') {
        std::string dir = SourceText(s).substr(SourceText(s).rfind(' ') + 1);
        for (char& c : dir) c = char(std::toupper(uint8_t(c)));
        if (s.index) std::snprintf(text, sizeof(text), "HAT %d %s", s.index + 1, dir.c_str());
        else std::snprintf(text, sizeof(text), "HAT %s", dir.c_str());
    }
    return text;
}

}  // namespace

const Action* PadActions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kPadCount> a{};
        for (size_t i = 0; i < kPadCount; i++) a[i] = kPadActions[i].action;
        return a;
    }();
    *count = kPadCount;
    return list.data();
}

std::string PadBinding(size_t action) {
    if (action >= kPadCount) return {};
    Source s = PadBindsNow()[action];
    if (!s.kind) return {};
    std::lock_guard lock(g_lock);
    return SourceName(s);
}

void PadBind(size_t action, const std::string& source) {
    if (action >= kPadCount) return;
    Source wanted = ParseSource(source);
    if (!source.empty() && !wanted.kind) return;

    {
        // Same lock order as ApplyBinds: controller state before binds.
        std::lock_guard input_lock(g_lock);

        const int trigger = LogicalTriggerCode(wanted);
        if (trigger >= 0)
            wanted = Source{'t', trigger};

        std::array<bool, kPadCount> displaced{};
        const int default_code =
            wanted.kind == 't' ? wanted.index : LooseCode(wanted);

        for (size_t a = 0; a < kPadCount; ++a)
            displaced[a] = kPadActions[a].code == default_code;

        if (wanted.kind == 'b' || wanted.kind == 'h' ||
            wanted.kind == 'a') {
            for (SDL_Gamepad* pad : g_pads) {
                if (!pad) continue;
                int count = 0;
                SDL_GamepadBinding** list =
                    SDL_GetGamepadBindings(pad, &count);

                for (int i = 0; list && i < count; ++i) {
                    if (!Matches(*list[i], wanted)) continue;
                    const int code = OutputCode(*list[i], wanted);
                    for (size_t a = 0; a < kPadCount; ++a)
                        if (kPadActions[a].code == code)
                            displaced[a] = true;
                }
                SDL_free(list);
            }
        }

        std::lock_guard bind_lock(g_pad_bind_lock);
        if (wanted.kind && wanted.kind != 'u') {
            for (size_t a = 0; a < kPadCount; ++a) {
                if (a == action) continue;
                const Source& previous = g_pad_binds[a];

                const bool same_raw =
                    previous.kind == wanted.kind &&
                    previous.index == wanted.index &&
                    previous.dir == wanted.dir;

                const bool same_trigger =
                    wanted.kind == 't' &&
                    LogicalTriggerCode(previous) == wanted.index;

                if (same_raw || same_trigger ||
                    (!previous.kind && displaced[a]))
                    g_pad_binds[a] = Source{'u'};
            }
        }

        g_pad_binds[action] = wanted;

        auto& saved = game::Opt().pad;
        for (size_t a = 0; a < kPadCount; ++a) {
            const char* id = kPadActions[a].action.id;
            if (g_pad_binds[a].kind)
                saved[id] = SourceText(g_pad_binds[a]);
            else
                saved.erase(id);
        }
    }
    app::Save();
}

void ResetPadBindings() {
    game::Opt().pad.clear();
    LoadPadBindings();
    app::Save();
}

void LoadPadBindings() {
    std::lock_guard lock(g_pad_bind_lock);
    const auto& saved = game::Opt().pad;
    for (size_t i = 0; i < kPadCount; i++) {
        auto it = saved.find(kPadActions[i].action.id);
        g_pad_binds[i] = it != saved.end() ? ParseSource(it->second) : Source{};
    }
}

std::string PressedPadInput() {
    std::lock_guard input_lock(g_lock);
    std::lock_guard loose_lock(g_loose_lock);

    // Prefer mapped logical triggers over their digital/raw duplicates.
    // A small threshold catches partial pulls before a full raw-axis
    // movement would have passed the old 20000-unit threshold.
    for (SDL_Gamepad* pad : g_pads) {
        if (!pad) continue;
        if (LogicalTriggerValue(pad, 5, false) > 0.12f)
            return SourceText({'t', 5});
        if (LogicalTriggerValue(pad, 4, false) > 0.12f)
            return SourceText({'t', 4});
    }

    if (LogicalTriggerValue(nullptr, 5, true) > 0.12f)
        return SourceText({'t', 5});
    if (LogicalTriggerValue(nullptr, 4, true) > 0.12f)
        return SourceText({'t', 4});

    for (SDL_Gamepad* pad : g_pads) {
        SDL_Joystick* joy =
            pad ? SDL_GetGamepadJoystick(pad) : nullptr;
        if (!joy) continue;

        for (int i = 0; i < SDL_GetNumJoystickButtons(joy); ++i)
            if (SDL_GetJoystickButton(joy, i))
                return SourceText({'b', i});

        for (int i = 0; i < SDL_GetNumJoystickHats(joy); ++i)
            for (const HatDir& d : kHatDirs)
                if (SDL_GetJoystickHat(joy, i) & d.bit)
                    return SourceText({'h', i, d.bit});

        for (int i = 0; i < SDL_GetNumJoystickAxes(joy); ++i) {
            Sint16 rest = 0;
            SDL_GetJoystickAxisInitialState(joy, i, &rest);
            // SDL takes an axis's first value for its rest, and on Android that is often taken mid-pull (the
            // gas trigger in a race): far from zero too, or a released axis stays pressed for good
            const int value = SDL_GetJoystickAxis(joy, i);
            const int moved = value - int(rest);
            if (std::abs(moved) > 20000 && std::abs(value) > 20000)
                return SourceText({'a', i, moved > 0 ? 1 : -1});
        }
    }

    for (int code = 0; code < kLooseCodes; ++code)
        if (g_loose.keys[size_t(code)])
            return SourceText({'k', code});

    return {};
}

}  // namespace input
