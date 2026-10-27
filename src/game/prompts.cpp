// The game's prompts to press a pad button, said for the touch screen. Its text is looked up by hash in the language's
// string table (chunk 0x39000, loaded by 0x131DF0: the pairs at [0x48EDC4], their count at [0x48EDBC]); every lookup,
// GetLocalizedString (0x124C40) and the copies into a buffer (0x131DB0, ...), goes through SearchForString (0x124B50,
// the hash in edx), which answers with the string's text, Latin-1 bytes. The hashes are the same in every language.
#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
namespace {

// in game::kLanguages order: English, French, German, Italian, Spanish, Indonesian (which plays the English table)
struct Prompt {
    uint32_t hash;
    bool front_end;  // only out of a race: a word a race's prompt may use too
    const char* text[6];
};
const Prompt kPrompts[] = {
    // the title screen's; its whole screen is a mouse region (src/game/fe_mouse.cpp), so a tap anywhere is START
    {0x9B580A55, false,  // "Press START"
     {"Tap anywhere", "Touchez l'\xE9" "cran", "Bildschirm ber\xFC" "hren", "Tocca lo schermo", "Toca la pantalla",
      "Ketuk di mana saja"}},
    {0xDC64C04C, false,  // "Press START button"
     {"Tap anywhere", "Touchez l'\xE9" "cran", "Bildschirm ber\xFC" "hren", "Tocca lo schermo", "Toca la pantalla",
      "Ketuk di mana saja"}},
    {0xD18D4C4C, false,  // "Press START to begin"
     {"Tap anywhere to begin", "Touchez l'\xE9" "cran pour commencer", "Zum Starten Bildschirm ber\xFC" "hren",
      "Tocca lo schermo per iniziare", "Toca la pantalla para empezar", "Ketuk di mana saja untuk mulai"}},
    // a tutorial's "Press (A) To Continue", three objects: a tap anywhere is its A (fe_mouse.cpp's g_tap_anywhere)
    {0x9D8F2513, true,  // "Press"
     {"Tap or press", "Touchez ou appuyez sur", "Tippe oder dr\xFC" "cke", "Tocca o premi", "Toca o pulsa",
      "Ketuk atau tekan"}},
    {0xD0258C0D, true,  // "To Continue"
     {nullptr, nullptr, nullptr, nullptr, nullptr, "untuk lanjut"}},
};

// Indonesian has no table on the disc: the English table's strings translated, answered by hash; a string not here
// (names, cars, places, brands) stays English
struct Translation {
    uint32_t hash;
    const char* text;
};
constexpr Translation kIndonesian[] = {
#include "strings_id.inc"
};
constexpr int kIndonesianIndex = 5;  // in game::kLanguages

const char* Indonesian(uint32_t hash) {
    auto at = std::lower_bound(std::begin(kIndonesian), std::end(kIndonesian), hash,
                               [](const Translation& t, uint32_t h) { return t.hash < h; });
    return at != std::end(kIndonesian) && at->hash == hash ? at->text : nullptr;
}

int LanguageIndex() {
    static const int index = [] {
        for (int i = 0; i < int(std::size(kLanguages)); i++)
            if (BootLanguage() == kLanguages[i]) return i;
        return 0;
    }();
    return index;
}

// each text copied into guest memory once
uint32_t GuestText(const char* text) {
    static std::mutex lock;
    static std::unordered_map<const char*, uint32_t> copies;
    std::lock_guard guard(lock);
    uint32_t& va = copies[text];
    if (!va) {
        size_t n = std::strlen(text) + 1;
        va = mem::AllocSystem(uint32_t(n));
        if (va) std::memcpy(kernel::HostPtr(va), text, n);
    }
    return va;
}

// strings of the recomp's own (the options' headers, options_menu.cpp), by hashes the table has not
std::mutex g_own_lock;
std::unordered_map<uint32_t, std::string> g_own;

const char* OwnString(uint32_t hash) {
    std::lock_guard guard(g_own_lock);
    auto it = g_own.find(hash);
    return it == g_own.end() ? nullptr : it->second.c_str();
}

}  // namespace

// game thread: a string the table answers for hash, Latin-1; set once (GuestText keeps the text it was given)
void SetOwnString(uint32_t hash, const std::string& text) {
    std::lock_guard guard(g_own_lock);
    g_own.emplace(hash, text);
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_00124B50);
X86_FUNC(sub_00124B50) {
    uint32_t hash = ctx.edx;
    const char* text = game::OwnString(hash);
    for (const game::Prompt& p : game::kPrompts) {
        if (text) break;
        if (p.hash != hash) continue;
        if (!p.front_end || !game::Racing()) text = p.text[game::LanguageIndex()];
        break;
    }
    if (!text && game::LanguageIndex() == game::kIndonesianIndex) text = game::Indonesian(hash);
    if (text) {
        if (uint32_t va = game::GuestText(text)) {
            ctx.eax = va;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_00124B50(ctx, base);
}
