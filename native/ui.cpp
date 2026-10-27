// ImGui's input on Android: SDL's touches, mouse and keys into ImGui's IO (main thread, under app::UiLock), and the
// first controller for the settings panel's navigation, read per frame on the render thread
#include <SDL3/SDL.h>

#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <imgui.h>

#include "common/game_font.h"
#include "common/menu.h"
#include "host.h"

namespace ui {

namespace {

ImGuiKey KeyOf(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

int MouseButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_RIGHT: return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default: return -1;
    }
}

SDL_FingerID g_finger = 0;  // the touch that is ImGui's mouse
bool g_fingered = false;

}  // namespace

void OnEvent(const SDL_Event& e) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    int w, h;
    host::WindowPixels(w, h);
    switch (e.type) {
        case SDL_EVENT_FINGER_DOWN:
            if (g_fingered) break;
            g_finger = e.tfinger.fingerID;
            g_fingered = true;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            io.AddMouseButtonEvent(0, true);
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(e.tfinger.x * float(w), e.tfinger.y * float(h));
            break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!g_fingered || e.tfinger.fingerID != g_finger) break;
            g_fingered = false;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMouseButtonEvent(0, false);
            // a lifted finger hovers nothing
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) break;
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.motion.x, e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) break;
            if (int b = MouseButton(e.button.button); b >= 0) {
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMouseButtonEvent(b, e.button.down);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
            if (ImGuiKey k = KeyOf(e.key.scancode); k != ImGuiKey_None) io.AddKeyEvent(k, e.key.down);
            break;
        case SDL_EVENT_TEXT_INPUT:
            io.AddInputCharactersUTF8(e.text.text);
            break;
        default:
            break;
    }
}

void NewFrame(float width, float height) {
    ImGuiIO& io = ImGui::GetIO();
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    auto now = clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60;
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    // the first controller steps through the panel
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* pad = count ? SDL_GetGamepadFromID(ids[0]) : nullptr;
    SDL_free(ids);
    if (!pad) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    constexpr Map kMap[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},       {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},    {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},     {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},    {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},  {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    };
    // a controller binding (the CONTROLLER page) takes the presses, and those held as it ended wait for release
    static bool held_off = false;
    bool any = false;
    for (const Map& m : kMap) any |= SDL_GetGamepadButton(pad, m.button);
    for (SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY})
        any |= std::abs(SDL_GetGamepadAxis(pad, a)) > 9830;
    if (app::Capturing()) held_off = true;
    else if (!any) held_off = false;
    for (const Map& m : kMap) io.AddKeyEvent(m.key, !held_off && SDL_GetGamepadButton(pad, m.button));
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, SDL_GamepadAxis a) {
        float v = held_off ? 0 : SDL_GetGamepadAxis(pad, a) / 32767.0f;
        io.AddKeyAnalogEvent(neg, v < -0.3f, v < -0.3f ? -v : 0);
        io.AddKeyAnalogEvent(pos, v > 0.3f, v > 0.3f ? v : 0);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
}

// The game's body font in place of ImGui's (src/game/game_font.cpp): the atlas made again with its glyphs over
// ProggyClean at the game's line height (which stays for what the game's font lacks), the touch controls' art laid in
// again. True when the atlas changed, for the renderer to upload it again
bool ApplyGameFont() {
    game::FontImage font;
    if (!game::TakeFont(font) || font.glyphs.empty() || font.line_height <= 0) return false;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->Clear();
    ImFontConfig config;
    config.SizePixels = float(font.line_height);
    ImFont* base = atlas->AddFontDefault(&config);
    std::vector<int> rects;
    rects.reserve(font.glyphs.size());
    for (const game::FontGlyph& g : font.glyphs)
        rects.push_back(atlas->AddCustomRectFontGlyph(base, ImWchar(g.code), g.w, g.h, float(g.advance),
                                                      ImVec2(float(g.x_off), float(g.y_off))));
    touch::LoadArt();  // its art's rects, and the atlas built
    unsigned char* pixels;
    int width, height;
    atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (size_t i = 0; i < font.glyphs.size(); i++) {
        const game::FontGlyph& g = font.glyphs[i];
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(rects[i]);
        if (!r) continue;
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                unsigned char* p = pixels + (size_t(r->Y + y) * width + r->X + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = g.alpha[size_t(y) * g.w + x];
            }
    }
    return true;
}

}  // namespace ui
