#include <tuple>
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "app/app.h"
#include <map>
#include "core/input.h"
#include <mutex>
#include <atomic>
#include "common/pad_binds.h"
// The recomp's settings in the game's own options menu. UIOptionsMain (ctor 0x104C70, both the title's OPTIONS and the
// pause menu's) is an icon scroller its Setup (0xED5A0) fills with IconOptions (0x4C bytes, ctor 0x138BC0: texture,
// name, description); an icon's React (vtable + 4) sets the category, FEDatabase's options [+0x24], on Accept, and
// Accept's fade (0xE1FDE1D1) opens Options.fng or Pause_Options.fng for categories 0 to 9 (0x105019). There
// UIOptionsScreen::Setup (0x1055B0) adds the category's widgets and finishes with 0x153080; a category past 9 gets
// none. So a section of the panel's pages is one more icon (category kFirstSection + section) and its rows are toggle
// widgets (0x5C bytes, ctor 0x13A800, added by 0x139D40, which takes the next row's title and value strings and
// arrows from the package) whose Act and Draw are host functions. The scroller has a slot object a icon (option_1
// on, ten in Options.fng with the game's seven), so the six pages are three sections. They sit in a menu of their own,
// ANDROID OPTIONS, an icon of the main menu beside its OPTIONS: it opens the options' package as OPTIONS does (the
// FEDatabase flag 0x10, 0x0E3F30) and that UIOptionsMain gets the three sections' icons in place of the game's; a
// section's Back comes back to it. The icons' textures are made here: GetTextureInfo (0xC5900) answers their hashes with a copy
// of the Audio icon's TextureInfo whose D3D texture is ours (src/game/option_icons.inc, tools/option_icons.py).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/aspect.h"
#include "common/fe_mouse.h"
#include "common/menu.h"
#include "common/translations.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace dispatch {
uint32_t HostThunk(X86Func* fn);  // sdk/runtime/cpu/dispatch.cpp
}

namespace game {

void SetOwnString(uint32_t hash, const std::string& text);  // prompts.cpp

namespace {

using app::rows::Kind;
using app::rows::Row;
using kernel::CallGuest;
using kernel::R32;
using kernel::W32;

#include "game/option_icons.inc"  // kIconSize, kIcons: an icon a section, 0xAARRGGBB rows top down

constexpr uint32_t kFEng = 0x48E94C;        // cFEng
constexpr uint32_t kFEDatabase = 0x48EDD4;  // its options at [+0x10], their category at +0x24
constexpr uint32_t kCategory = 0x24;
constexpr uint32_t kFirstSection = 0x20;  // past the game's 0 to 9

// the game's functions: operator new (cdecl), FEPrintf (cdecl: string object, format, ...), FindPackage (cFEng,
// name), FindObject (package, hash), SwitchPackage (cFEng: name, arg, 0, 0)
constexpr uint32_t kNew = 0x40110, kPrintf = 0xE11F0, kFindPackage = 0xE1B10, kFindObject = 0x15CA70;
constexpr uint32_t kSwitchPackage = 0xEB160;
// IconOption: ctor (texture, name, description), its deleting destructor, the scroller's add
constexpr uint32_t kIconOptionSize = 0x4C, kIconCtor = 0x138BC0, kIconDtor = 0x1031A0, kAddIcon = 0x12A120;
// a toggle widget: ctor (enabled), the Audio page's first toggle's vtable, the screen's add (widget, 1)
constexpr uint32_t kWidgetSize = 0x5C, kWidgetCtor = 0x13A800, kToggleVtable = 0x3D4CF0, kAddWidget = 0x139D40;
constexpr uint32_t kVtableSlots = 15;
constexpr uint32_t kWidgetEnabled = 0x28, kWidgetChanged = 0x2A, kWidgetTitle = 0x2C, kWidgetValue = 0x30;
// vtable: Position (the title, value and arrows placed), the colours of a row the cursor stops on and of one it
// passes over, the cursor arriving (package) and leaving, as SetCurrent (0x149E20) calls them, the arrows' blink
// (message)
constexpr uint32_t kPositionSlot = 0x10, kNormalSlot = 0x1C, kGreySlot = 0x20, kBlinkSlot = 0x38;
constexpr uint32_t kHighlightSlot = 0x24, kUnhighlightSlot = 0x28;
// UIOptionsMain: the last message, in the game, the scroller's current option, the name and two more labels
constexpr uint32_t kMainLast = 0x134, kMainInGame = 0x144, kMainCurrent = 0x38;
constexpr uint32_t kMainLabels[] = {0x128, 0x12C};
// UIOptionsScreen, a UIWidgetMenu: the package name, the current widget, the rows' title prefix ("OPTION_NAME_"), the
// widgets added, the rows shown; SetCurrent (widget); the header's icon and title objects
constexpr uint32_t kScreenPackage = 0x10, kScreenCurrent = 0x34, kScreenTitlePrefix = 0xAC, kScreenCount = 0x100;
constexpr uint32_t kScreenVisible = 0x108, kSetCurrent = 0x149E20;
constexpr uint32_t kHeaderIcon = 0x8007B4C, kHeaderTitle = 0x42ADB44C;
constexpr uint32_t kObjectType = 0x18, kObjectFlags = 0x1C, kImageTexture = 0x24;
constexpr uint32_t kTypeImage = 1, kTypeString = 2, kTypeLanguage = 5, kImageDirty = 0x2400000;
constexpr uint32_t kSetLanguageString = 0xEAB30, kTitleHashes = 0x52430100;  // ours, "RC" 1 and the section
// FEng messages: Accept, the dpad, the fade after Accept
constexpr uint32_t kAccept = 0x0C407210, kLeft = 0x9120409E, kRight = 0xB5971BF1, kUp = 0x72619778, kDown = 0x911C0A4B;
constexpr uint32_t kFadeDone = 0xE1FDE1D1;
// return addresses: Setup's 0x128AB0 after its icons, UIOptionsScreen::Setup's 0x153080 in its default case
constexpr uint32_t kAfterIcons = 0xED82C, kAfterRows = 0x105758;
// the screens Accept opens, as 0x105029 picks them
constexpr uint32_t kPauseOptionsFng = 0x3D0604, kOptionsFng = 0x3D6A34;
constexpr uint32_t kScreenBack = 0x105479;  // the return from the options screen's switch back to the options
// UIOptionsMain::Setup's adds of the game's icons (0x12A120 called from 0xED5FC to 0xED81E)
constexpr uint32_t kSetupFirst = 0xED5A0, kSetupLast = 0xED82C;
// The menus ANDROID OPTIONS sits in, beside their OPTIONS, at their Setup's 0x128AB0: the main menu's (0x104900 on;
// OPTIONS, vtable 0x3D534C, sets the FEDatabase flag 0x10 on which the main menu's fade opens MainMenu_Sub.fng's
// UIOptionsMain, 0x11697E) and the pause menu's (0x105A00, icons by 0x0EED40; OPTIONS, vtable 0x3D537C, posts
// 0x33195CF0 after its fade, on which the pause menu opens Pause_Main.fng's, 0x10F12A). ANDROID OPTIONS' React is
// OPTIONS' own, then the options it opens get the sections
struct OptionsMenu {
    uint32_t after_icons, options_vtable, options_react;
};
constexpr OptionsMenu kOptionsMenus[] = {{0x104C0F, 0x3D534C, 0x0E3F30}, {0x105A5B, 0x3D537C, 0x0EEC30}};
constexpr uint32_t kTemplateTexture = 0xF37AF144;  // the Audio icon
constexpr uint32_t kIconHashes = 0x52430000;       // ours, "RC" and the section
constexpr uint32_t kStringHash = 0x157E40;         // bStringHash (text), cdecl

uint32_t g_scratch = 0;  // strings handed to FEPrintf
uint32_t g_icon_vtable = 0, g_widget_vtable = 0;
std::unordered_map<uint32_t, int> g_icons;  // an IconOption of ours, its section
uint32_t g_waiting = 0;
uint32_t g_waiting_screen = 0;
std::atomic<uint64_t> g_capture_target{0}, g_capture_request{0};
std::atomic<uint32_t> g_capture_package{0}, g_capture_screen{0};
std::atomic<int> g_capture_kind{0};
std::atomic<bool> g_capture_cancel{false}, g_capture_drain{false};
uint32_t g_capture_epoch = 0;

bool IsBinding(const Row* row) {
    return row && (row->kind == Kind::Bind || row->kind == Kind::PadBind);
}                     // the widget whose binding waits
// Assignments as this Controls screen was entered.
uint32_t g_binding_snapshot_screen = 0;
std::map<std::string, std::string> g_saved_keys;
std::map<std::string, std::string> g_saved_pad;

uint32_t g_textures[kIconCount] = {};
bool g_android_next = false;  // the next UIOptionsMain is ANDROID OPTIONS: the sections' icons, not the game's
uint32_t g_android_menu = 0;  // the UIOptionsMain that is
uint32_t g_android_react = 0;  // the React of OPTIONS beside ANDROID OPTIONS

// the panel's pages in three: graphics; the touch controls, keyboard and controller; the general settings and the
// credits
struct Section {
    const char* name;
    std::vector<int> pages;
};
// the last is the menu that holds them, the main menu's icon
const Section kSections[] = {{"GRAPHICS", {0}}, {"CONTROLS", {1, 3, 4}}, {"GENERAL", {2, 5}}, {"ANDROID OPTIONS", {}}};
constexpr int kSectionCount = int(std::size(kSections));
constexpr int kAndroidSection = kSectionCount - 1;
static_assert(kSectionCount <= kIconCount, "tools/option_icons.py draws an icon a section");

// a section's rows, its pages' one after the other; a page after the first opens with its name when it has no
// heading of its own
const std::vector<Row>& SectionRows(int section) {
    static std::vector<Row> rows[kSectionCount];
    static const std::vector<Row> none;
    if (section < 0 || section >= kSectionCount) return none;
    if (rows[section].empty())
        for (int page : kSections[section].pages) {
            size_t count = 0;
            const Row* list = app::rows::Page(page, &count);
            if (!rows[section].empty() && count && list[0].kind != Kind::Heading)
                rows[section].push_back({Kind::Heading, -1, app::rows::PageName(page)});
            rows[section].insert(rows[section].end(), list, list + count);
        }
    return rows[section];
}

// the game's options read "Sound Effects Volume", "On": each word's first letter in capitals, but for the short
// forms and anything with a digit in it
std::string TitleCase(std::string text) {
    static const char* const kKeep[] = {"FPS", "CPU", "GPU", "FXAA", "SSAA", "HDR", "CRT", "EA", "UI", "TV"};
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(" /()-", start);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(start, end - start);
        bool keep = std::any_of(word.begin(), word.end(), [](char c) { return std::isdigit(uint8_t(c)); });
        for (const char* k : kKeep) keep |= word == k;
        for (size_t i = start + 1; !keep && i < end; i++)
            if (uint8_t(text[i]) < 0x80) text[i] = char(std::tolower(uint8_t(text[i])));
        start = end + 1;
    }
    return text;
}

std::string SectionTitle(int section) { return TitleCase(app::Ui(kSections[section].name)); }

uint32_t Options() { return R32(R32(kFEDatabase) + 0x10); }

// the section the category opens, -1 for the game's own
int CategorySection() {
    uint32_t options = R32(kFEDatabase) ? Options() : 0;
    if (!options) return -1;
    int section = int(R32(options + kCategory)) - int(kFirstSection);
    return section >= 0 && section < kSectionCount ? section : -1;
}

uint32_t Scratch() {
    if (!g_scratch) g_scratch = mem::AllocSystem(512);
    return g_scratch;
}

// the game's fonts are Latin-1: UTF-8 to it, what it lacks as '?'
std::string Latin1(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        uint8_t c = uint8_t(text[i]);
        if (c < 0x80) {
            out += char(c);
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            uint32_t cp = (c & 0x1Fu) << 6 | (uint8_t(text[++i]) & 0x3Fu);
            out += cp < 0x100 ? char(cp) : '?';
        } else {
            while (i + 1 < text.size() && (uint8_t(text[i + 1]) & 0xC0) == 0x80) i++;
            out += '?';
        }
    }
    return out;
}

