// Need for Speed: Most Wanted (Black Edition) needs no patch of its code to boot: the XDK it calls is all on the host.
// Its own hooks go here, [[hook]] entries in config/game.toml and X86_FUNC replacements of recompiled functions.
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

// The console's widescreen flag (XGetVideoFlags at boot, 0x0D0202, kept at 0x48099C; set from 3:2 on, native/main.cpp)
// gives the front end's and HUD's screen transform (0x0D4888, 0x0D8330: 576 / width across, 448 / 480 down) a frame
// 853 wide (480 * 16 / 9, at 0x427C00) for a 4:3 one's 640. It is the frame's own shape here, so 19.5:9 is not 16:9
// stretched
constexpr uint32_t kVideoFlags = 0x48099C, kWideWidth = 0x427C00;
void WidenAspect() {
    if (!(kernel::G<uint8_t>(kVideoFlags) & 1)) return;
    float w = 480.0f * Aspect();
    uint32_t bits;
    std::memcpy(&bits, &w, 4);
    kernel::W32(kWideWidth, bits);
}

// The 3D views' projection (0x0C7740, thiscall, the view's matrices in ecx, the eView first of 4 arguments): the
// camera's horizontal field of view ([[view + 0x40] + 0xC4], 16 bit angle) gives the x scale, cot of its half, at
// +0x40; the vertical is that angle times 9/16 with the widescreen flag (3/4 without), its scale at +0x54. A frame
// wider than 16:9 was the 16:9 picture stretched across it. The x scale is made the frame's, keeping the vertical
// (more of the road on the sides, not less above and below), before the view's matrix times it (0x2B91C4, at
// 0x0C7A04) gives the view projection that the frustum's planes are cut from, so what is culled widens with it.
// Only the views drawn over the frame: the players' (1, 2), their road reflections (4, 5) and glows (6, 7), the
// ones the game gives the widescreen angle; not the mirror's or the environment cube's faces
constexpr uint32_t kViewType = 0x4, kProjection = 0x40;
uint32_t g_widen_view = 0;  // the view's matrices while its projection is made, game thread
bool WideView(uint32_t type) { return type == 1 || type == 2 || (type >= 4 && type <= 7); }
}  // namespace game

X86_EXTERN_FUNC(__imp__sub_000C7740);
X86_FUNC(sub_000C7740) {
    uint32_t view = kernel::R32(ctx.esp + 4);
    bool wide = (kernel::G<uint8_t>(game::kVideoFlags) & 1) && view && game::WideView(kernel::R32(view + game::kViewType));
    game::g_widen_view = wide ? ctx.ecx : 0;
    __imp__sub_000C7740(ctx, base);
    game::g_widen_view = 0;
}

// the matrix product (out, a, b; cdecl): for the view being made, its projection is widened just before
X86_EXTERN_FUNC(__imp__sub_002B91C4);
X86_FUNC(sub_002B91C4) {
    uint32_t m = game::g_widen_view;
    if (m && kernel::R32(ctx.esp + 8) == m && kernel::R32(ctx.esp + 12) == m + game::kProjection) {
        game::g_widen_view = 0;
        float& x = kernel::G<float>(m + game::kProjection);
        x *= (16.0f / 9.0f) / game::Aspect();
    }
    __imp__sub_002B91C4(ctx, base);
}

