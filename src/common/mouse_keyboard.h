#pragma once
#include <cstddef>
#include <string>

#include "core/input.h"

// the touch controls, and a keyboard and mouse plugged into the phone, as the controller on port 0
// (native/touch.cpp)
namespace input {

void ReleaseCursor();            // the game lost the input: held keys wait for release
void Keyboard(Pad& pad);         // held keys onto port 0's pad

// actions the keyboard and mouse can be bound to, key names comma separated
// letter, digit and symbol keys bind by position as the US key there; Binding shows the player's labels
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
};
const Action* Actions(size_t* count);
std::string Binding(size_t action);
// slot 0 the key, 1 its alternate (the F10 panel's second box); vk 0 clears the slot. A key the other slot has moves.
// The settings panel is never left without a key.
void Bind(size_t action, int vk, int slot = 0);
void ResetBindings();
void LoadBindings();     // after the settings load
int PressedKey();        // a bindable key or mouse button held now, 0 for none
bool PanelKey(int vk);   // the settings panel is bound to this key and no key capture wants it
// the touch controls used last
bool TouchActive();

// the game's own menus
void SeenPad(const Pad& pad);  // a controller's report before the keys join it: its input hides the cursor
bool KeyboardActive();         // keyboard or mouse used last, not a controller

}  // namespace input