// FEPrintf(object, "%s", text). It sets the object's flag 2, with which a row's new text after a scroll stayed where
// the text before it was placed; cleared, as 0xE10F0 does after a language string, it is placed again. It cuts the
// text to the string's room (0xE11B3: the options' header has 14 letters, "Android Option"), so the room is made first
// (0x15D680, the wide string's reserve)
constexpr uint32_t kStringBuffer = 0x64, kStringRoom = 0x68, kStringReserve = 0x15D680;
void Print(X86Context& ctx, uint32_t object, const std::string& text) {
    if (!object) return;
    std::string latin = Latin1(text).substr(0, 499);
    if (latin.size() + 1 > R32(object + kStringRoom)) {
        ctx.ecx = object + kStringBuffer;
        CallGuest(ctx, kStringReserve, {uint32_t(latin.size() + 1)});
    }
    uint32_t s = Scratch();
    std::memcpy(kernel::HostPtr(s), "%s", 3);
    std::snprintf(static_cast<char*>(kernel::HostPtr(s + 4)), 500, "%s", latin.c_str());
    CallGuest(ctx, kPrintf, {object, s, s + 4});
    W32(object + kObjectFlags, R32(object + kObjectFlags) & ~0x2u);
}

uint32_t FindObject(X86Context& ctx, uint32_t package_name, uint32_t hash) {
    if (!package_name) return 0;
    ctx.ecx = R32(kFEng);
    uint32_t package = CallGuest(ctx, kFindPackage, {package_name});
    if (!package) return 0;
    ctx.ecx = package;
    return CallGuest(ctx, kFindObject, {hash});
}

// thiscall from the host
uint32_t CallThis(X86Context& ctx, uint32_t self, uint32_t fn, std::initializer_list<uint32_t> args) {
    ctx.ecx = self;
    return CallGuest(ctx, fn, args);
}

// ---- the icons' textures

// Morton order, as the NV2A reads a square swizzled texture: x's bits on the even places, y's on the odd
uint32_t Swizzle(uint32_t x, uint32_t y) {
    uint32_t out = 0;
    for (uint32_t bit = 0; bit < 16; bit++) {
        out |= ((x >> bit) & 1) << (2 * bit);
        out |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    return out;
}

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) n++;
    return n;
}

