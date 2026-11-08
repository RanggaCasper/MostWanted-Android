// Where the textures the packs replace are (src/common/texture_packs.h). GetTextureInfo (0xC5900, cdecl: hash, default,
// unloaded) with no default is a lookup and nothing more: the loaded packs (the list at 0x4830A8, each searched by
// 0xC5380 behind the hash filter at 0x483384), then the table at 0x47FA00; 0 when none has it. A TextureInfo's first field is
// its platform info, whose +0x10 is the D3D texture the draws bind. The track streams its packs in and out as the car goes,
// an address freed and taken by another texture: asked again every few frames, and the size kept to tell them apart.
#include <unordered_map>

#include "common/options.h"
#include "common/texture_packs.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {
constexpr uint32_t kGetTextureInfo = 0xC5900, kPlatD3D = 0x10;
constexpr uint32_t kEvery = 8;  // presents
}  // namespace

void TexturePacksFrame(X86Context& ctx) {
    const std::vector<uint32_t>& hashes = texpacks::Hashes();
    static uint32_t frames = 0;
    if (hashes.empty() || frames++ % kEvery) return;
    std::unordered_map<uint32_t, texpacks::Bound> bound;
    // TEXTURE PACKS off: none found, and native/vulkan.cpp binds the game's own again at the next draw
    if (!Opt().texture_packs) {
        texpacks::Publish(std::move(bound));
        return;
    }
    uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    for (uint32_t hash : hashes) {
        uint32_t info = kernel::CallGuest(ctx, kGetTextureInfo, {hash, 0, 0});
        if (!info || !mem::IsCommitted(info)) continue;
        uint32_t plat = kernel::R32(info);
        if (!plat || !mem::IsCommitted(plat + kPlatD3D + 0x14)) continue;
        gpu::TextureDesc d;
        if (!gpu::Describe(plat + kPlatD3D, d) || !d.data || d.cube || d.depth != 1 || d.linear) continue;
        bound[d.data] = {hash, d.width, d.height};
    }
    ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4],
    ctx.edi = saved[5], ctx.ebp = saved[6];
    // the count as the track streams, every 10 s at most (75 times kEvery presents at 60 fps)
    static size_t shown = ~size_t(0);
    static uint32_t quiet = 0;
    if (bound.size() != shown && quiet == 0) {
        shown = bound.size();
        quiet = 75;
        LOGR("texpacks: %zu of %zu replaced textures loaded in the game", shown, hashes.size());
    }
    if (quiet) quiet--;
    texpacks::Publish(std::move(bound));
}

}  // namespace game
