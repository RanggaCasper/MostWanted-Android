// Most Wanted's menus are FEng packages, and FEng kept the PC's mouse: packages carry mouse regions (FF_MouseObject,
// MainMenu.fng's five: its arrows, its middle icon and the button prompts) that it tests a pointer against and sends
// over, press and click messages for (FEngine::UpdateMouseState, 0x158500). The console's FEngine has its mouse off
// (bMouseActive, FEngine + 1) and its interface's GetMouseInfo is an empty function shared with other empty methods
// (0x19BC0), so the touch screen is the mouse here: the interface's vtable slot points at a host function instead,
// and the mouse is on while a package that takes input has regions and a finger is on it or just left it: else the
// front end runs as the console's, its mouse off.
// The console's screens left the PC's mouse handling out (ArrayScroller::UpdateMouse is empty); what the packages'
// own scripts do with a click still works (MainMenu's arrows, a dialog's OK). A tap on the highlighted button, which
// those screens accept on the pad's A alone, is told to the touch controls to press A (FeTakeAccept).
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "common/aspect.h"
#include "common/fe_mouse.h"
#include "common/options.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // sdk/runtime/cpu/dispatch.cpp
}

namespace game {

void OptionsMenuTick(X86Context& ctx);  // options_menu.cpp

namespace {

// cFEng's instance; its first field the FEngine
constexpr uint32_t kFEng = 0x48E94C;
// FEngine: bMouseActive, the first package, the game's interface
constexpr uint32_t kMouseActive = 0x1, kFirstPackage = 0xE0, kInterface = 0x108;
constexpr uint32_t kGetMouseInfoSlot = 0x44;  // FEGameInterface's vtable
// FEPackage: the next, its controllers, input taken, its current button, its mouse regions
constexpr uint32_t kNext = 0x4, kControlMask = 0x20, kInputEnabled = 0x28, kCurrentButton = 0x8C, kMouseObjects = 0xB0;
constexpr int kMaxPackages = 64;
// FEngTestForIntersection(float x, float y, FEObject*), cdecl: a point of FEng's screen on an object as drawn
constexpr uint32_t kTestForIntersection = 0x10CF20;

std::atomic<bool> g_menu{false};
std::atomic<uint32_t> g_package{0};  // the menu's package, game thread
// the finger, FEng's screen; each touch counts a press, so one shorter than a frame is still seen down
std::atomic<int32_t> g_x{0}, g_y{0};
std::atomic<bool> g_down{false};
bool g_reported_down = false;  // the button as GetMouseInfo gave it this update, game thread
std::atomic<uint32_t> g_presses{0};
// presents the mouse stays on after a touch: past the pointer's leaving, so its regions see it go
constexpr int kMouseFrames = 30;
std::atomic<int> g_mouse_frames{0};
// the press came down on this package's highlighted button
std::atomic<uint32_t> g_accept_package{0}, g_accept_button{0};
// the menu has no mouse regions and prompts A (a tutorial's "Press A To Continue"): a tap anywhere is its A
std::atomic<bool> g_tap_anywhere{false};
constexpr uint32_t kAnywhere = 1;  // g_accept_button for such a tap

uint32_t FEngine() {
    uint32_t feng = kernel::R32(kFEng);
    return feng ? kernel::R32(feng) : 0;
}

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// the press came down on the package's highlighted button, one FEng's mouse does not click itself (a dialog's OK is
// a mouse region: A as well would accept the next screen too)
constexpr uint32_t kObjectFlags = 0x1C, kMouseObject = 1u << 17;
bool OnCurrentButton(X86Context& ctx) {
    uint32_t p = g_package, button = p ? kernel::R32(p + kCurrentButton) : 0;
    if (!button || (kernel::R32(button + kObjectFlags) & kMouseObject)) return false;
    uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t hit = kernel::CallGuest(ctx, kTestForIntersection, {Bits(float(g_x)), Bits(float(g_y)), button}) & 0xFF;
    ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4];
    ctx.edi = saved[5], ctx.ebp = saved[6];
    return hit != 0;
}

// FEngine::Update's GetMouseInfo(FEMouseInfo&), thiscall: x, y, wheel and the buttons, int16 each. A press shows down
// for two updates at least, for the press and then the click on its release; then the pointer stays a few updates
// and leaves, so a resting pointer neither keeps an item hovered nor fights the pad
void GetMouseInfo(X86Context& ctx, uint8_t*) {
    constexpr int kHoldUpdates = 2, kStayUpdates = 4;
    constexpr int16_t kAway = -2000;
    static uint32_t seen = 0;
    static int hold = 0, stay = 0;
    uint32_t info = kernel::Args{ctx}[0];
    uint32_t presses = g_presses;
    if (presses != seen) {
        seen = presses;
        hold = kHoldUpdates;
        uint32_t p = g_package;
        if (!g_tap_anywhere) {  // FeTouch took that one
            g_accept_button = OnCurrentButton(ctx) ? kernel::R32(p + kCurrentButton) : 0;
            g_accept_package = p;
        }
    }
    bool down = g_down || hold > 0;
    g_reported_down = down;
    if (hold > 0) hold--;
    if (down) stay = kStayUpdates;
    else if (stay > 0) stay--;
    bool here = down || stay > 0;
    kernel::W16(info, uint16_t(here ? int16_t(g_x.load()) : kAway));
    kernel::W16(info + 2, uint16_t(here ? int16_t(g_y.load()) : kAway));
    kernel::W16(info + 4, 0);
    kernel::W16(info + 6, down ? 1 : 0);  // FEMouse_Left
    OptionsMenuTick(ctx);
    kernel::Return(ctx, 0, 1);
}

// The menu's button prompts (the bar of "A Accept", "B Back", "Y Defaults"): in its package an image with the pad
// button's icon and the string after it in its group, the label; hidden ones, or ones in a hidden group, are not up
constexpr uint32_t kPackageObjects = 0x4C;
constexpr uint32_t kObjectNext = 0x4, kObjectType = 0x18, kObjectHidden = 1, kImageTexture = 0x24, kStringText = 0x64;
constexpr uint32_t kGroupCount = 0x60, kGroupObjects = 0x64;
constexpr uint32_t kTypeImage = 1, kTypeString = 2, kTypeGroup = 5;
constexpr uint32_t kPromptTextures[] = {0x02959349, 0x6851AAF5, 0x7A8B6BD0, 0x54A68051};  // A, B, X, Y
constexpr int kMaxObjects = 600;

uint32_t Peek(uint32_t va) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + 3) ? kernel::R32(va) : 0; }