// The Audio icon's TextureInfo (OPTIONS_ICON_AUDIO: 64x64 at +0x40, its platform record at +0) and platform record,
// copied; the D3D texture in the copy (platform record + 0x10; the Audio icon's a DXT3, NV2A format 0x0E) reads our
// pixels, A8R8G8B8 swizzled, one level. Their size is the Audio icon's, kIconSize
uint32_t MakeTexture(X86Context& ctx, int section) {
    constexpr uint32_t kInfoBytes = 0x80, kPlatBytes = 0x40, kD3D = 0x10, kInfoHash = 0x24;
    uint32_t tmpl = CallGuest(ctx, 0xC5900, {kTemplateTexture, 0, 0});
    if (!tmpl || !R32(tmpl)) return 0;
    uint32_t info = mem::AllocSystem(kInfoBytes);
    uint32_t plat = mem::AllocSystem(kPlatBytes);
    std::memcpy(kernel::HostPtr(info), kernel::HostPtr(tmpl), kInfoBytes);
    std::memcpy(kernel::HostPtr(plat), kernel::HostPtr(R32(tmpl)), kPlatBytes);
    W32(info, plat);
    W32(info + kInfoHash, kIconHashes + uint32_t(section));

    uint32_t size = kIconSize, shift = Log2(size);
    uint32_t data = mem::AllocContiguous(size * size * 4, 0, 0xFFFFFFFF, 0x1000);
    const uint32_t* src = kIcons[section];
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) W32(data + Swizzle(x, y) * 4, src[y * size + x]);
    uint32_t d3d = plat + kD3D;
    uint32_t format = (R32(d3d + 12) & 0xFF) | 0x06u << 8 | 1u << 16 | shift << 20 | shift << 24;
    W32(d3d + 4, mem::PhysicalFromVirtual(data));
    W32(d3d + 8, 0);
    W32(d3d + 12, format);
    W32(d3d + 16, 0);
    return info;
}

// ---- the icons

// IconOption::React(..., message, ...), thiscall, five arguments: Accept picks our category. It runs within the fade
// after Accept, before UIOptionsMain::Notification picks the screen
void IconReact(X86Context& ctx, uint8_t*) {
    auto it = g_icons.find(ctx.ecx);
    uint32_t esp = ctx.esp;
    if (it != g_icons.end() && it->second == kAndroidSection && g_android_react) {
        // a menu's: as OPTIONS beside it, and the options it opens are ANDROID OPTIONS'
        CallThis(ctx, ctx.ecx, g_android_react, {R32(esp + 4), R32(esp + 8), R32(esp + 12), R32(esp + 16), R32(esp + 20)});
        if (R32(esp + 8) == kAccept) g_android_next = true;
    } else if (it != g_icons.end() && R32(esp + 8) == kAccept) {
        W32(Options() + kCategory, kFirstSection + uint32_t(it->second));
    }
    ctx.esp = esp + 4 + 0x14;
}

// ANDROID OPTIONS' node in the scroller's list (option + 4: next, previous) moved to after OPTIONS'; it was added last
void PlaceBeside(uint32_t menu, uint32_t options_vtable) {
    uint32_t head = menu + 0x2C + 4, ours = 0, target = 0;  // the scroller (+0x2C), its list (+4)
    for (uint32_t n = R32(head), k = 0; n && n != head && k < 64; n = R32(n), k++) {
        if (R32(n - 4) == g_icon_vtable) ours = n;
        else if (R32(n - 4) == options_vtable) target = n;
    }
    if (!ours || !target) return;
    // React at once on Accept, as OPTIONS does (+0x45: the pause menu's icons, 0x0EEEC1; the scroller's 0x139884)
    kernel::G<uint8_t>(ours - 4 + 0x45) = kernel::G<uint8_t>(target - 4 + 0x45);
    if (R32(target) == ours) return;
    W32(R32(ours + 4), R32(ours));  // out
    W32(R32(ours) + 4, R32(ours + 4));
    uint32_t next = R32(target);  // in after OPTIONS
    W32(ours, next);
    W32(ours + 4, target);
    W32(target, ours);
    W32(next + 4, ours);
}

// the scroller (UIOptionsMain + 0x2C, 0x139200) takes an option while its package has an object "<prefix><count>"
// (option_1 on), and deletes one it has no object for
constexpr uint32_t kMainScroller = 0x2C, kScrollerPackage = 0x18, kScrollerPrefix = 0x1C, kScrollerCount = 0x24;
constexpr uint32_t kFindImage = 0xEAAB0;
bool SlotFree(X86Context& ctx, uint32_t menu) {
    uint32_t scroller = menu + kMainScroller;
    uint32_t s = Scratch();
    std::snprintf(static_cast<char*>(kernel::HostPtr(s)), 64, "%s%u",
                  kernel::ReadCString(R32(scroller + kScrollerPrefix), 32).c_str(), R32(scroller + kScrollerCount));
    uint32_t hash = CallGuest(ctx, kStringHash, {s});
    return CallGuest(ctx, kFindImage, {R32(scroller + kScrollerPackage), hash}) != 0;
}

// the icons of sections first to last, after the menu's own
void AddIcons(X86Context& ctx, uint32_t menu, int first, int last) {
    if (!g_icon_vtable) {
        g_icon_vtable = mem::AllocSystem(8);
        W32(g_icon_vtable, kIconDtor);
        W32(g_icon_vtable + 4, dispatch::HostThunk(IconReact));
    }
    g_icons.clear();
    for (int section = first; section < last && SlotFree(ctx, menu); section++) {
        uint32_t option = CallGuest(ctx, kNew, {kIconOptionSize});
        if (!option) break;
        CallThis(ctx, option, kIconCtor, {kIconHashes + uint32_t(section), 0, 0});
        W32(option, g_icon_vtable);
        g_icons[option] = section;
        CallThis(ctx, menu, kAddIcon, {option});
    }
}

// ---- the rows

// The screen has an object of each kind for so many rows (OPTION_NAME_1 on), and a row past them would have none
// (0x12A310 finds "<prefix><n>", 0x139D40 asks for the next n). So a section longer than the objects or the screen's
// visible rows gets that many widgets, and the rows scroll through them: widget i shows row top + i
struct Window {
    uint32_t screen = 0;
    int section = -1;
    std::vector<uint32_t> widgets;
    size_t top = 0;
};
Window g_window;

const std::vector<Row>& Rows() { return SectionRows(g_window.section); }

// the row a widget of ours shows, nullptr for another
const Row* RowOf(uint32_t widget) {
    const std::vector<Row>& rows = Rows();
    for (size_t i = 0; i < g_window.widgets.size(); i++)
        if (g_window.widgets[i] == widget && g_window.top + i < rows.size()) return &rows[g_window.top + i];
    return nullptr;
}

// headings and the credits are rows the cursor passes over, greyed
bool Selectable(const Row& r) { return r.kind != Kind::Heading && r.kind != Kind::Info; }

