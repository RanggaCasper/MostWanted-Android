#pragma once
#include <cstddef>
#include <string>

#include "common/mouse_keyboard.h"

// a controller's buttons, hats and axes onto the Xbox pad's, for the CONTROLLER page (native/input.cpp). An action
// with no input of its own keeps SDL's mapping; an input bound to an action no longer presses its mapped one.
namespace input {

const Action* PadActions(size_t* count);  // `defaults` names the mapping's input
std::string PadBinding(size_t action);     // the bound input's name, empty for the mapping's
void PadBind(size_t action, const std::string& source);  // empty goes back to the mapping
void ResetPadBindings();
void LoadPadBindings();      // after the settings load
std::string PressedPadInput();  // an input of any controller pressed now, empty for none

}  // namespace input
