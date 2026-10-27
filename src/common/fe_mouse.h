#pragma once
#include <string>

// Most Wanted's FEng mouse, the touch screen (src/game/fe_mouse.cpp)
namespace game {

void FeFrame();             // game thread, at each present
bool FeMenuShown();         // any thread: a menu that takes the pointer is up
bool FeMouseDown();         // game thread: FEng's mouse button down this update
bool FeFinger(float& x, float& y);  // any thread: the finger in FEng's screen, true while it is down
// the menu's button prompts lately, for A, B, X and Y: shown, and its label in capitals ("DEFAULTS")
struct FePrompts {
    bool shown[4] = {};
    std::string label[4];
};
FePrompts CurrentFePrompts();  // any thread
void FeTouch(bool down, float u, float v);  // any thread: the finger in shares of the frame, down or lifted
// any thread, a moment after a tap: it came down on the highlighted button, still the same, for the pad's A; once
bool FeTakeAccept();

}  // namespace game