float F32(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// The game's rows end their titles at the left arrow; the toggle's Position (0x13A710) puts a title's middle at the
// end of its box (widget + 0x0C, 0x34 wide; OPTION_NAME_n justified 2, drawn centred on its place). So ours are
// placed again, their middle half their width before where the game's titles end: kTitleEnd past the box. The width
// is the title's font's (0x138060, thiscall (text, 0), on the x87 stack) times the object's scale (its data + 0x38)
constexpr uint32_t kFontOf = 0x137A40, kMeasure = 0x138060, kSetPosition = 0xEA970, kToggleWidgetPosition = 0x13A870;
constexpr uint32_t kWidgetX = 0x0C, kWidgetY = 0x10, kWidgetTitleWidth = 0x34, kWidgetHeight = 0x38;
constexpr uint32_t kStringFont = 0x24, kStringText = 0x64, kObjectData = 0x2C, kDataScaleX = 0x38;
constexpr float kTitleEnd = 53.0f;  // FEng units past the box: a little before the left arrow
constexpr uint32_t kVideoFlags = 0x48099C;  // XGetVideoFlags kept at boot, bit 0 widescreen (src/game/game.cpp)
void AlignTitle(X86Context& ctx, uint32_t widget) {
    uint32_t title = R32(widget + kWidgetTitle);
    if (!title || !R32(title + kStringText)) return;
    uint32_t font = CallGuest(ctx, kFontOf, {R32(title + kStringFont)});
    if (!font) return;
    CallThis(ctx, font, kMeasure, {R32(title + kStringText), 0});
    float width = float(X87_ST(0)) * F32(R32(title + kObjectData) + kDataScaleX);
    ctx.top = (ctx.top + 1) & 7;  // the fstp the caller would do
    // without the console's widescreen flag (a screen under 3:2) FEng's 640 wide screen is stretched to the frame's
    // shape but its letters are not: they take the share of the width a 4:3 frame would
    if (!(kernel::G<uint8_t>(kVideoFlags) & 1)) width *= 640.0f / (480.0f * Aspect());
    float x = F32(widget + kWidgetX) + F32(widget + kWidgetTitleWidth) + kTitleEnd - width * 0.5f;
    float y = F32(widget + kWidgetY) + F32(widget + kWidgetHeight) * 0.5f;
    CallGuest(ctx, kSetPosition, {title, Bits(x), Bits(y)});
}

// Position, thiscall: the toggle's, then the title against the arrow
void WidgetPosition(X86Context& ctx, uint8_t*) {
    uint32_t widget = ctx.ecx, esp = ctx.esp;
    CallThis(ctx, widget, kToggleWidgetPosition, {});
    if (RowOf(widget)) AlignTitle(ctx, widget);
    ctx.esp = esp + 4;
}

// Draw, thiscall: the title and the value, as the game's rows in title case; a heading stays in capitals, and a key's
// name or a person's as it is
void WidgetDraw(X86Context& ctx, uint8_t*) {
    uint32_t widget = ctx.ecx;
    if (const Row* r = RowOf(widget)) {
        std::string label = app::rows::Label(*r), value = app::rows::Value(*r);
        bool names = r->kind == Kind::Info || r->kind == Kind::Bind || r->kind == Kind::PadBind;
        Print(ctx, R32(widget + kWidgetTitle), r->kind == Kind::Heading ? label : TitleCase(label));
        Print(ctx, R32(widget + kWidgetValue), names ? value : TitleCase(value));
        // a string's place is set through its font (0xEA970), justified for the text it has then: placed again for
        // the new text, else it keeps the last text's place
        uint32_t esp = ctx.esp;
        CallThis(ctx, widget, R32(R32(widget) + kPositionSlot), {});
        ctx.esp = esp;
    }
    ctx.esp += 4;
}

void Redraw(X86Context& ctx, uint32_t widget) {
    if (!RowOf(widget) || R32(widget) != g_widget_vtable) return;
    uint32_t saved = ctx.esp;
    ctx.esp -= 4;  // WidgetDraw pops a return address
    ctx.ecx = widget;
    WidgetDraw(ctx, nullptr);
    ctx.esp = saved;
}

// a selectable row's arrows as the cursor leaves them, or as it arrives on the current one: NORMAL and the package's
// first state show the arrows (both, or the right alone) on rows the cursor is not on
void SetCursorState(X86Context& ctx, uint32_t widget) {
    uint32_t screen = g_window.screen;
    if (widget == R32(screen + kScreenCurrent)) CallThis(ctx, widget, R32(R32(widget) + kHighlightSlot), {R32(screen + kScreenPackage)});
    else CallThis(ctx, widget, R32(R32(widget) + kUnhighlightSlot), {});
}

// every widget's row again: its colours (the vtable's NORMAL or GREY) and text
void RedrawAll(X86Context& ctx) {
    const std::vector<Row>& rows = Rows();
    for (size_t i = 0; i < g_window.widgets.size() && g_window.top + i < rows.size(); i++) {
        uint32_t widget = g_window.widgets[i];
        bool enabled = Selectable(rows[g_window.top + i]);
        if (kernel::G<uint8_t>(widget + kWidgetEnabled) != enabled) {
            CallThis(ctx, widget, R32(R32(widget) + (enabled ? kNormalSlot : kGreySlot)), {});
            if (enabled) SetCursorState(ctx, widget);
        }
        Redraw(ctx, widget);
    }
}

// FEng's mouse (fe_mouse.cpp) on our rows. The PC's mouse made a touched row the current one; the console's screens
// dropped that, so a tap on another row's arrow stepped the current row's value and left that row's arrow showing.
// FEng still finds the arrow under the finger itself and gives it its pressed script as the finger comes down,
// in the same update as its click: that row is made current as the click comes (WidgetAct), the click stepping it,
// and on the next update for a press that clicks nothing. Only while our screen's package is up (the screen goes
// with it), its widgets read with care
constexpr uint32_t kWidgetLeft = 0x4C, kWidgetRight = 0x50, kObjectScript = 0x54, kScriptId = 0x30;
constexpr uint32_t kArrowPressed = 0xE647B375;  // an arrow's script while FEng's mouse is down on it

uint32_t Peek(uint32_t va) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + 3) ? R32(va) : 0; }

// our widgets while their screen is up: our section's screen and its package still loaded (the screen's name for it
// one of the two options packages), and the cursor's row
bool LiveWidgets(X86Context& ctx, uint32_t& current, uint32_t* package = nullptr) {
    uint32_t screen = g_window.screen;
    if (!screen || g_window.widgets.empty() || CategorySection() != g_window.section) return false;
    uint32_t name = Peek(screen + kScreenPackage);
    if (!name || !Peek(name)) return false;
    std::string text = kernel::ReadCString(name, 32);
    if (text != kernel::ReadCString(kOptionsFng, 32) && text != kernel::ReadCString(kPauseOptionsFng, 32)) return false;
    ctx.ecx = R32(kFEng);
    uint32_t found = CallGuest(ctx, kFindPackage, {name});
    if (!found) return false;
    if (package) *package = found;
    current = Peek(screen + kScreenCurrent);
    return true;
}

uint32_t InputPackage() {
    const uint32_t engine = Peek(Peek(kFEng));
    uint32_t package = engine ? Peek(engine + 0xE0) : 0;
    for (unsigned count = 0; package && count < 64; ++count) {
        if (!mem::IsCommitted(package + 0x28)) return 0;
        if (kernel::G<uint8_t>(package + 0x28) &&
            (Peek(package + 0x20) & 1))
            return package;
        package = Peek(package + 4);
    }
    return 0;
}

bool CaptureOwner(X86Context& ctx, uint32_t& current, uint32_t& package) {
    return !app::MenuOpen() &&
        LiveWidgets(ctx, current, &package) &&
        InputPackage() == package &&
        Peek(current) == g_widget_vtable;
}

bool ArrowPressed(uint32_t widget) {
    for (uint32_t field : {kWidgetLeft, kWidgetRight})
        if (Peek(Peek(Peek(widget + field) + kObjectScript) + kScriptId) == kArrowPressed) return true;
    return false;
}

