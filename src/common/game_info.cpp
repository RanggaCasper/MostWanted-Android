#include "common/fe_mouse.h"
#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"

namespace game {

namespace {

// port 0 has the touch controls, the keyboard and mouse; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    const bool panel = app::MenuOpen();
    if (!panel && port == 0) input::Keyboard(pad);
    const bool consumed = OptionsMenuPad(port, pad);
    if (panel || consumed)
        pad = input::Pad{.connected = pad.connected};
}

// no vibration while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    // Settings from before version 4 stored FULL (0) as the default. Move that old default to QUARTER;
    // explicit half, quarter and off choices are kept.
    if (t["defaults"].value_or(int64_t(0)) < 4 && t["car_reflections"].value_or(int64_t(2)) == 0)
        o.reflections = 2;
    std::string language = t["language"].value_or(o.language);
    // a language no longer offered (French, German, Italian, Spanish) goes back to English
    if (KnownLanguage(language) && LanguageAvailable(language)) o.language = language;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
    if (const toml::table* pad = t["pad"].as_table())
        for (auto&& [id, value] : *pad)
            if (auto v = value.value<std::string>()) o.pad[std::string(id.str())] = *v;
}

// after the settings load, before boot
void Init() {
    BootLanguage();  // latched before the options can change the setting
    input::LoadBindings();
    input::LoadPadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
    toml::table pad;
    for (const auto& [id, value] : o.pad) pad.insert(id, value);
    t.insert("pad", pad);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
    return language;
}

}  // namespace game

const app::GameInfo app::kGame = {
    .title = L"Need for Speed: Most Wanted",
    .short_title = L"Most Wanted",
    .name = L"MostWanted",
    .log = "mostwanted",
    .settings = "mostwanted.toml",
    .splash = nullptr,
    .missing = L"Put the game in the MostWanted folder on the phone's storage: its disc image (.iso), or default.xbe "
               L"with the disc's NFS folder beside it.",
    .images = {{"default.xbe", "MostWanted"}},
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"bloom", &game::Opt().bloom, nullptr},
                {"fog", &game::Opt().fog, nullptr},
                {"car_reflections", nullptr, &game::Opt().reflections},
                {"particles", &game::Opt().particles, nullptr},
                {"shadows", &game::Opt().shadows, nullptr},
                {"road_reflections", &game::Opt().road_reflections, nullptr},
                {"rain", &game::Opt().rain, nullptr},
                {"rain_drops", &game::Opt().rain_drops, nullptr},
                {"yellow_tint", &game::Opt().yellow_tint, nullptr},
                {"motion_blur", &game::Opt().motion_blur, nullptr},
                {"shader_preload", &game::Opt().shader_preload, nullptr},
                {"cheat_nos", &game::Opt().cheat_nos, nullptr},
                {"cheat_speedbreaker", &game::Opt().cheat_speedbreaker, nullptr},
                {"cheat_cash", &game::Opt().cheat_cash, nullptr},
                {"cheat_no_police", &game::Opt().cheat_no_police, nullptr},
                {"cheat_unlock_all", &game::Opt().cheat_unlock_all, nullptr},
                {"skip_movies", &game::Opt().skip_movies, nullptr},
                {"fps_usage", &game::Opt().fps_usage, nullptr},
                {"fps_temps", &game::Opt().fps_temps, nullptr},
                {"fps_position", nullptr, &game::Opt().fps_position},
                {"audio_output", nullptr, &game::Opt().audio_output},
                {"aspect_ratio", nullptr, &game::Opt().aspect_ratio},
                {"screen_filter", nullptr, &game::Opt().screen_filter},
                {"anti_aliasing", nullptr, &game::Opt().anti_aliasing}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