namespace game {

// Switches the game reads and never writes, all 1 on the disc:
// - the cars' reflection is a cube drawn around the camera's car each frame, the world six times more (views 0x12 on,
//   0x0D7AD5), only while the byte at 0x4279D6 is set, one of a row of render switches at 0x4279CC. Off,
//   native/vulkan.cpp has the cars read a black cube, not the last one drawn
// - the particle system at 0x482CC0 (tyre smoke, sparks, crash debris) is drawn by 0x0CE8B0 (15 to 25 draws a frame)
//   only while the byte at 0x4279CF is set. Not its master switch, 0x434130: at 0 nothing spawns, and loading a race
//   with it off corrupted the heap (a crash in the allocator 0x054DC0 under 0x1C0D50), so the particles still run
// - a car's render (0x255490) draws the shadow under it, the CARSHADOW texture (0x24C0F0) and a second layer
//   (0x24BB90), only while the dword at 0x469910 is set
// - the wet road's reflection, the world drawn upside down into views 4 and 5 (0x0D787B), only while the dword at
//   0x427A3C is set. The road still samples view 4's target, [[view + 0x60] + 8] (0x0C0F90), so native/vulkan.cpp
//   gives it black when off, not the last one drawn
constexpr uint32_t kEnvMapSwitch = 0x4279D6, kParticleDrawSwitch = 0x4279CF, kCarShadowSwitch = 0x469910;
constexpr uint32_t kRoadReflectionSwitch = 0x427A3C, kRoadReflectionView = 0x48AE40;
constexpr uint32_t kEnvMapFace0Target = 0x48B4C0;  // view 0x12's [view + 0x60], the faces' views 0x70 apart

// the colour data of a view's target, [target + 8]'s, as a texture's address reads
uint32_t TargetData(uint32_t target) {
    uint32_t surface = target ? kernel::R32(target + 8) : 0;
    return surface ? mem::VirtualFromPhysical(kernel::R32(surface + 4)) : 0;
}
void ApplyGraphicsSwitches() {
    // HALF and QUARTER draw the cube every 2nd or 4th frame; between, the cars read the faces drawn last. OFF still
    // draws it every 512th: a cube never drawn leaves the paint white, whatever native/vulkan.cpp binds for it. Out of a
    // race and in its first frames it is drawn every frame: a race's cube is new, and its cars stayed white until the
    // count came round
    constexpr uint32_t kRaceWarmup = 120;
    static uint32_t frame = 0, racing = 0;
    racing = Racing() ? racing + (racing < kRaceWarmup) : 0;
    uint32_t rate = Opt().reflections;
    uint32_t every = racing < kRaceWarmup ? 1 : rate < 3 ? 1u << rate : 512;
    bool envmap = frame++ % every == 0;
    kernel::G<uint8_t>(kEnvMapSwitch) = envmap ? 1 : 0;
    kernel::G<uint8_t>(kParticleDrawSwitch) = Opt().particles ? 1 : 0;
    kernel::W32(kCarShadowSwitch, Opt().shadows ? 1 : 0);
    kernel::W32(kRoadReflectionSwitch, Opt().road_reflections ? 1 : 0);
    g_road_reflection = TargetData(kernel::R32(kRoadReflectionView + 0x60));
    // the cube is its six faces' memory, the first face's at its start
    g_env_cube = TargetData(kernel::R32(kEnvMapFace0Target));
}

// Cheats:
// - the nitrous' drain (0x1E6A9A, an engine's NOS, [engine + 0xFC], less the flow) is 0 while the byte at 0x49A61C is
//   set, and the speedbreaker's (0x20E663, LocalPlayer +0x84 less the time) while the one at 0x4B9694 is: debug
//   switches the game reads there alone and never writes, 0 on the disc. Every car's nitrous, the racers' too
// - the career's cash is CareerSettings::CurrentCash, [[0x48EDD4] + 0x10] + 0xB4: the FEDatabase's first profile, as
//   the CASH_DATA string prints it (0x0F5425)
// - the cop manager ([0x494E40], ICopMgr at AICopManager +0x54) spawns no cop and starts no pursuit while its byte at
//   +0x60 is set (VehicleSpawningEnabled, 0x01A370): what the scripts' NoNewPursuitsOrCops sets (0x0130E0). The game
//   clears it again on a reset (0x01A9CD), so it is set at each present; off gives back the value it had
// - UnlockAllThings, the byte at 0x493D00 (SkipFE at 0x493C40 + 0xC0, as on the PC), is or'ed into every unlock test
//   (cars, parts, races: 0x12C0E0, 0x13B2DB, ...). The game sets it itself for a mode of its menus (0x1049FA), so off
//   gives back the value it had too
constexpr uint32_t kInfiniteNos = 0x49A61C, kInfiniteSpeedbreaker = 0x4B9694;
constexpr uint32_t kFeDatabase = 0x48EDD4, kProfileCash = 0xB4, kCheatCash = 9999999;
constexpr uint32_t kCopManager = 0x494E40, kNoNewCops = 0x60;
constexpr uint32_t kUnlockAllThings = 0x493D00;

// a game byte a cheat holds at 1 while on, given back as it was when it goes off or its object is made again
struct HeldByte {
    uint32_t held = 0;
    uint8_t before = 0;
    void Apply(bool on, uint32_t addr) {
        if (held && held != addr) held = 0;
        if (on && addr) {
            if (!held) {
                held = addr;
                before = kernel::G<uint8_t>(addr);
            }
            kernel::G<uint8_t>(addr) = 1;
        } else if (held) {
            kernel::G<uint8_t>(held) = before;
            held = 0;
        }
    }
};

void ApplyCheats() {
    const Options& o = Opt();
    kernel::G<uint8_t>(kInfiniteNos) = o.cheat_nos ? 1 : 0;
    kernel::G<uint8_t>(kInfiniteSpeedbreaker) = o.cheat_speedbreaker ? 1 : 0;
    if (o.cheat_cash) {
        uint32_t db = kernel::R32(kFeDatabase);
        uint32_t profile = db && mem::IsCommitted(db + 0x10) ? kernel::R32(db + 0x10) : 0;
        if (profile && mem::IsCommitted(profile + kProfileCash)) kernel::W32(profile + kProfileCash, kCheatCash);
    }
    static HeldByte no_cops, unlock_all;
    uint32_t mgr = kernel::R32(kCopManager);
    no_cops.Apply(o.cheat_no_police, mgr && mem::IsCommitted(mgr + kNoNewCops) ? mgr + kNoNewCops : 0);
    unlock_all.Apply(o.cheat_unlock_all, kUnlockAllThings);
}

// The movies' screens skip on A or START: FEAnyMovie (0x116230, the FE's and the story's), InGameAnyMovie (0x131A30)
// and the boot's (0x1311B0: EA logo, PSA, intro, attract; only while its byte at +0x2C allows). SKIP MOVIES has a tick
// (0xC98356BA, sent each frame, the subtitles' clock at 0x10D760) of each screen taken for A: once, as the skip posts
// the movie's end (0xC3960EB9) and a second would end what comes after; a few frames in, as the attract movie skipped
// on its first, with the boot screen's byte forced, hung on its first frame; and on the boot's screen only once its
// byte is set, the logos the game lets play out still play. A screen ends its movie once: a press of the player's
// after the skip (or before it) is taken for a tick, as a second end hung the attract movie on its frame. The screens'
// destructors forget them
constexpr uint32_t kMsgTick = 0xC98356BA, kMsgPadAccept = 0x406415E3, kMsgPadStart = 0xB5AF2461;
constexpr int kSkipTick = 10;
std::unordered_map<uint32_t, int> g_movie_ticks;  // a movie screen's ticks so far, past kSkipTick once skipped

// game thread, a movie screen's NotificationMessage (thiscall: the message, the object and two params)
void SkipMovie(X86Context& ctx, uint32_t allow_flag) {
    uint32_t message = kernel::R32(ctx.esp + 4);
    bool press = message == kMsgPadAccept || message == kMsgPadStart;
    if (!Opt().skip_movies || (!press && message != kMsgTick)) return;
    int& ticks = g_movie_ticks[ctx.ecx];
    if (press) {
        if (ticks > kSkipTick) kernel::W32(ctx.esp + 4, kMsgTick);  // ended already
        else if (!allow_flag || kernel::G<uint8_t>(ctx.ecx + allow_flag)) ticks = kSkipTick + 1;  // the player's ends it
        return;
    }
    if (ticks > kSkipTick || ++ticks < kSkipTick) return;
    if (allow_flag && !kernel::G<uint8_t>(ctx.ecx + allow_flag)) {
        ticks = kSkipTick - 1;  // not yet: the next tick asks again
        return;
    }
    ticks = kSkipTick + 1;
    kernel::W32(ctx.esp + 4, kMsgPadAccept);
}
void ForgetMovie(uint32_t screen) { g_movie_ticks.erase(screen); }

// The touch controls' RESET: the player's car put back on the road, as when it falls off the world. EResetPlayerCar
// does it (0x1B3640): the first player ([0x49550C], the count at 0x495514), its object's ([player + 4]) IResetable
// (found by 0x169A80, the interface 0x19AFC0; ResetCar's at +0x50), whose ResetVehicle (vtable +8, 0x1FF390, thiscall)
// takes the last place on the road ResetCar kept. The event passes false; true first asks whether a reset is allowed
constexpr uint32_t kPlayers = 0x49550C, kPlayerCount = 0x495514, kFindInterface = 0x169A80, kIResetable = 0x19AFC0;
constexpr uint32_t kResetVehicle = 8;
void ResetCarFrame(X86Context& ctx) {
    if (!g_reset_car.exchange(false) || !Racing()) return;
    struct Restore {
        X86Context& destination;
        X86Context saved;
        ~Restore() { destination = saved; }
    } restore{ctx, ctx};
    uint32_t list = kernel::R32(kPlayers);
    uint32_t player = kernel::R32(kPlayerCount) && list ? kernel::R32(list) : 0;
    uint32_t object = player ? kernel::R32(player + 4) : 0;
    if (!object) return;
    ctx.ecx = object;
    uint32_t resetable = kernel::CallGuest(ctx, kFindInterface, {kIResetable});
    if (!resetable) return;
    ctx.ecx = resetable;
    uint32_t done = kernel::CallGuest(ctx, kernel::R32(kernel::R32(resetable) + kResetVehicle), {0}) & 0xFF;
    LOGI("reset car: %s", done ? "done" : "no place kept");
}

// only English and Indonesian are offered; Indonesian plays the English table, translated (prompts.cpp)
bool LanguageAvailable(const std::string& code) { return code == "int" || code == "ind"; }

// GameFlowManager's state (0x4936B8, written by 0x1C5CD7 and the rest): 3 the front end, 6 racing, loading between
constexpr uint32_t kGameFlowState = 0x4936B8;
constexpr uint32_t kGameFlowRacing = 6;
bool Racing() { return mem::IsCommitted(kGameFlowState) && kernel::R32(kGameFlowState) == kGameFlowRacing; }

}  // namespace game

