#pragma once
#include <cstdint>
#include <vector>

// The game's own font for the ImGui panel: FONT_MW_BODY's glyphs, read out of the game's memory once it is loaded
// (src/game/game_font.cpp) and laid into ImGui's atlas by the renderer (native/ui.cpp)
namespace game {

struct FontGlyph {
    uint32_t code;  // Unicode
    int w, h;
    int x_off, y_off;  // from the line's top left
    int advance;
    std::vector<uint8_t> alpha;  // w * h, rows top down
};

struct FontImage {
    int line_height;
    std::vector<FontGlyph> glyphs;
};

void CaptureFont();             // game thread, at each present: once the font is there, its glyphs
bool TakeFont(FontImage& out);  // render thread: the glyphs, once, after CaptureFont has them

}  // namespace game