// FEng's text, 16 bit characters, in capitals
std::string Label(uint32_t text) {
    std::string out;
    for (uint32_t i = 0; text && i < 24 && mem::IsCommitted(text + i * 2 + 1); i++) {
        uint16_t c = kernel::G<uint16_t>(text + i * 2);
        if (!c) break;
        out += c < 128 ? char(std::toupper(c)) : '?';
    }
    return out;
}

void ScanPrompts(uint32_t first, int count, FePrompts& out, int& budget) {
    int pending = -1;  // an icon waiting for its label
    for (uint32_t o = first; o && budget-- > 0 && count != 0; o = Peek(o + kObjectNext), count--) {
        if (Peek(o + kObjectFlags) & kObjectHidden) continue;
        uint32_t type = Peek(o + kObjectType);
        if (type == kTypeGroup) {
            ScanPrompts(Peek(o + kGroupObjects), int(Peek(o + kGroupCount)), out, budget);
        } else if (type == kTypeImage) {
            for (int i = 0; i < 4; i++)
                if (Peek(o + kImageTexture) == kPromptTextures[i]) {
                    out.shown[i] = true;
                    pending = i;
                }
        } else if (type == kTypeString && pending >= 0) {
            if (out.label[pending].empty()) out.label[pending] = Label(Peek(o + kStringText));
            pending = -1;
        }
    }
}

std::mutex g_prompts_lock;
FePrompts g_prompts;  // the menu's, lately

}  // namespace

FePrompts CurrentFePrompts() {
    std::lock_guard lock(g_prompts_lock);
    return g_menu ? g_prompts : FePrompts{};
}