// a row the cursor can stop on whose arrow the finger is down on, the current one too; 0 for none
uint32_t PressedRow(X86Context& ctx, uint32_t& current) {
    current = 0;
    if (!LiveWidgets(ctx, current)) return 0;
    for (uint32_t widget : g_window.widgets)
        if (Peek(widget) == g_widget_vtable && kernel::G<uint8_t>(widget + kWidgetEnabled) && ArrowPressed(widget))
            return widget;
    return 0;
}

// Left or Right on a row: its value stepped, its arrow blinked
void Step(X86Context& ctx, uint32_t widget, uint32_t message) {
    const Row* r = RowOf(widget);
    if (!r || r->kind != Kind::Value) return;
    app::rows::Value(*r, message == kLeft ? -1 : 1);
    CallThis(ctx, widget, R32(R32(widget) + kBlinkSlot), {message});
    kernel::G<uint8_t>(widget + kWidgetChanged) = 1;
    RedrawAll(ctx);  // a value can change others (the language, the steering's tilt angle)
}

// FEng sends an arrow's click as the finger comes down, before the arrow shows it is pressed: a click under the finger
// waits for that, a few updates at most, then goes to the current row
uint32_t g_pending = 0;
int g_pending_updates = 0;
constexpr int kPendingUpdates = 6;

// each update: the row whose arrow the finger came down on made current, and the click that came with it stepping it
void FocusTouchedRow(X86Context& ctx) {
    uint32_t current;
    uint32_t pressed = PressedRow(ctx, current);
    if (pressed && pressed != current) {
        CallThis(ctx, g_window.screen, kSetCurrent, {pressed});
        current = pressed;
    }
    if (!g_pending || (!pressed && ++g_pending_updates < kPendingUpdates)) return;
    uint32_t message = g_pending;
    g_pending = 0;
    if (current) Step(ctx, current, message);
}

// A tap on a row's title or value, which are no mouse regions: FEng's mouse only knows the arrows, so EDIT CONTROLS
// POSITION's OPEN or a binding took the dpad to the row and A. A finger down and up on one row makes it current, and
// opens a link or a binding as Accept would; the pad's A that a tap on the highlighted row would give is let go
constexpr uint32_t kTestForIntersection = 0x10CF20;  // FEngTestForIntersection(x, y, object), cdecl
uint32_t g_tap_row = 0;
bool g_finger_was_down = false;
float g_finger_x = 0, g_finger_y = 0;

// FEng tests an object in its group's frame. The package's mouse regions (states at +0xAC, 0x10 each: the object,
// then its offset in the screen) are tested at the pointer less that offset plus the object's position
// (FEngine::UpdateMouseState, 0x158500; FEngGetPosition 0xEA7E0); a row's strings share its arrows' group, so the
// arrows' entries give the group's offset for them too
constexpr uint32_t kMouseStates = 0xAC, kMouseStateSize = 0x10, kGetPosition = 0xEA7E0;
constexpr uint32_t kPackageMouseCount = 0xB0;

bool GroupOffset(X86Context& ctx, uint32_t package, float& ox, float& oy) {
    uint32_t states = Peek(package + kMouseStates), count = Peek(package + kPackageMouseCount);
    for (uint32_t i = 0; states && i < count && i < 256; i++) {
        uint32_t entry = states + i * kMouseStateSize, object = Peek(entry);
        if (!object) continue;
        for (uint32_t widget : g_window.widgets) {
            if (object != Peek(widget + kWidgetLeft) && object != Peek(widget + kWidgetRight)) continue;
            uint32_t s = Scratch();
            CallGuest(ctx, kGetPosition, {object, s, s + 4});
            ox = F32(entry + 4) - F32(s);
            oy = F32(entry + 8) - F32(s + 4);
            return true;
        }
    }
    return false;
}

// a link or a binding on our row opened, as Accept does; false for another row
bool OpenRow(X86Context& ctx, uint32_t widget) {
    const Row* row = RowOf(widget);
    if (!row || (!IsBinding(row) && row->kind != Kind::Link))
        return false;

    if (IsBinding(row)) {
        uint32_t current = 0, package = 0;
        if (!CaptureOwner(ctx, current, package) || current != widget ||
            app::Capturing() || g_waiting || g_capture_drain.load())
            return true;
        g_capture_target = 0;
        g_capture_cancel = false;
        g_capture_kind = row->kind == Kind::Bind ? 1 : 2;
    }

    app::rows::Activate(*row);
    if (IsBinding(row)) {
        if (app::Capturing()) {
            g_waiting = widget;
            g_waiting_screen = g_window.screen;
        } else {
            g_capture_kind = 0;
        }
    }
    Redraw(ctx, widget);
    return true;
}

bool Hits(X86Context& ctx, float x, float y, uint32_t object) {
    return object && (CallGuest(ctx, kTestForIntersection, {Bits(x), Bits(y), object}) & 0xFF);
}

// the selectable row whose title, value or arrows are under the point, 0 for none
uint32_t RowAt(X86Context& ctx, float x, float y) {
    uint32_t current, package;
    float ox, oy;
    if (!LiveWidgets(ctx, current, &package) || !GroupOffset(ctx, package, ox, oy)) return 0;
    x -= ox, y -= oy;
    for (uint32_t widget : g_window.widgets) {
        if (Peek(widget) != g_widget_vtable || !kernel::G<uint8_t>(widget + kWidgetEnabled)) continue;
        for (uint32_t field : {kWidgetTitle, kWidgetValue, kWidgetLeft, kWidgetRight})
            if (Hits(ctx, x, y, Peek(widget + field))) return widget;
    }
    return 0;
}

void TapRows(X86Context& ctx) {
    float x, y;
    bool down = FeFinger(x, y);
    if (down) g_finger_x = x, g_finger_y = y;
    if (down && !g_finger_was_down) g_tap_row = RowAt(ctx, x, y);
    bool lifted = !down && g_finger_was_down;
    g_finger_was_down = down;
    if (!lifted || !g_tap_row) return;
    uint32_t widget = g_tap_row;
    g_tap_row = 0;
    uint32_t current;
    if (RowAt(ctx, g_finger_x, g_finger_y) != widget || !LiveWidgets(ctx, current)) return;
    if (widget != current) CallThis(ctx, g_window.screen, kSetCurrent, {widget});
    if (OpenRow(ctx, widget)) FeTakeAccept();
}

// Act(..., message), thiscall: the dpad steps a value, Accept opens a link or waits for a binding
void WidgetAct(X86Context& ctx, uint8_t*) {
    uint32_t widget = ctx.ecx, message = R32(ctx.esp + 8);
    uint32_t esp = ctx.esp;
    if (g_waiting || g_capture_drain.load()) {
        ctx.esp = esp + 4 + 8;
        return;
    }
    bool step = message == kLeft || message == kRight;
    if (step && FeMouseDown()) {  // an arrow's click: whose row the next update shows
        g_pending = message;
        g_pending_updates = 0;
    } else if (step) {
        Step(ctx, widget, message);
    } else if (message == kAccept) {
        const Row* row = RowOf(widget);
        if (row && row->kind == Kind::Link) OpenRow(ctx, widget);
    }
    ctx.esp = esp + 4 + 8;
}

// the screen has the objects for row n
bool RowSlot(X86Context& ctx, uint32_t screen, uint32_t n) {
    uint32_t s = Scratch();
    std::snprintf(static_cast<char*>(kernel::HostPtr(s)), 64, "%s%u",
                  kernel::ReadCString(R32(screen + kScreenTitlePrefix), 32).c_str(), n);
    uint32_t hash = CallGuest(ctx, kStringHash, {s});
    uint32_t package = R32(screen + kScreenPackage);
    return FindObject(ctx, package, hash) != 0;
}