// The drops on the screen in the rain (0x0CC530, a view's "Rain Drop" pass): 20 of them, each a copy of the frame into
// a 32x32 target and a draw back, 40 renderings a frame. SCREEN RAINDROPS off returns at once (cdecl, the view)
X86_EXTERN_FUNC(__imp__sub_000CC530);
X86_FUNC(sub_000CC530) {
    if (game::Opt().rain_drops) return __imp__sub_000CC530(ctx, base);
    ctx.esp += 4;
}

// The falling rain: a view's precipitation ([view + 0x68], 0x0D818A) draws its 350 drops (+0x33D8, the RAINDROP texture
// at +0x3404) in 0x2527A0. FALLING RAIN off returns at once (thiscall, nothing on the stack); the rain's strength
// (+0x290), which wets the road, is still worked out around it
X86_EXTERN_FUNC(__imp__sub_002527A0);
X86_FUNC(sub_002527A0) {
    if (game::Opt().rain) return __imp__sub_002527A0(ctx, base);
    ctx.esp += 4;
}

// The yellow look: the "Visual Look Pass" (0x0CDA40, pixel shader 0x17) adds the scene times a tint (texture 3) over
// the desaturated scene. 0x0C1BD0 fills that tint's 256x1 ramp ([this + 0x234]) from a curve (+0x50) times the colour
// at +0xA0/+0xA4/+0xA8 (R, G, B), the visual treatment's, blended by the game through the day. YELLOW TINT off makes
// the ramp grey of the colour's luma while it is built, and gives the game its colour back
X86_EXTERN_FUNC(__imp__sub_000C1BD0);
X86_FUNC(sub_000C1BD0) {
    if (game::Opt().yellow_tint) return __imp__sub_000C1BD0(ctx, base);
    uint32_t tint = ctx.ecx + 0xA0;
    float& r = kernel::G<float>(tint);
    float& g = kernel::G<float>(tint + 4);
    float& b = kernel::G<float>(tint + 8);
    float saved[] = {r, g, b};
    r = g = b = 0.299f * saved[0] + 0.587f * saved[1] + 0.114f * saved[2];
    __imp__sub_000C1BD0(ctx, base);
    r = saved[0], g = saved[1], b = saved[2];
}