void FeFrame() {
    uint32_t fe = FEngine();
    if (!fe) {
        g_menu = false;
        return;
    }
    static const uint32_t thunk = dispatch::HostThunk(GetMouseInfo);
    if (uint32_t iface = kernel::R32(fe + kInterface)) {
        uint32_t slot = kernel::R32(iface) + kGetMouseInfoSlot;
        if (kernel::R32(slot) != thunk) kernel::W32(slot, thunk);
    }
    // a package taking input with mouse regions; else one with no regions showing a pad prompt (a tutorial's "Press A
    // To Continue"), which no tap reaches: a menu still, for the touch controls' ENTER and CLOSE, its mouse off. Not in
    // a race, where a HUD's prompt would take the pad away
    uint32_t menu = 0, prompted = 0;
    bool racing = Racing();
    FePrompts prompts, found;
    int n = 0;
    for (uint32_t p = kernel::R32(fe + kFirstPackage); p && n < kMaxPackages && !menu; p = kernel::R32(p + kNext), n++) {
        if (!kernel::G<uint8_t>(p + kInputEnabled) || !(kernel::R32(p + kControlMask) & 1)) continue;
        if (int32_t(kernel::R32(p + kMouseObjects)) > 0) {
            menu = p;
        } else if (!prompted && !racing) {
            int budget = kMaxObjects;
            ScanPrompts(Peek(p + kPackageObjects), -1, found, budget);
            if (found.shown[0] || found.shown[1] || found.shown[2] || found.shown[3]) prompted = p;
            else found = {};
        }
    }
    int frames = g_mouse_frames;
    if (g_down) frames = kMouseFrames;
    else if (frames > 0) frames--;
    g_mouse_frames = frames;
    kernel::G<uint8_t>(fe + kMouseActive) = menu != 0 && frames > 0;
    if (menu) {
        int budget = kMaxObjects;
        ScanPrompts(Peek(menu + kPackageObjects), -1, prompts, budget);
    } else if (prompted) {
        menu = prompted;
        prompts = found;
    }
    g_package = menu;
    g_menu = menu != 0;
    g_tap_anywhere = menu && menu == prompted && prompts.shown[0];
    std::lock_guard lock(g_prompts_lock);
    g_prompts = prompts;
}

bool FeMenuShown() { return g_menu; }
bool FeMouseDown() { return g_reported_down; }
bool FeFinger(float& x, float& y) {
    x = float(g_x), y = float(g_y);
    return g_down;
}

// FEng's mouse may have answered the tap itself (the splash's whole screen region, a dialog's OK): then the screen is
// on its way out and A as well would act on the one that follows (the splash's A landed on a loading screen and
// crashed it). The touch controls ask a moment after the tap; the same screen with the same item highlighted and
// still taking input gets its A
bool FeTakeAccept() {
    uint32_t p = g_accept_package.exchange(0), button = g_accept_button.exchange(0);
    if (!button || p != g_package || !g_menu || !kernel::G<uint8_t>(p + kInputEnabled)) return false;
    return button == kAnywhere || kernel::R32(p + kCurrentButton) == button;
}

// FEng's screen has its origin in the middle of the frame (MC_Main_GC.fng's OK, 39 under the middle) and is 480 high.
// Its hit tests (FEngTestForIntersection) take it as 480 times the frame's aspect wide, in square pixels: with the
// console's widescreen flag the front end lays out to that width (WidenAspect's), and without it (a frame under 3:2)
// its 640 wide layout is stretched to the frame and tested so. 640 there put a tap on a row's right arrow 11% short
// of it on a 3:2 tablet
void FeTouch(bool down, float u, float v) {
    float wide = 480.0f * Aspect();
    g_x = int32_t((u - 0.5f) * wide);
    g_y = int32_t((v - 0.5f) * 480.0f);
    if (down && !g_down) {
        g_presses++;
        if (g_tap_anywhere) {
            g_accept_package = g_package.load();
            g_accept_button = kAnywhere;
        }
    }
    g_down = down;
    g_mouse_frames = kMouseFrames;
}

}  // namespace game