auto GraphicsFields() {
    auto& s = app::Get();
    auto& o = game::Opt();
    return std::tie(
        s.render_percent,
        s.vsync,
        s.fps_cap,
        s.aniso,
        o.aspect_ratio,
        o.fps_counter,
        o.fps_position,
        o.fps_usage,
        o.fps_temps,
        o.shader_preload,
        o.anti_aliasing,
        o.bloom,
        o.fog,
        o.reflections,
        o.road_reflections,
        o.shadows,
        o.particles,
        o.rain,
        o.rain_drops,
        o.yellow_tint,
        o.motion_blur,
        o.screen_filter
    );
}

auto GraphicsValues() {
    return std::apply(
        [](const auto&... values) {
            return std::make_tuple(values...);
        },
        GraphicsFields()
    );
}

decltype(GraphicsValues()) g_saved_graphics{};
uint32_t g_section_snapshot_screen = 0;
int g_section_snapshot_index = -1;
std::vector<std::string> g_saved_section_values;

// Value rows only: headings, links, and capture prompts are not settings.
// Bindings are compared separately using their actual assignment maps.
std::vector<std::string> SectionValueState(int section) {
    std::vector<std::string> values;
    for (const Row& row : SectionRows(section))
        if (row.kind == Kind::Value)
            values.push_back(app::rows::Value(row));
    return values;
}

void SnapshotBindings(uint32_t screen, int section) {
    g_section_snapshot_screen =
        section >= 0 && section < kAndroidSection ? screen : 0;
    g_section_snapshot_index = section;
    g_saved_section_values = SectionValueState(section);
    g_saved_graphics = GraphicsValues();

    g_binding_snapshot_screen = 0;
    g_saved_keys.clear();
    g_saved_pad.clear();

    if (section < 0 || section >= kSectionCount) return;

    bool has_bindings = false;
    for (const Row& row : SectionRows(section))
        if (IsBinding(&row)) {
            has_bindings = true;
            break;
        }

    if (!has_bindings) return;
    g_saved_keys = game::Opt().keys;
    g_saved_pad = game::Opt().pad;
    g_binding_snapshot_screen = screen;
}

bool SectionUnchanged(uint32_t screen, int section) {
    if (!screen || screen != g_section_snapshot_screen ||
        section != g_section_snapshot_index)
        return false;

    // Graphics comparison uses actual values, not displayed resolution
    // dimensions, which could change when the device display changes.
    if (section == 0)
        return GraphicsValues() == g_saved_graphics;

    if (screen == g_binding_snapshot_screen &&
        (game::Opt().keys != g_saved_keys ||
         game::Opt().pad != g_saved_pad))
        return false;

    return SectionValueState(section) == g_saved_section_values;
}

void AddRows(X86Context& ctx, uint32_t screen, int section) {
    if (!g_widget_vtable) {
        g_widget_vtable = mem::AllocSystem(kVtableSlots * 4);
        for (uint32_t i = 0; i < kVtableSlots; i++) W32(g_widget_vtable + i * 4, R32(kToggleVtable + i * 4));
        W32(g_widget_vtable + 4, dispatch::HostThunk(WidgetAct));
        W32(g_widget_vtable + 12, dispatch::HostThunk(WidgetDraw));
        W32(g_widget_vtable + kPositionSlot, dispatch::HostThunk(WidgetPosition));
    }
    if (g_waiting) {
        app::rows::CancelBinding();
        g_capture_drain = true;
    }
    g_capture_target = 0;
    g_capture_request = 0;
    g_capture_kind = 0;
    g_capture_cancel = false;
    g_waiting_screen = 0;
    g_pending = 0;
    SnapshotBindings(screen, section);
    g_window = Window{screen, section, {}, 0};
    g_waiting = 0;
    g_tap_row = 0;  // a finger still down from the screen before: its row is not this screen's
    const std::vector<Row>& rows = Rows();
    size_t visible = std::max<uint32_t>(R32(screen + kScreenVisible), 1);
    while (g_window.widgets.size() < rows.size() && g_window.widgets.size() < visible &&
           RowSlot(ctx, screen, R32(screen + kScreenCount))) {
        const Row& r = rows[g_window.widgets.size()];
        uint32_t widget = CallGuest(ctx, kNew, {kWidgetSize});
        if (!widget) break;
        CallThis(ctx, widget, kWidgetCtor, {Selectable(r) ? 1u : 0u});
        W32(widget, g_widget_vtable);
        g_window.widgets.push_back(widget);
        CallThis(ctx, screen, kAddWidget, {widget, 1});
        if (Selectable(r)) SetCursorState(ctx, widget);  // its objects found: the arrows off until the cursor comes
    }
    // the header: the section's icon and name
    uint32_t package = R32(screen + kScreenPackage);
    uint32_t icon = FindObject(ctx, package, kHeaderIcon);
    if (icon && R32(icon + kObjectType) == kTypeImage) {
        W32(icon + kImageTexture, kIconHashes + uint32_t(section));
        W32(icon + kObjectFlags, R32(icon + kObjectFlags) | kImageDirty);
    }
    uint32_t title = FindObject(ctx, package, kHeaderTitle);
    // a plain string printed; another kind (5, the pause menu's) takes a language hash (0xEAB30: package, object,
    // hash), one of ours that SearchForString answers with the section's name (prompts.cpp)
    if (title && R32(title + kObjectType) == kTypeString) {
        Print(ctx, title, SectionTitle(section));
    } else if (title && R32(title + kObjectType) == kTypeLanguage) {
        uint32_t hash = kTitleHashes + uint32_t(section);
        SetOwnString(hash, Latin1(SectionTitle(section)));
        CallGuest(ctx, kSetLanguageString, {package, kHeaderTitle, hash});
    }
}

// Up or Down past the widgets' rows scrolls them, the cursor to the next row the cursor stops on; false leaves the
// message to the screen
bool Scroll(X86Context& ctx, uint32_t screen, int step) {
    const std::vector<Row>& rows = Rows();
    size_t shown = g_window.widgets.size();
    if (screen != g_window.screen || rows.size() <= shown) return false;
    size_t at = 0;
    while (at < shown && g_window.widgets[at] != R32(screen + kScreenCurrent)) at++;
    if (at == shown) return false;
    long target = long(g_window.top + at) + step;
    while (target >= 0 && target < long(rows.size()) && !Selectable(rows[size_t(target)])) target += step;
    if (target < 0 || target >= long(rows.size())) {
        // the ends: the heading above the first row comes into view
        if (step < 0 && g_window.top > 0) {
            g_window.top = 0;
            RedrawAll(ctx);
        }
        return true;
    }
    size_t top = g_window.top;
    if (size_t(target) < top) top = size_t(target) > 0 && !Selectable(rows[size_t(target) - 1]) ? target - 1 : target;
    else if (size_t(target) >= top + shown) top = size_t(target) - shown + 1;
    if (top == g_window.top) return false;
    g_window.top = top;
    RedrawAll(ctx);
    CallThis(ctx, screen, kSetCurrent, {g_window.widgets[size_t(target) - top]});
    return true;
}

void FinishBindingDialog(X86Context& ctx) {
    app::rows::CancelBinding();
    g_waiting = 0;
    g_waiting_screen = 0;
    g_capture_target = 0;
    g_capture_request = 0;
    g_capture_kind = 0;
    g_capture_cancel = false;

    // The A that confirmed the dialog must come up before another capture.
    g_capture_drain = true;
    g_pending = 0;
    g_tap_row = 0;

    // Redrawing invokes guest functions; preserve the caller's context.
    const X86Context saved = ctx;
    RedrawAll(ctx);
    ctx = saved;
}

