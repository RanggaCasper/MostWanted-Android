// Need for Speed: Most Wanted (Black Edition) needs no patch of its code to boot: the XDK it calls is all on the host.
// Its own hooks go here, [[hook]] entries in config/game.toml and X86_FUNC replacements of recompiled functions.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <unordered_map>

#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "common/music.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

// The console's widescreen flag (XGetVideoFlags at boot, 0x0D0202, kept at 0x48099C; set from 3:2 on, native/main.cpp)
// gives the front end's and HUD's screen transform (0x0D4888, 0x0D8330: 576 / width across, 448 / 480 down, made 640
// and 480 below) a frame
// 853 wide (480 * 16 / 9, at 0x427C00) for a 4:3 one's 640. It is the frame's own shape here, so 19.5:9 is not 16:9
// stretched
constexpr uint32_t kVideoFlags = 0x48099C, kWideWidth = 0x427C00;
// The 576 and 448 (0x3D3534, 0x3D3530, read only by those two transforms) are the console's title safe area, the
// 640x480 layout drawn 0.9 across and 0.93 down for a TV's overscan: the front end, its movies and loading screens
// had a black border on a screen that shows the whole frame. 640 and 480 draw the layout over the whole frame, as
// the PC's did; fe_mouse.cpp reads them back for its hit tests
constexpr uint32_t kSafeWidth = 0x3D3534, kSafeHeight = 0x3D3530;
void FullFrameFrontEnd() {
    constexpr float kWidth = 640.0f, kHeight = 480.0f;
    uint32_t w, h;
    std::memcpy(&w, &kWidth, 4);
    std::memcpy(&h, &kHeight, 4);
    kernel::W32(kSafeWidth, w);
    kernel::W32(kSafeHeight, h);
}
void WidenAspect() {
    FullFrameFrontEnd();
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
constexpr uint32_t kSkyMidday = 0x42FDA0, kSkySunset = 0x42FDB0;
constexpr uint32_t kHorizonHaze = 0x4279D7;
constexpr uint32_t kHeadlightBeamColor = 0x469B0C;
inline int g_headlights_clear_frames = 0;
inline bool g_prev_night_mode = false;

// the colour data of a view's target, [target + 8]'s, as a texture's address reads
uint32_t TargetData(uint32_t target) {
    uint32_t surface = target ? kernel::R32(target + 8) : 0;
    return surface ? mem::VirtualFromPhysical(kernel::R32(surface + 4)) : 0;
}

uint32_t TextureDataFromInfo(uint32_t info) {
    if (!info || !mem::IsCommitted(info)) return 0;
    uint32_t plat = kernel::R32(info);
    if (!plat || !mem::IsCommitted(plat)) return 0;
    uint32_t phys = kernel::R32(plat + 0x14);
    if (!phys) return 0;
    return mem::VirtualFromPhysical(phys);
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

    // Update sky texture targets if available (from global array 0x4D5578)
    if (uint32_t d = TextureDataFromInfo(kernel::R32(0x4D5578 + 0x08))) g_sky_clouds_a_data = d;
    if (uint32_t d = TextureDataFromInfo(kernel::R32(0x4D5578 + 0x10))) g_sky_clouds_b_data = d;

    // night mode sky dome multipliers, horizon haze, and headlight road projection color
    static const float kSkyDayMidday[4] = {0.9723f, 0.9900f, 1.1000f, 1.0f};
    static const float kSkyDaySunset[4] = {0.95858f, 1.0700f, 1.1150f, 1.0f};
    static const float kSkyNight[4] = {0.10f, 0.15f, 0.32f, 1.0f};
    const float* midday = Opt().night_mode ? kSkyNight : kSkyDayMidday;
    const float* sunset = Opt().night_mode ? kSkyNight : kSkyDaySunset;
    std::memcpy(kernel::HostPtr(kSkyMidday), midday, sizeof(kSkyNight));
    std::memcpy(kernel::HostPtr(kSkySunset), sunset, sizeof(kSkyNight));
    kernel::G<uint8_t>(kHorizonHaze) = Opt().night_mode ? 0 : 1;
    kernel::W32(kHeadlightBeamColor, Opt().night_mode ? 0x48FFF4D8 : 0x80cccccc);

    // Car headlight beam quad geometry tuning:
    // Default vanilla is a 20m flat rectangle (13m center, 10m length, 4.9m width) which severely clips/floats on curves.
    // In night mode, use a realistic 12m cone (10m center, 6m length, 5.2m wide ahead, 2.4m at bumper) that hugs road contours.
    static const float kBeamDay[5] = {13.0f, 10.0f, 4.9f, 10.0f, 4.9f};
    static const float kBeamNight[5] = {10.0f, 6.0f, 5.2f, 6.0f, 2.4f};
    const float* beam = Opt().night_mode ? kBeamNight : kBeamDay;
    kernel::G<float>(0x469ACC) = beam[0];
    kernel::G<float>(0x469AE4) = beam[1];
    kernel::G<float>(0x469AE8) = beam[1];
    kernel::G<float>(0x469AEC) = beam[2];
    kernel::G<float>(0x469AF0) = beam[2];
    kernel::G<float>(0x469AF4) = beam[3];
    kernel::G<float>(0x469AF8) = beam[3];
    kernel::G<float>(0x469AFC) = beam[4];
    kernel::G<float>(0x469B00) = beam[4];

    bool night = Opt().night_mode;
    if (!night && g_prev_night_mode) g_headlights_clear_frames = 120;
    g_prev_night_mode = night;
    if (g_headlights_clear_frames > 0) --g_headlights_clear_frames;
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

// the game's fonts are Latin-1
static std::string MusicLatin1(const std::string& text) {
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

// text for the game, kept: each song's the once
static uint32_t MusicText(const std::string& text) {
    static std::unordered_map<std::string, uint32_t> copies;
    uint32_t& va = copies[text];
    if (!va) {
        std::string latin = MusicLatin1(text);
        va = mem::AllocSystem(uint32_t(latin.size() + 1));
        if (va) std::memcpy(kernel::HostPtr(va), latin.c_str(), latin.size() + 1);
    }
    return va;
}

// My Music begins as the game's menu music does: at the main menu, or a race if one comes first, past the boot's movies and title.
// FEng's packages are a list from cFEng's FEngine (0x48E94C, +0xE0, next +4, name +0xC); once there, it stays on
static bool MusicMayBegin() {
    constexpr uint32_t kFEng = 0x48E94C, kFirstPackage = 0xE0, kPackageNext = 4, kPackageName = 0xC;
    static bool reached = false;
    if (reached) return true;
    if (Racing()) return reached = true;
    uint32_t fe = kernel::R32(kFEng);
    fe = fe && mem::IsCommitted(fe) ? kernel::R32(fe) : 0;
    uint32_t package = fe && mem::IsCommitted(fe + kFirstPackage) ? kernel::R32(fe + kFirstPackage) : 0;
    for (int n = 0; package && n < 64 && mem::IsCommitted(package + kPackageName); n++) {
        uint32_t name = kernel::R32(package + kPackageName);
        if (name && mem::IsCommitted(name + 7) && !strncasecmp(static_cast<const char*>(kernel::HostPtr(name)), "MainMenu", 8))
            return reached = true;
        package = kernel::R32(package + kPackageNext);
    }
    return false;
}

// My Music (native/music.cpp) or the game's EA Trax, as the panel says, EA Trax's own list untouched either way. The game's music
// shares one buffer with its other sounds, so it can only be stopped in the game: the profile's switch at +0x78 (0x0DC6A0, the
// option beside Interactive Music, +0x74, which goes on playing in pursuits). My Music plays at the
// game's own sliders, menu music +0x54 and game music +0x58, and a song's beginning shows on EA Trax's banner (0x1443C0, as 0xB8590)
void MusicFrame(X86Context& ctx) {
    constexpr uint32_t kFeDatabase = 0x48EDD4, kProfile = 0x10, kEaTrax = 0x78, kMenuMusic = 0x54, kGameMusic = 0x58;
    constexpr uint32_t kShowSong = 0x1443C0;
    if (!music::HasSongs()) return;
    uint32_t db = kernel::R32(kFeDatabase);
    uint32_t profile = db && mem::IsCommitted(db + kProfile) ? kernel::R32(db + kProfile) : 0;
    if (!profile || !mem::IsCommitted(profile + kGameMusic)) return;
    static int source = -1;
    int now = music::Source();
    if (now == music::kMyMusic) {
        kernel::W32(profile + kEaTrax, 0);
    } else if (source == music::kMyMusic) {
        kernel::W32(profile + kEaTrax, 1);  // given back, once: the player's own to change after
    }
    source = now;
    uint32_t bits = kernel::R32(profile + (Racing() ? kGameMusic : kMenuMusic));
    float level;
    std::memcpy(&level, &bits, sizeof level);
    music::SetContext(MusicMayBegin(), Racing(), std::clamp(level, 0.0f, 1.0f));
    music::Song song;
    if (music::TakeStarted(&song)) {
        uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
        kernel::CallGuest(ctx, kShowSong, {MusicText(song.title), MusicText(song.artist), MusicText(song.album)});
        ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4], ctx.edi = saved[5],
        ctx.ebp = saved[6];
    }
}

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

// The yellow look and night mode look: the "Visual Look Pass" (0x0CDA40, pixel shader 0x17) adds the scene times a tint (texture 3) over
// the desaturated scene. 0x0C1BD0 fills that tint's 256x1 ramp ([this + 0x234]) from a curve (+0x50) times the colour
// at +0xA0/+0xA4/+0xA8 (R, G, B), the visual treatment's, blended by the game through the day. YELLOW TINT off makes
// the ramp grey of the colour's luma while it is built. NIGHT MODE tints it with cool moonlight blue
X86_EXTERN_FUNC(__imp__sub_000C1BD0);
X86_FUNC(sub_000C1BD0) {
    if (game::Opt().yellow_tint && !game::Opt().night_mode) return __imp__sub_000C1BD0(ctx, base);
    uint32_t tint = ctx.ecx + 0xA0;
    float& r = kernel::G<float>(tint);
    float& g = kernel::G<float>(tint + 4);
    float& b = kernel::G<float>(tint + 8);
    float saved[] = {r, g, b};
    float luma = 0.299f * saved[0] + 0.587f * saved[1] + 0.114f * saved[2];
    if (game::Opt().night_mode) {
        r = 0.15f * luma;
        g = 0.32f * luma;
        b = 0.68f * luma;
    } else {
        r = g = b = luma;
    }
    __imp__sub_000C1BD0(ctx, base);
    r = saved[0], g = saved[1], b = saved[2];
}

// In night mode, clamp bloom intensity (+0x170) to prevent blinding daytime glare
X86_EXTERN_FUNC(__imp__sub_000CDA40);
X86_FUNC(sub_000CDA40) {
    if (!game::Opt().night_mode) return __imp__sub_000CDA40(ctx, base);
    uint32_t obj = ctx.ecx;
    float& bloom = kernel::G<float>(obj + 0x170);
    float saved_bloom = bloom;
    bloom = std::min(bloom, 0.20f);
    __imp__sub_000CDA40(ctx, base);
    bloom = saved_bloom;
}

// Sun glare (0x0D5AB0) and sun disk/rays pass (0x0D5CA0): night mode skips them completely
X86_EXTERN_FUNC(__imp__sub_000D5AB0);
X86_FUNC(sub_000D5AB0) {
    if (game::Opt().night_mode) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_000D5AB0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000D5CA0);
X86_FUNC(sub_000D5CA0) {
    if (game::Opt().night_mode) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_000D5CA0(ctx, base);
}

// Vehicle headlights and flares (0x24AB90): PVehicle::mGlareState (at [esp + 4]) bits | 7
// turns on front headlights, flares and illuminated light textures on all vehicles
X86_EXTERN_FUNC(__imp__sub_0024AB90);
X86_FUNC(sub_0024AB90) {
    uint32_t ptr = kernel::R32(ctx.esp + 4);
    if (ptr && mem::IsCommitted(ptr)) {
        if (game::Opt().night_mode) {
            kernel::W32(ptr, kernel::R32(ptr) | 7);
        } else if (game::g_headlights_clear_frames > 0) {
            kernel::W32(ptr, kernel::R32(ptr) & ~7u);
        }
    }
    __imp__sub_0024AB90(ctx, base);
}

// Vehicle headlight ground projection (0x24ABE0): arg 5 (at [esp + 20])
// activates the "2PLAYERHEADLIGHT1" headlight beam quad projected onto the road surface
X86_EXTERN_FUNC(__imp__sub_0024ABE0);
X86_FUNC(sub_0024ABE0) {
    if (game::Opt().night_mode) {
        kernel::W32(ctx.esp + 20, 1);
    }
    __imp__sub_0024ABE0(ctx, base);
}

// InitSkyTextures: also update sky texture addresses from global array
X86_EXTERN_FUNC(__imp__sub_00244700);
X86_FUNC(sub_00244700) {
    __imp__sub_00244700(ctx, base);
    if (uint32_t d = game::TextureDataFromInfo(kernel::R32(0x4D5578 + 0x08))) game::g_sky_clouds_a_data = d;
    if (uint32_t d = game::TextureDataFromInfo(kernel::R32(0x4D5578 + 0x10))) game::g_sky_clouds_b_data = d;
}

// In night mode, ensure cloud/star layers (layer 1 & 2) have full alpha (1.0f) so they are never skipped
X86_EXTERN_FUNC(__imp__sub_00244DB0);
X86_FUNC(sub_00244DB0) {
    uint32_t layer = kernel::R32(ctx.esp + 8);
    uint32_t ptr_a = kernel::R32(ctx.esp + 24);
    __imp__sub_00244DB0(ctx, base);
    if (game::Opt().night_mode && (layer == 1 || layer == 2)) {
        if (ptr_a && mem::IsCommitted(ptr_a)) {
            kernel::G<float>(ptr_a) = 1.0f;
        }
    }
}

// In night mode, boost the cloud/star layers (layer 1 & 2) color to brilliant white (1.0)
// so the stars shine crisp and bright in the night sky
X86_EXTERN_FUNC(__imp__sub_00244B70);
X86_FUNC(sub_00244B70) {
    uint32_t layer = kernel::R32(ctx.esp + 8);
    if (game::Opt().night_mode && (layer == 1 || layer == 2)) {
        kernel::G<float>(0x42FE00) = 1.0f;
        kernel::G<float>(0x42FE04) = 1.0f;
        kernel::G<float>(0x42FE08) = 1.0f;
        kernel::G<float>(0x42FE0C) = 1.0f;
    }
    __imp__sub_00244B70(ctx, base);
}

// The motion blur: the "Motion Blur" pass (0x0D1440, cdecl: the view, a flag, a mode, an amount; from the "Visual Look
// Filters" at 0x0D2382) blends the frame with the ones before it by the car's speed, and returns whether it drew; the
// "Radial Blur Mask Pass" (0x0CE1C0, nitrous and the speedbreaker) follows only then. Slow, it draws nothing and
// returns false itself (0x0D1661): MOTION BLUR off returns so at once
X86_EXTERN_FUNC(__imp__sub_000D1440);
X86_FUNC(sub_000D1440) {
    // and none while cinematic mode flies the camera: its moves are no speed of the car's
    if (game::Opt().motion_blur && !game::CinematicActive()) return __imp__sub_000D1440(ctx, base);
    ctx.eax &= ~0xFFu;
    ctx.esp += 4;
}

// EA's file streams ("STRM", the system's handles 16 bytes each at +0x34): the stream thread hands a handle its data
// in 0x871D0, under the system's critical section (+8, entered by 0x2BC401/left by 0x2BC40C with the system + 4),
// adding to the bytes ready (+8) and then, from none, setting the first block (+0xC). The reader 0x87A40 (cdecl, the
// handle) checks the bytes and takes the block before it enters the section: on a single core the window was
// a few instructions, on the phone's cores (and stores out of order) the movies read a block not set yet, 0xEEEEEEEE
// from bMalloc's fill of a fresh handle, a SIGSEGV as a movie started or stopped. The whole read holds the section
// (recursive: the reader's own enter nests)
X86_EXTERN_FUNC(__imp__sub_00087A40);
X86_EXTERN_FUNC(sub_002BC401);
X86_EXTERN_FUNC(sub_002BC40C);
X86_FUNC(sub_00087A40) {
    constexpr uint32_t kStrm = 0x4D525453;
    uint32_t handle = kernel::R32(ctx.esp + 4);
    uint32_t system = handle ? kernel::R32(handle) : 0;
    if (!system || !mem::IsCommitted(system) || kernel::R32(system) != kStrm) return __imp__sub_00087A40(ctx, base);
    ctx.ecx = system + 4;
    X86_PUSH32(0);
    sub_002BC401(ctx, base);
    __imp__sub_00087A40(ctx, base);
    uint32_t result = ctx.eax;
    ctx.ecx = system + 4;
    X86_PUSH32(0);
    sub_002BC40C(ctx, base);
    ctx.eax = result;
}

// AUTO SHIFT IN DRAG: the player's transmission (ITransmission, 0x11FB0, found as RESET finds IResetable) is
// EngineDragster's (its interface at +0x50, vtable 0x3E3560) in a drag race. Its shift potential (+0x18C, the drag
// HUD's shift state, 0x2108C0) stayed 0 through a whole race, and the HUD's update was not called in every race, so the
// cheat watches the engine itself: the rpm (+0x184, IEngine's 0x1F3890) against the redline ([+0x150] + 0x58,
// 0x1F38C0), the gear (+0x88: 0 reverse, 1 neutral, 2 first) and the clutch's state (+0x19C, 0 engaged), and shifts with
// the transmission's own Shift (vtable +0xC, 0x1FDF50, thiscall, the gear; 0x1F3F70 checks it against the top gear).
// Once a gear, again if the gear has not changed a while later. EngineDragster stays: the races' EngineRacer in its
// place broke the drag race's sound as it loaded (0x8AC00 on a stream that was not there)
namespace {
constexpr uint32_t kITransmission = 0x11FB0, kDragTransmissionVtable = 0x3E3560, kTransmissionShift = 0xC;
constexpr uint32_t kEngineFromTransmission = 0x50;
constexpr uint32_t kEngineRpm = 0x184, kEngineInfo = 0x150, kInfoRedline = 0x58, kEngineGear = 0x88;
constexpr uint32_t kEngineClutch = 0x19C;
constexpr uint32_t kFirstGear = 2;
constexpr float kShiftAt = 0.97f;  // of the redline: a perfect shift; sitting on it blows a drag car's engine
constexpr uint64_t kShiftRetryMs = 600;

uint64_t SteadyMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

float GuestF32(uint32_t va) {
    uint32_t v = kernel::R32(va);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
}  // namespace

namespace game {
void DragShiftFrame(X86Context& ctx) {
    if (!Opt().cheat_drag_shift || !Racing()) return;
    uint32_t list = kernel::R32(kPlayers);
    uint32_t player = kernel::R32(kPlayerCount) && list ? kernel::R32(list) : 0;
    uint32_t object = player ? kernel::R32(player + 4) : 0;
    if (!object) return;
    struct Restore {
        X86Context& destination;
        X86Context saved;
        ~Restore() { destination = saved; }
    } restore{ctx, ctx};
    ctx.ecx = object;
    uint32_t transmission = kernel::CallGuest(ctx, kFindInterface, {kITransmission});
    if (!transmission || kernel::R32(transmission) != kDragTransmissionVtable) return;
    uint32_t engine = transmission - kEngineFromTransmission;
    uint32_t info = kernel::R32(engine + kEngineInfo);
    if (!mem::IsCommitted(info + kInfoRedline)) return;
    static uint32_t last_gear = ~0u;
    static uint64_t sent_ms = 0;
    uint32_t gear = kernel::R32(engine + kEngineGear);
    float rpm = GuestF32(engine + kEngineRpm), redline = GuestF32(info + kInfoRedline);
    uint64_t now = SteadyMs();
    if (gear != last_gear) {
        LOGI("drag shift: gear %u, rpm %.0f, redline %.0f", gear, rpm, redline);
        last_gear = gear;
        sent_ms = 0;
    }
    if (gear < kFirstGear || kernel::R32(engine + kEngineClutch) != 0) return;
    if (!(redline > 0) || rpm < redline * kShiftAt) return;
    if (sent_ms && now - sent_ms < kShiftRetryMs) return;
    sent_ms = now;
    ctx.ecx = transmission;
    uint32_t done = kernel::CallGuest(ctx, kernel::R32(kDragTransmissionVtable + kTransmissionShift), {gear + 1}) & 0xFF;
    LOGI("drag shift: up from gear %u, rpm %.0f: %s", gear, rpm, done ? "shifted" : "refused");
}
}  // namespace game

// NO TRAFFIC: AITrafficManager's update (0x35A50, thiscall, the time step) spawns traffic only while the density
// 0x22CC0 gives (cdecl, a float on the x87 stack) is above 0. The game gives 0 itself in a pursuit; with the cheat on
// it is always 0. The cars already about (switched on in a race) are let go as the update lets go of the ones it no
// longer keeps (0x35B41): each active car in its list (+0x94, a ring of nodes, the car at +8; IsActive vtable +0x88)
// has its AI (vtable +0xAC) told to leave (vtable +0x8C)
namespace {
constexpr uint32_t kTrafficCars = 0x94, kNodeCar = 8;
constexpr uint32_t kCarIsActive = 0x88, kCarAi = 0xAC, kAiLeave = 0x8C;
uint32_t VCall(X86Context& ctx, uint32_t self, uint32_t slot) {
    ctx.ecx = self;
    return kernel::CallGuest(ctx, kernel::R32(kernel::R32(self) + slot), {});
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00022CC0);
X86_FUNC(sub_00022CC0) {
    __imp__sub_00022CC0(ctx, base);
    if (game::Opt().cheat_no_traffic) X87_ST(0) = 0.0;
}

X86_EXTERN_FUNC(__imp__sub_00035A50);
X86_FUNC(sub_00035A50) {
    uint32_t manager = ctx.ecx;
    __imp__sub_00035A50(ctx, base);
    if (!game::Opt().cheat_no_traffic) return;
    struct Restore {
        X86Context& destination;
        X86Context saved;
        ~Restore() { destination = saved; }
    } restore{ctx, ctx};
    uint32_t head = kernel::R32(manager + kTrafficCars);
    if (!head) return;
    int released = 0;
    for (uint32_t node = kernel::R32(head); node && node != head; node = kernel::R32(node)) {
        uint32_t car = kernel::R32(node + kNodeCar);
        if (!car || !(VCall(ctx, car, kCarIsActive) & 0xFF)) continue;
        if (uint32_t ai = VCall(ctx, car, kCarAi)) {
            VCall(ctx, ai, kAiLeave);
            released++;
        }
    }
    if (released) LOGI("no traffic: %d cars let go", released);
}

// The drag races' traffic is not the manager's: a script activity spawns it a car at a time as the race goes on
// (0x181120, thiscall, no arguments, true once done; its state at +0x4C, 1 until the car is made). In state 1 it reads
// the car's model from its attributes (+4, 0x490A0 with the key 0xF833C06F and 0, the string at the answer) and gives
// the car traffic's AI, or a cop's for "copmidsize" (0x18117D). With the cheat on a car that is not a cop is never
// made: the activity stays in state 1, which the race does not wait on (its cars came seconds into the race)
namespace {
constexpr uint32_t kSpawnState = 0x4C, kSpawnAttributes = 4, kAttribute = 0x490A0, kModelKey = 0xF833C06F;
constexpr uint8_t kSpawnMaking = 1;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00181120);
X86_FUNC(sub_00181120) {
    uint32_t spawner = ctx.ecx;
    uint8_t state = *static_cast<const uint8_t*>(kernel::HostPtr(spawner + kSpawnState));
    if (game::Opt().cheat_no_traffic && state == kSpawnMaking) {
        X86Context saved = ctx;
        ctx.ecx = spawner + kSpawnAttributes;
        uint32_t model = kernel::CallGuest(ctx, kAttribute, {kModelKey, 0});
        ctx = saved;
        const char* name = model && mem::IsCommitted(model) && kernel::R32(model)
                               ? static_cast<const char*>(kernel::HostPtr(kernel::R32(model)))
                               : nullptr;
        if (name && std::strcmp(name, "copmidsize") != 0) {
            ctx.eax = 0;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_00181120(ctx, base);
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
