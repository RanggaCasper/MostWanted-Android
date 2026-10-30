// The HUD's speedometer, minimap, race information and EA Trax banner moved where the touch layout editor puts them,
// out from under the touch controls. A race's HUD is the FEng package HUD_SingleRace.fng (HUD_Drag.fng and the split
// screen's are others), the song's banner EA_TRAX.fng; their pieces are groups found by their names' hashes
// (FEHashUpper, 0x157E40: -1, then times 33 plus each letter in capitals, the object's at +0x10). An object's place is its data's (+0x2C) x and y at +0x1C and +0x20, FEng units from its group's
// middle, read as it is drawn: a group moved moves what is in it. The game writes some of those places each frame
// (Hud_Minimap scrolls TRACK_MAP) and the package's scripts others, so they are moved after FEngine::Update, the last
// writer before the drawing (at the present, the map's roads stayed behind); and a place found other than as it was
// left is the game's new one, moved again from there
#include <cstring>
#include <unordered_map>

#include "common/hud_layout.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {

constexpr uint32_t kFEng = 0x48E94C;           // cFEng's instance, its first field the FEngine
constexpr uint32_t kFirstPackage = 0xE0;       // FEngine's
constexpr uint32_t kPackageNext = 0x4, kPackageName = 0xC, kPackageObjects = 0x4C;
constexpr uint32_t kObjectNext = 0x4, kObjectHash = 0x10, kObjectType = 0x18, kObjectData = 0x2C;
constexpr uint32_t kGroupCount = 0x60, kGroupObjects = 0x64;
constexpr uint32_t kTypeGroup = 5;
constexpr uint32_t kDataX = 0x1C, kDataY = 0x20;
constexpr int kMaxPackages = 64, kMaxObjects = 1500;

// the groups of each item, by their names' hashes. The speedometer's are in the bottom right cluster (group 0x5D0101F1,
// whose other groups are the race's information and a pursuit's boxes at the top right): the dial, the speed, the needle, the nitrous and
// speedbreaker gauges and the turbo's. The minimap's: its map (TRACK_MAP's four tiles), its icons (group 0x00A1A862:
// the player's arrow, the cops, the events, placed by Hud_Minimap) and the ring round the event it points to
struct Piece {
    uint32_t hash;
    HudItem item;
};
constexpr Piece kPieces[] = {
    {0x5164D4EA, kHudSpeedometer},  // GaugeCluster
    {0x941FFF09, kHudSpeedometer},  // SpeedometerGroup
    {0xC5D551B7, kHudSpeedometer},  // TURBO_GROUP
    {0x8991BD54, kHudSpeedometer},  // Speedbreaker_Meter_Group
    {0x5DF2F5B4, kHudSpeedometer},  // Speedbreaker_Meter_Multi_Image
    {0xEDFB6D37, kHudSpeedometer},  // the nitrous gauge's image
    {0x87C38E97, kHudSpeedometer},
    {0x0F365871, kHudMinimap},  // TRACK_MAP
    {0x00A1A862, kHudMinimap},  // the map's icons
    {0x382D2FC9, kHudMinimap},  // TRACKMAPTARGETRING
    {0xB0F63386, kHudMinimap},  // the roads, two multi images
    {0xC46A80A9, kHudMinimap},  // the map's rim and the heat level over it
    {0xB1C5ED0D, kHudRaceInfo},  // RaceInformationGroup: the position, the lap or completion and the time
    {0x5ED609A1, kHudRaceInfo},  // LeaderBoardGroup: the racers under it
    {0x9AFCE2B8, kHudRaceInfo},
    {0x2269D232, kHudRaceInfo},  // a pursuit's milestones and their progress
    // a pursuit's other boxes in the cluster, its column at the top right: the bounty with the pursuit's time, then the
    // total bounty, the cash and the cost to state
    {0xDE89E070, kHudRaceInfo},
    {0xEA903012, kHudRaceInfo},
    {0x3F8644CA, kHudRaceInfo},
    {0xD60FC075, kHudRaceInfo},
    {0xA1341735, kHudTrax},  // EA_TRAX.fng's banner, the song playing (in the front end too)
};

struct Moved {
    float x, y;            // the place the game gave it
    float set_x, set_y;    // what was written over it
};
std::unordered_map<uint32_t, Moved> g_moved;  // by the object's data

uint32_t Peek(uint32_t va) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + 3) ? kernel::R32(va) : 0; }

float Float(uint32_t va) {
    uint32_t b = kernel::R32(va);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
void SetFloat(uint32_t va, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    kernel::W32(va, b);
}

void Move(uint32_t data, HudItem item, std::unordered_map<uint32_t, Moved>& seen) {
    if (!data || !mem::IsCommitted(data) || !mem::IsCommitted(data + kDataY + 3)) return;
    float x = Float(data + kDataX), y = Float(data + kDataY);
    Moved m{x, y, x, y};
    if (auto it = g_moved.find(data); it != g_moved.end() && it->second.set_x == x && it->second.set_y == y) m = it->second;
    m.set_x = m.x + g_hud_x[item].load();
    m.set_y = m.y + g_hud_y[item].load();
    if (m.set_x != x) SetFloat(data + kDataX, m.set_x);
    if (m.set_y != y) SetFloat(data + kDataY, m.set_y);
    seen[data] = m;
}

void Walk(uint32_t first, int count, int& budget, std::unordered_map<uint32_t, Moved>& seen) {
    for (uint32_t o = first; o && budget-- > 0 && count != 0; o = Peek(o + kObjectNext), count--) {
        uint32_t hash = Peek(o + kObjectHash);
        bool piece = false;
        for (const Piece& p : kPieces)
            if (p.hash == hash) {
                Move(Peek(o + kObjectData), p.item, seen);
                piece = true;
            }
        if (!piece && Peek(o + kObjectType) == kTypeGroup) Walk(Peek(o + kGroupObjects), int(Peek(o + kGroupCount)), budget, seen);
    }
}

bool HudPackage(uint32_t p) {
    uint32_t name = Peek(p + kPackageName);
    char head[4] = {};
    for (uint32_t i = 0; name && i < 4 && mem::IsCommitted(name + i); i++) head[i] = char(kernel::G<uint8_t>(name + i));
    return std::memcmp(head, "HUD_", 4) == 0 || std::memcmp(head, "EA_T", 4) == 0;  // EA_TRAX.fng
}

}  // namespace

void ApplyHudLayout() {
    uint32_t feng = Peek(kFEng), fe = Peek(feng);
    std::unordered_map<uint32_t, Moved> seen;
    int n = 0;
    for (uint32_t p = fe ? Peek(fe + kFirstPackage) : 0; p && n < kMaxPackages; p = Peek(p + kPackageNext), n++) {
        if (!HudPackage(p)) continue;
        int budget = kMaxObjects;
        Walk(Peek(p + kPackageObjects), -1, budget, seen);
    }
    g_moved.swap(seen);  // a package gone forgets its objects
}

}  // namespace game

// FEngine::Update (thiscall, the time and a flag, ret 8): HUD's code has written its places by then (Hud_Minimap
// scrolls TRACK_MAP each frame), the drawing reads them after
X86_EXTERN_FUNC(__imp__sub_00165B20);
X86_FUNC(sub_00165B20) {
    __imp__sub_00165B20(ctx, base);
    game::ApplyHudLayout();
}