// The motion blur: the "Motion Blur" pass (0x0D1440, cdecl: the view, a flag, a mode, an amount; from the "Visual Look
// Filters" at 0x0D2382) blends the frame with the ones before it by the car's speed, and returns whether it drew; the
// "Radial Blur Mask Pass" (0x0CE1C0, nitrous and the speedbreaker) follows only then. Slow, it draws nothing and
// returns false itself (0x0D1661): MOTION BLUR off returns so at once
X86_EXTERN_FUNC(__imp__sub_000D1440);
X86_FUNC(sub_000D1440) {
    if (game::Opt().motion_blur) return __imp__sub_000D1440(ctx, base);
    ctx.eax &= ~0xFFu;
    ctx.esp += 4;
}

X86_EXTERN_FUNC(__imp__sub_00116230);
X86_FUNC(sub_00116230) {
    game::SkipMovie(ctx, 0);
    __imp__sub_00116230(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00131A30);
X86_FUNC(sub_00131A30) {
    game::SkipMovie(ctx, 0);
    __imp__sub_00131A30(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_001311B0);
X86_FUNC(sub_001311B0) {
    game::SkipMovie(ctx, 0x2C);
    __imp__sub_001311B0(ctx, base);
}
// their destructors (FEAnyMovie, InGameAnyMovie, the boot's)
X86_EXTERN_FUNC(__imp__sub_0010D6D0);
X86_FUNC(sub_0010D6D0) {
    game::ForgetMovie(ctx.ecx);
    __imp__sub_0010D6D0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0013F6D0);
X86_FUNC(sub_0013F6D0) {
    game::ForgetMovie(ctx.ecx);
    __imp__sub_0013F6D0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0013EEE0);
X86_FUNC(sub_0013EEE0) {
    game::ForgetMovie(ctx.ecx);
    __imp__sub_0013EEE0(ctx, base);
}

namespace app {

// the console's XC_LANGUAGE the game reads at boot: English, for Indonesian too, which the disc lacks
uint32_t ConsoleLanguage() { return 1; }

}  // namespace app
