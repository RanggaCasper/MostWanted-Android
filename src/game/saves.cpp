// A profile keeps the hash of the car list the game was built with: at boot FEDatabase hashes a fresh profile's
// FEPlayerCarDB (0x157180: +0x414, 0x8CC8 bytes, 0x4E6A0 seeded -1) into its +8, every save stores it (data +0x5A09, before
// the profile's name), and loading one whose hash differs fails (0x14768B), the game calling the profile damaged. Add-on
// cars are in that list, so each change of the Mods Loader's cars turned every save away: a career made before the mods
// came, or with others, could not be played (0xAE7B6735 the disc's alone, 0x115DD05D and 0x447841F2 two sets of mods).
// The cars a profile lists are put right after it loads (car_mods.cpp: an add-on new to it is added, one taken out goes),
// so the hash asks nothing here that is not answered: the save's own is taken as the game's before it is compared
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {
constexpr uint32_t kFeDatabase = 0x48EDD4, kCarListHash = 8;
constexpr uint32_t kSavedHash = 0x10 + 0x59F9;  // past the save's settings, both ways 0x147600 reads them
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00147600);
X86_FUNC(sub_00147600) {  // thiscall (data, size, first load, player): UserProfile from a save, true if it took it
    uint32_t data = kernel::R32(ctx.esp + 4), size = kernel::R32(ctx.esp + 8);
    uint32_t db = kernel::R32(kFeDatabase);
    if (db && size >= kSavedHash + 4 && mem::IsCommitted(data + kSavedHash + 3)) {
        uint32_t saved = kernel::R32(data + kSavedHash), ours = kernel::R32(db + kCarListHash);
        if (saved != ours) {
            LOGR("saves: a profile from another car list (%08X, the game's %08X) loaded all the same", saved, ours);
            kernel::W32(db + kCarListHash, saved);
        }
    }
    __imp__sub_00147600(ctx, base);
}
