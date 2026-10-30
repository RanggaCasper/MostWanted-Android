#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool Capturing();  // a key or controller binding waits for its input
void ToggleMenu();
void DrawUi(double fps);
inline constexpr uint32_t kFpsCapRefresh = 1;  // Settings::fps_cap: just under the display's refresh rate
void PaceFrame();  // app::Pace, with kFpsCapRefresh; game thread, after the present

// the panel's rows, for the game's own options menu
namespace rows {

// a link row opens its page on a tap, an info row is a line of plain text; PadBind binds a controller's input
enum class Kind { Heading, Value, Bind, Link, Info, PadBind };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
// 0 graphics, 1 touch controls, 2 general, 3 mouse and keyboard, 4 controller, 5 credits, 6 cheats (the panel's alone)
const Row* Page(int page, size_t* count);
int PageCount();
const char* PageName(int page);  // as the panel's tab, untranslated

// the game's options menu (src/game/options_menu.cpp), on the game thread
std::string Label(const Row& r);                // translated
std::string Value(const Row& r, int step = 0);  // as shown, stepped first; empty for a heading
void Activate(const Row& r);  // a link opens; a bind row waits for its input, PollBinding takes it
void CancelBinding();
bool PollBinding();           // a bind row's wait took its input or gave up, once
void Defaults(int page);

}  // namespace rows

}  // namespace app