bool RestoreBindingSnapshot(uint32_t screen) {
    if (!screen) return false;
    bool restored = false;

    if (screen == g_binding_snapshot_screen) {
        game::Opt().keys = g_saved_keys;
        game::Opt().pad = g_saved_pad;
        input::LoadBindings();
        input::LoadPadBindings();
        restored = true;
    }

    if (screen == g_section_snapshot_screen &&
        g_section_snapshot_index == 0) {
        GraphicsFields() = g_saved_graphics;
        restored = true;
    }

    // Editing applies and saves immediately. Ignore must reverse both
    // the live values and the persisted configuration.
    if (restored)
        app::Save();

    return restored;
}

}  // namespace

// fe_mouse.cpp's GetMouseInfo, each FEngine update: a touched row made current, and a binding that took its input
// shows it
bool OptionsMenuPad(int port, const input::Pad& pad) {
    if (port < 0 || port >= 4) return false;
    static std::mutex mutex;
    static uint64_t observed[4] = {};
    static bool armed[4] = {};
    std::lock_guard lock(mutex);

    const bool a = pad.analog[0] > 0x20;
    const bool b = pad.analog[1] > 0x20;
    const int kind = g_capture_kind.load();

    if (kind || g_capture_drain.load() || g_capture_request.load()) {
        if (kind == 1 && b) g_capture_cancel = true;
        armed[port] = false;
        return true;
    }

    uint64_t target = g_capture_target.load();
    const uint32_t screen = g_capture_screen.load();
    if (target &&
        (app::MenuOpen() || b || !screen ||
         InputPackage() != g_capture_package.load() ||
         Peek(screen + kScreenCurrent) != uint32_t(target))) {
        g_capture_target = 0;
        target = 0;
    }

    if (observed[port] != target) {
        observed[port] = target;
        armed[port] = false;
    }

    if (!target) return false;
    if (!a) {
        armed[port] = true;
        return false;
    }

    if (armed[port]) {
        armed[port] = false;
        uint64_t empty = 0;
        g_capture_request.compare_exchange_strong(empty, target);
    }
    return true;
}

void OptionsMenuTick(X86Context& ctx) {
    if (g_waiting || g_capture_drain.load() || g_capture_request.load())
        return;
    uint32_t current = 0, package = 0;
    if (!CaptureOwner(ctx, current, package)) return;
    FocusTouchedRow(ctx);
    TapRows(ctx);
}

void OptionsBindingFrame(X86Context& ctx) {
    struct Restore {
        X86Context& destination;
        X86Context saved;
        ~Restore() { destination = saved; }
    } restore{ctx, ctx};

    uint32_t current = 0, package = 0;
    const bool owner = CaptureOwner(ctx, current, package);
    const bool released =
        input::PressedPadInput().empty() && !input::PressedKey();

    // an input that never reads released must not hold every input off: a capture still needs A seen up first
    // (OptionsMenuPad's armed), so two seconds of drain is enough
    static unsigned drain_frames = 0;
    drain_frames = g_capture_drain.load() ? drain_frames + 1 : 0;
    if (g_capture_drain.load() && (released || drain_frames > 120))
        g_capture_drain = false;

    const uint64_t request = g_capture_request.load();
    if (request) {
        if (owner && IsBinding(RowOf(current)) &&
            request == g_capture_target.load() &&
            uint32_t(request) == current &&
            !g_waiting && !g_capture_drain.load())
            OpenRow(ctx, current);
        g_capture_request = 0;
    }

    if (g_waiting) {
        const bool cancel = !owner ||
            g_waiting_screen != g_window.screen ||
            g_capture_cancel.exchange(false);
        if (cancel) app::rows::CancelBinding();

        if (cancel || !app::Capturing() || app::rows::PollBinding()) {
            g_waiting = 0;
            g_waiting_screen = 0;
            g_capture_kind = 0;
            g_capture_drain = true;
            g_pending = 0;
            g_tap_row = 0;
            if (owner) RedrawAll(ctx);
        }
    }

    const uint32_t target =
        owner && IsBinding(RowOf(current)) && !g_waiting &&
        !g_capture_drain.load() && !app::Capturing() ? current : 0;

    g_capture_package = owner ? package : 0;
    g_capture_screen = owner ? g_window.screen : 0;
    if (uint32_t(g_capture_target.load()) != target) {
        ++g_capture_epoch;
        if (!g_capture_epoch) ++g_capture_epoch;
        g_capture_target = target
            ? (uint64_t(g_capture_epoch) << 32) | target : 0;
    }
}

}  // namespace game

using namespace game;

// GetTextureInfo(hash, default, unloaded), cdecl: our icons' hashes get our textures
X86_EXTERN_FUNC(__imp__sub_000C5900);
X86_FUNC(sub_000C5900) {
    uint32_t hash = kernel::R32(ctx.esp + 4);
    uint32_t page = hash - kIconHashes;
    if (page >= uint32_t(kIconCount)) return __imp__sub_000C5900(ctx, base);
    if (!g_textures[page]) g_textures[page] = MakeTexture(ctx, int(page));
    ctx.eax = g_textures[page];
    ctx.esp += 4;
}

// cdecl (package), called by UIOptionsMain::Setup and the main menu's after their icons: ours go after the game's,
// ANDROID OPTIONS' sections in its UIOptionsMain, ANDROID OPTIONS beside the main menu's OPTIONS. It answers the icon
// the scroller starts on, the one last picked in the package (0x128A60): ANDROID OPTIONS starts on the section left
X86_EXTERN_FUNC(__imp__sub_00128AB0);
X86_FUNC(sub_00128AB0) {
    uint32_t from = kernel::R32(ctx.esp), menu = ctx.esi, ecx = ctx.ecx;
    bool android = false;
    if (from == kAfterIcons) {
        android = g_android_next;
        g_android_next = false;
        g_android_menu = android ? menu : 0;
        if (android) AddIcons(ctx, menu, 0, kAndroidSection);
        else g_icons.clear();
    } else {
        for (const OptionsMenu& m : kOptionsMenus) {
            if (from != m.after_icons) continue;
            g_android_next = false;
            g_android_react = m.options_react;
            AddIcons(ctx, menu, kAndroidSection, kSectionCount);
            PlaceBeside(menu, m.options_vtable);
        }
    }
    ctx.ecx = ecx;
    __imp__sub_00128AB0(ctx, base);
    if (android) {
        int section = CategorySection();
        ctx.eax = (ctx.eax & ~0xFFu) | uint32_t(section >= 0 && section < kAndroidSection ? section : 0);
    }
}

// the scroller's add (option), thiscall: ANDROID OPTIONS' UIOptionsMain takes none of the game's icons, deleted as they
// come. Ours are told by their vtable: a game's icon can be new where one of ours was freed
X86_EXTERN_FUNC(__imp__sub_0012A120);
X86_FUNC(sub_0012A120) {
    uint32_t from = kernel::R32(ctx.esp), option = kernel::R32(ctx.esp + 4);
    if (!g_android_next || from <= kSetupFirst || from > kSetupLast || !option || kernel::R32(option) == g_icon_vtable)
        return __imp__sub_0012A120(ctx, base);
    uint32_t esp = ctx.esp;
    CallThis(ctx, option, kernel::R32(kernel::R32(option)), {1});  // its deleting destructor
    ctx.esp = esp + 4 + 4;
}

