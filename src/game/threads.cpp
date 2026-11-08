// The console had one core: the game's threads took turns, and state they share with no lock was only ever caught
// half written when the scheduler cut in between two instructions. Here the main thread and the file system's three
// job threads (guest 1C, 28 and 34, EA's queue at 0x2BD01F) run at once, and the crashes in the issues were all where
// that state is: chunks of garbage walked by the loader (0x1C0460, its assert 0x1C0130, 0x1B9620), the slot pools'
// free lists holding path text ("soun", "TRAC", "LANG" taken for a pointer at 0x54DC0), the string pool torn
// (0x51930), and the heap check failing (int3 at 0x54281, from 0x240690). The places those threads meet are locked
// here, the rest left as the game wrote it.
#include <mutex>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

// Reads: a file's read seeks its handle and then reads from where the handle is (ReadFile with no OVERLAPPED,
// 0x2BE83E; SetFilePointer, 0x2BE8A3), two calls. The ZDIR files (0x1BA800, a ZZDATA archive's part) seek and read
// the archive's one handle, which every job thread and the main thread share: another thread's seek in between and
// the read took another file's bytes, the loader's chunks garbage. The file system's own jobs do the same on any
// handle (read 0x2BE225 and 0x2BE32C, write 0x2BE433). One lock over each seek and its transfer: the reads are the
// disc's, one at a time anyway
std::recursive_mutex g_io;

// Slot pools (EA's bSlotPool: free list +0x10, count +0x18): no lock of their own. The file handles' (0x49362C) are
// taken by 0x1C0D50 on whichever thread opens a file, and two threads popping the list at once got one slot both;
// its path then written over the list's next pointer. Allocation (0x54DC0, 0x54E20), free (0x53570, and 0x55220,
// the 16 byte pool's own) and teardown (0x54EF0) take one lock. A pool that grows takes bMalloc's section inside
// it, and nothing holding a heap's section frees to a pool, so the two never wait on each other
std::recursive_mutex g_pools;

}  // namespace

#define LOCKED(lock, addr)                                   \
    X86_EXTERN_FUNC(__imp__sub_##addr);                      \
    X86_FUNC(sub_##addr) {                                   \
        std::lock_guard<std::recursive_mutex> hold(lock);    \
        __imp__sub_##addr(ctx, base);                        \
    }

LOCKED(g_io, 001BA800)
LOCKED(g_io, 002BE225)
LOCKED(g_io, 002BE32C)
LOCKED(g_io, 002BE433)

LOCKED(g_pools, 00054DC0)
LOCKED(g_pools, 00054E20)
LOCKED(g_pools, 00053570)
LOCKED(g_pools, 00055220)
LOCKED(g_pools, 00054EF0)

#undef LOCKED

// The streams' level (0x2D6D40, cdecl: a mask of channels, a level 0 to 100; the music's, called by EA Trax's player
// 0x0B47A0): under the sound system's lock (0x2D2EA0, 0x2D2ED0) each group (0x2D7A60, which makes it current at
// 0x4E3AFC) has 24 channels from +0x40, and a playing one ([+0x1A] negative) has its stream at +0x12C told the level
// (vtable +0x30, thiscall, the channel's own level +3 times it / 100). A channel playing with no stream yet crashed
// on [0] (after a boss race, with the markers); here the level is kept for it (+2) and the call is skipped
X86_FUNC(sub_002D6D40) {
    using kernel::R32;
    uint32_t mask = R32(ctx.esp + 4);
    int8_t level = int8_t(kernel::G<uint8_t>(ctx.esp + 8));
    ctx.esp += 4;
    if (!(kernel::CallGuest(ctx, 0x2D2EA0, {}) & 0xFF)) {
        ctx.eax = 0xFFFFFFEE;
        return;
    }
    if (level > 100) level = 100;
    if (level < 0) level = 0;
    if (mask == 0xFFFFFFFF) kernel::G<uint8_t>(0x46FCA8) = uint8_t(level);
    uint32_t result = 0xFFFFFFF8;
    for (uint32_t group = 0; group < 4; group++) {
        if (!kernel::CallGuest(ctx, 0x2D7A60, {group, mask})) continue;
        for (uint32_t i = 0; i < 24; i++) {
            uint32_t ch = R32(R32(0x4E3AFC) + 0x40 + i * 4);
            if (!ch || !(mask & (1u << i))) continue;
            kernel::G<uint8_t>(ch + 2) = uint8_t(level);
            result = 0;
            if (int16_t(kernel::R16(ch + 0x1A)) >= 0) continue;
            uint32_t stream = R32(ch + 0x12C);
            if (!stream) continue;
            int32_t v = int32_t(int8_t(kernel::G<uint8_t>(ch + 3))) * level / 100;
            uint32_t ecx = ctx.ecx;
            ctx.ecx = stream;
            kernel::CallGuest(ctx, R32(R32(stream) + 0x30), {uint32_t(v)});
            ctx.ecx = ecx;
        }
    }
    kernel::CallGuest(ctx, 0x2D2ED0, {});
    ctx.eax = result;
}
