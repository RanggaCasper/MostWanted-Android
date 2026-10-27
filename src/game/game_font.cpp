// The game's body font (FONT_MW_BODY, the options' rows and most of the menus) for the ImGui panel. cFEng keeps its
// loaded fonts in a list at 0x48F2F0 (FindFont 0x137A40): a node's TextureInfo at +8, its FNTX resource at +0xC, its
// name's bStringHash at +0x14. The resource ('FNTX'): glyph count u16 +0xA, flags +0xC (0x40000: 16 byte glyphs),
// glyph table offset +0x14, line height +0x24. A glyph: Unicode u16 +0, width u8 +2, height u8 +3, its place in the
// texture u16 +4 / +6, x offset s8 +9, y offset from the line's top s8 +0xA, advance s16 +0xE (0x137EB0, 0x123620).
// The texture is 256x256 DXT3, linear (NV2A format 0x0E; the D3D texture at the TextureInfo's platform record +0x10).
#include <cstring>
#include <mutex>

#include "common/game_font.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {

constexpr uint32_t kFontList = 0x48F2F0, kFontHash = 0x545570C6;  // bStringHash("FONT_MW_BODY")
constexpr uint32_t kFntx = 0x58544E46, kGlyphs16 = 0x40000, kFormatDxt3 = 0x0E;

std::mutex g_lock;
FontImage g_font;
bool g_ready = false, g_taken = false, g_tried = false;

bool Readable(uint32_t va, uint32_t n) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + n - 1); }

// DXT3 to alpha alone: the glyphs are white, their alpha the explicit 4 bits a texel
std::vector<uint8_t> DecodeAlpha(const uint8_t* data, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(size_t(w) * h);
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++, data += 16) {
            uint64_t alpha;
            std::memcpy(&alpha, data, 8);
            for (uint32_t k = 0; k < 16; k++)
                out[size_t(by * 4 + k / 4) * w + bx * 4 + k % 4] = uint8_t(((alpha >> (4 * k)) & 15) * 17);
        }
    return out;
}

}  // namespace

void CaptureFont() {
    if (g_tried || !Readable(kFontList, 4)) return;
    uint32_t node = kernel::R32(kFontList);
    for (int k = 0; node && node != kFontList && k < 64; node = kernel::R32(node), k++)
        if (Readable(node, 0x18) && kernel::R32(node + 0x14) == kFontHash) break;
    if (!node || node == kFontList || !Readable(node, 0x18)) return;  // not loaded yet
    g_tried = true;
    uint32_t res = kernel::R32(node + 0xC), info = kernel::R32(node + 8);
    if (!Readable(res, 0x28) || kernel::R32(res) != kFntx || !(kernel::R32(res + 0xC) & kGlyphs16) ||
        !Readable(info, 4)) {
        LOGW("font: FONT_MW_BODY is not a font this reads");
        return;
    }
    uint32_t plat = kernel::R32(info), d3d = plat + 0x10;
    if (!Readable(plat, 0x20)) return;
    uint32_t format = kernel::R32(d3d + 12);
    uint32_t w = 1u << ((format >> 20) & 15), h = 1u << ((format >> 24) & 15);
    uint32_t data = mem::VirtualFromPhysical(kernel::R32(d3d + 4));
    if (((format >> 8) & 0xFF) != kFormatDxt3 || w < 4 || h < 4 || !Readable(data, w * h)) {
        LOGW("font: FONT_MW_BODY's texture is format %08X, not DXT3", format);
        return;
    }
    std::vector<uint8_t> alpha = DecodeAlpha(static_cast<const uint8_t*>(kernel::HostPtr(data)), w, h);
    FontImage font;
    font.line_height = int(kernel::R32(res + 0x24));
    uint32_t count = kernel::R32(res + 8) >> 16, table = res + kernel::R32(res + 0x14);
    if (!Readable(table, count * 16)) return;
    const uint8_t* g = static_cast<const uint8_t*>(kernel::HostPtr(table));
    for (uint32_t i = 0; i < count; i++, g += 16) {
        FontGlyph glyph;
        uint16_t code, x, y;
        int16_t advance;
        std::memcpy(&code, g, 2);
        std::memcpy(&x, g + 4, 2);
        std::memcpy(&y, g + 6, 2);
        std::memcpy(&advance, g + 0xE, 2);
        glyph.code = code;
        glyph.w = g[2];
        glyph.h = g[3];
        glyph.x_off = int8_t(g[9]);
        glyph.y_off = int8_t(g[0xA]);
        glyph.advance = advance;
        if (x + uint32_t(glyph.w) > w || y + uint32_t(glyph.h) > h) continue;
        glyph.alpha.resize(size_t(glyph.w) * glyph.h);
        for (int row = 0; row < glyph.h; row++)
            std::memcpy(glyph.alpha.data() + size_t(row) * glyph.w, alpha.data() + size_t(y + row) * w + x, size_t(glyph.w));
        font.glyphs.push_back(std::move(glyph));
    }
    LOGI("font: FONT_MW_BODY, %zu glyphs, %d high", font.glyphs.size(), font.line_height);
    std::lock_guard guard(g_lock);
    g_font = std::move(font);
    g_ready = true;
}

bool TakeFont(FontImage& out) {
    std::lock_guard guard(g_lock);
    if (!g_ready || g_taken) return false;
    g_taken = true;
    out = std::move(g_font);
    return true;
}

}  // namespace game
