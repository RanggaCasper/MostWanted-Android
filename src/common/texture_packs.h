// Texture packs (the PC's NextGenGraphics TexturePackInfo.json packs, converted by the Mods Loader's TexturePacks.java into
// MostWanted/mods/textures/<pack>/<hash>.dds): a game texture whose name's hash a pack has is drawn with the pack's image.
// The game thread finds where each such texture's data is (GetTextureInfo, src/game/texture_packs.cpp); native/vulkan.cpp
// binds the pack's image for a texture at that address, its pixels never in guest memory (128 MB would not hold them).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace texpacks {

// a texture the game has loaded that a pack replaces, by its data's guest virtual address
struct Bound {
    uint32_t hash = 0;
    uint32_t width = 0, height = 0;  // the game's own, so a reused address with another texture in it is not taken
};

void Load(const std::string& dir);          // native/texture_packs.cpp, at boot: every pack in dir, a later pack's over an earlier's
const std::vector<uint32_t>& Hashes();      // what the packs replace, after Load
std::string PathOf(uint32_t hash);          // the image's file, empty if none
void Publish(std::unordered_map<uint32_t, Bound>&& bound);  // game thread: where they are now
uint64_t Generation();                       // bumped by each Publish that changed anything
void Snapshot(std::unordered_map<uint32_t, Bound>& out, uint64_t& generation);

}  // namespace texpacks