// UIOptionsMain's header, thiscall: the current icon's name, ours as text
X86_EXTERN_FUNC(__imp__sub_0012A030);
X86_FUNC(sub_0012A030) {
    uint32_t menu = ctx.ecx;
    __imp__sub_0012A030(ctx, base);
    uint32_t current = kernel::R32(menu + kMainCurrent);
    auto it = g_icons.find(current);
    if (it == g_icons.end() || kernel::R32(current) != g_icon_vtable) return;  // a game's icon where ours was freed
    uint32_t eax = ctx.eax;
    for (uint32_t label : kMainLabels) Print(ctx, kernel::R32(menu + label), SectionTitle(it->second));
    ctx.eax = eax;
}

// UIOptionsMain::Notification(message, ...), thiscall: Accept's fade on our category opens the options screen, as on
// the game's own (0x105029). The icon's React runs within it, so the category is read after
X86_EXTERN_FUNC(__imp__sub_00104D40);
X86_FUNC(sub_00104D40) {
    uint32_t menu = ctx.ecx;
    bool fade = kernel::R32(ctx.esp + 4) == kFadeDone && kernel::R32(menu + kMainLast) == kAccept;
    __imp__sub_00104D40(ctx, base);
    if (!fade || CategorySection() < 0) return;
    uint32_t flags = kernel::R32(kernel::R32(kFEDatabase) + 0x12C);
    bool pause = kernel::G<uint8_t>(menu + kMainInGame) && !(flags & 0x8) && !(flags & 0x40);
    uint32_t eax = ctx.eax;
    CallThis(ctx, kernel::R32(kFEng), kSwitchPackage, {pause ? kPauseOptionsFng : kOptionsFng, pause ? 1u : 0u, 0, 0});
    ctx.eax = eax;
}

// (0), thiscall, ends UIOptionsScreen::Setup: our category's rows before it
X86_EXTERN_FUNC(__imp__sub_00153080);
X86_FUNC(sub_00153080) {
    int section = kernel::R32(ctx.esp) == kAfterRows ? CategorySection() : -1;
    if (section >= 0) {
        uint32_t screen = ctx.ecx;
        AddRows(ctx, screen, section);
        ctx.ecx = screen;
    }
    __imp__sub_00153080(ctx, base);
}

// UIWidgetMenu::Notification(message, ...), thiscall: Up and Down scroll our rows through the widgets; the pad's A,
// which the options screen sends as its accept-and-leave (0x406415E3, then 0xE1FDE1D1 to go) rather than Accept,
// opens a link or a binding on our current row as well. The screen still leaves after it (its package's own script,
// not this handler, sends the leave): the editor opens over the options' icons
constexpr uint32_t kAcceptLeave = 0x406415E3;
X86_EXTERN_FUNC(__imp__sub_00155D50);
X86_FUNC(sub_00155D50) {
    uint32_t message = kernel::R32(ctx.esp + 4);
    if (ctx.ecx == g_window.screen &&
        (g_waiting || g_capture_drain.load())) {
        ctx.esp += 4 + 0x10;
        return;
    }
    if ((message == kAccept || message == kAcceptLeave) && ctx.ecx == g_window.screen && CategorySection() >= 0) {
        uint32_t ecx = ctx.ecx;
        if (IsBinding(RowOf(kernel::R32(ecx + kScreenCurrent)))) {
            ctx.esp += 4 + 0x10;
            return;
        }
        if (OpenRow(ctx, kernel::R32(ecx + kScreenCurrent))) {
            ctx.esp += 4 + 0x10;
            return;
        }
        ctx.ecx = ecx;
    }
    if ((message == kUp || message == kDown) && ctx.ecx == g_window.screen && CategorySection() >= 0) {
        uint32_t ecx = ctx.ecx;
        if (Scroll(ctx, ecx, message == kDown ? 1 : -1)) {
            ctx.esp += 4 + 0x10;
            return;
        }
        ctx.ecx = ecx;
    }
    __imp__sub_00155D50(ctx, base);
}

// UIOptionsScreen's defaults (restore), thiscall: on our section, its pages' defaults, as the panel's Defaults; the
// bindings for the controls, not the touch controls (the panel's touch page restores every setting)
X86_EXTERN_FUNC(__imp__sub_000EE570);
X86_FUNC(sub_000EE570) {
    if (CategorySection() < 0 || ctx.ecx != g_window.screen)
        return __imp__sub_000EE570(ctx, base);

    // thiscall with one argument.
    ctx.esp += 8;
}

// SwitchPackage (name, arg, 0, 0), thiscall: an options screen of a section leaving for the options (0x105479) comes
// back to ANDROID OPTIONS' icons
X86_EXTERN_FUNC(__imp__sub_000EB160);
X86_FUNC(sub_000EB160) {
    int section = kernel::R32(ctx.esp) == kScreenBack ? CategorySection() : -1;
    if (section >= 0 && section < kAndroidSection) g_android_next = true;
    if (section >= 0 &&
        kernel::R32(ctx.esp) == kScreenBack) {
        g_section_snapshot_screen = 0;
        g_section_snapshot_index = -1;
        g_saved_section_values.clear();
        g_binding_snapshot_screen = 0;
        g_saved_keys.clear();
        g_saved_pad.clear();
    }
    __imp__sub_000EB160(ctx, base);
}


// The stock Ignore path restores the screen's entry-state settings.
// Our synthetic categories need the host binding maps restored too.
X86_EXTERN_FUNC(__imp__sub_000DCF30);
X86_FUNC(sub_000DCF30) {
    const uint32_t screen = ctx.ecx;
    if (CategorySection() < 0 || screen != g_window.screen)
        return __imp__sub_000DCF30(ctx, base);

    if (RestoreBindingSnapshot(screen))
        FinishBindingDialog(ctx);

    // No arguments; the stock switch has no case for our categories.
    ctx.esp += 4;
}

// The confirmed Defaults path calls E5350, not EE570.
X86_EXTERN_FUNC(__imp__sub_000E5350);
X86_FUNC(sub_000E5350) {
    const int section = CategorySection();
    if (section < 0 || ctx.ecx != g_window.screen)
        return __imp__sub_000E5350(ctx, base);

    app::rows::CancelBinding();

    // Preserve the original custom-menu defaults policy:
    // reset the section's pages, but leave the touch layout alone.
    for (int page : kSections[section].pages)
        if (page != 1)
            app::rows::Defaults(page);

    // Do not replace the entry snapshot: Defaults can still be ignored.
    FinishBindingDialog(ctx);

    // No arguments.
    ctx.esp += 4;
}


// UIOptionsScreen's comparison against its entry-state settings.
// Stock categories retain their original implementation.
X86_EXTERN_FUNC(__imp__sub_000DCE60);
X86_FUNC(sub_000DCE60) {
    const int section = CategorySection();
    const uint32_t screen = ctx.ecx;

    if (section < 0 || section >= kAndroidSection ||
        screen != g_window.screen ||
        screen != g_section_snapshot_screen ||
        section != g_section_snapshot_index)
        return __imp__sub_000DCE60(ctx, base);

    const bool unchanged = SectionUnchanged(screen, section);
    ctx.eax = (ctx.eax & 0xFFFFFF00u) | (unchanged ? 1u : 0u);

    // thiscall, no arguments.
    ctx.esp += 4;
}
