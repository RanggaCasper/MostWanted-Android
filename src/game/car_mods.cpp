// Add-on cars from tools/car_mod.py. Its cars.txt, in the mods folder, has a line for each: the pvehicle's key, the
// frontend's key, the hash of its CARNAME string, its name texture's hash, width, height and NV2A format, and the name.
// The stock car list a profile keeps (FEPlayerCarDB, at +0x414, 200 entries of 20 bytes, filled by 0x140890 when the
// profile is new) is saved with it: a car newer than the profile is added here, as 0x140890 adds them, and is saved with
// it from then on. The car select's name is a texture no front end pack has: GetTextureInfo answers its hash with a copy
// of a TextureInfo reading its name_<hash>.dxt (the PC tool's one car: car_name.dxt), as src/game/options_menu.cpp does its icons. A car taken out again (the app's
// ModInstaller lists it in cars_removed.txt) stays in the saves that kept it, its key naming nothing now: it is taken out
// of the list here, with the customization and career slots of its record.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

void SetOwnString(uint32_t hash, const std::string& text);  // prompts.cpp

namespace {

constexpr uint32_t kFeDatabase = 0x48EDD4, kProfile = 0x10, kPlayerCarDb = 0x414, kCurrentCar = 0xA8;
constexpr uint32_t kEntry = 0x14, kEntries = 0xC8, kFrontend = 4, kVehicle = 8, kEmpty = 0xFFFFFFFF;
constexpr uint32_t kSetUpEntry = 0x134070;  // thiscall: the car's type and class from its keys
// a record's slots, 0xFF none, each slot's first byte 0xFF when free (0x1408C0 to 0x1408F7)
constexpr uint32_t kCustomization = 0x10, kCareer = 0x11, kNoSlot = 0xFF;
constexpr uint32_t kCustomizations = 0x1134, kCustomizationSize = 0x198, kCareers = 0x8728, kCareerSize = 0x38;

constexpr uint32_t kCarTypes = 0x4D5800, kStockTypes = 84, kTypeSize = 0xD0, kTypeName = 0x10, kTypeHash = 0x50, kTypeId = 0x90;

constexpr uint32_t kTemplateTexture = 0xF37AF144;  // OPTIONS_ICON_AUDIO, a TextureInfo the front end always has

struct Car {
    uint32_t vehicle, frontend, name_texture, width, height, format;
};

const std::vector<Car>& Cars() {
    static const std::vector<Car> cars = [] {
        std::vector<Car> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            uint32_t vehicle, frontend, hash, texture, width, height, format;
            int name = 0;
            if (std::sscanf(line.c_str(), "%x %x %x %x %u %u %u %n", &vehicle, &frontend, &hash, &texture, &width, &height,
                            &format, &name) < 7 || !name)
                continue;
            out.push_back({vehicle, frontend, texture, width, height, format});
            SetOwnString(hash, line.substr(name));
        }
        return out;
    }();
    return cars;
}

// the car types, the game's 84 and an empty one (84 is the "no car" answer) and the add-ons, from car_types.txt
uint32_t CarTypes() {
    static const uint32_t types = [] {
        std::vector<uint8_t> file;
        uint32_t n = 0;
        if (kernel::ReadGameFile("D:\\car_types.txt", file)) {
            file.push_back(0);
            std::sscanf(reinterpret_cast<const char*>(file.data()), "%u", &n);
        }
        return n > kStockTypes ? n : kStockTypes;
    }();
    return types;
}

// the type of a car's name's hash, kStockTypes if none (0x244FE0)
uint32_t TypeOfName(uint32_t hash) {
    uint32_t table = kernel::R32(kCarTypes);
    for (uint32_t i = 0, n = CarTypes(); i < n; i++) {
        if (i == kStockTypes) continue;
        uint32_t h = 0xFFFFFFFF;
        const auto* name = static_cast<const uint8_t*>(kernel::HostPtr(table + i * kTypeSize + kTypeName));
        while (*name) h = h * 33 + *name++;
        if (h == hash) return kernel::R32(table + i * kTypeSize + kTypeId);
    }
    return kStockTypes;
}

const std::vector<uint32_t>& RemovedCars() {
    static const std::vector<uint32_t> keys = [] {
        std::vector<uint32_t> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars_removed.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            uint32_t key;
            if (std::sscanf(text.substr(at, end - at).c_str(), "%x", &key) == 1) out.push_back(key);
            at = end + 1;
        }
        return out;
    }();
    return keys;
}

}  // namespace

// GetTextureInfo's question, on the game thread: our TextureInfo for the name texture's hash, else 0
uint32_t ModCarTexture(X86Context& ctx, uint32_t hash) {
    static std::map<uint32_t, uint32_t> made;
    for (const Car& car : Cars()) {
        if (!car.name_texture || car.name_texture != hash) continue;
        if (uint32_t info = made[hash]) return info;
        constexpr uint32_t kInfoBytes = 0x80, kPlatBytes = 0x40, kD3D = 0x10, kInfoHash = 0x24, kInfoSize = 0x40;
        std::vector<uint8_t> pixels;
        char file[32];
        std::snprintf(file, sizeof file, "D:\\name_%08x.dxt", hash);
        if (!kernel::ReadGameFile(file, pixels) && !kernel::ReadGameFile("D:\\car_name.dxt", pixels)) return 0;
        uint32_t tmpl = kernel::CallGuest(ctx, 0xC5900, {kTemplateTexture, 0, 0});
        if (!tmpl || !kernel::R32(tmpl)) return 0;
        uint32_t info = mem::AllocSystem(kInfoBytes), plat = mem::AllocSystem(kPlatBytes);
        std::memcpy(kernel::HostPtr(info), kernel::HostPtr(tmpl), kInfoBytes);
        std::memcpy(kernel::HostPtr(plat), kernel::HostPtr(kernel::R32(tmpl)), kPlatBytes);
        kernel::W32(info, plat);
        kernel::W32(info + kInfoHash, hash);
        kernel::W32(info + kInfoSize, car.width | car.height << 16);
        uint32_t data = mem::AllocContiguous(uint32_t(pixels.size()), 0, 0xFFFFFFFF, 0x1000);
        std::memcpy(kernel::HostPtr(data), pixels.data(), pixels.size());
        auto log2 = [](uint32_t v) {
            uint32_t n = 0;
            while ((1u << n) < v) n++;
            return n;
        };
        uint32_t d3d = plat + kD3D;
        kernel::W32(d3d + 4, mem::PhysicalFromVirtual(data));
        kernel::W32(d3d + 8, 0);
        kernel::W32(d3d + 12, (kernel::R32(d3d + 12) & 0xFF) | car.format << 8 | 1u << 16 | log2(car.width) << 20 |
                                  log2(car.height) << 24);
        kernel::W32(d3d + 16, 0);
        made[hash] = info;
        return info;
    }
    return 0;
}

// game thread, at each present
void ModCarsFrame(X86Context& ctx) {
    const std::vector<Car>& cars = Cars();
    const std::vector<uint32_t>& removed = RemovedCars();
    if (cars.empty() && removed.empty()) return;
    uint32_t db = kernel::R32(kFeDatabase);
    uint32_t profile = db && mem::IsCommitted(db + kProfile) ? kernel::R32(db + kProfile) : 0;
    if (!profile || !mem::IsCommitted(profile + kPlayerCarDb + kEntries * kEntry - 1)) return;
    uint32_t table = profile + kPlayerCarDb;
    if (kernel::R32(table) != 0) return;  // not filled yet: its first car is 0
    for (uint32_t key : removed) {
        for (uint32_t i = 0; i < kEntries; i++) {
            uint32_t entry = table + i * kEntry;
            if (kernel::R32(entry) == kEmpty || kernel::R32(entry + kVehicle) != key) continue;
            auto byte = [](uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); };
            uint8_t custom = *byte(entry + kCustomization), career = *byte(entry + kCareer);
            if (custom != kNoSlot) *byte(table + kCustomizations + custom * kCustomizationSize) = kNoSlot;
            if (career != kNoSlot) *byte(table + kCareers + career * kCareerSize) = kNoSlot;
            kernel::W32(entry, kEmpty);
        }
    }
    // the career's car (CareerSettings::CurrentCar, a record's handle, set at boot from 0x132A90 at 0x1571F6) gone with
    // the add-on: another of the career's cars instead, so the career goes on with what it has
    uint32_t current = kernel::R32(profile + kCurrentCar);
    bool found = false;
    uint32_t career_car = kEmpty;
    for (uint32_t i = 0; i < kEntries; i++) {
        uint32_t entry = table + i * kEntry, handle = kernel::R32(entry);
        if (handle == kEmpty) continue;
        if (handle == current) found = true;
        if (career_car == kEmpty && *static_cast<uint8_t*>(kernel::HostPtr(entry + kCareer)) != kNoSlot) career_car = handle;
    }
    if (!found && current != kEmpty && career_car != kEmpty) {
        LOGR("mods: the career's car %u is gone with its add-on; the career goes on in %u", current, career_car);
        kernel::W32(profile + kCurrentCar, career_car);
    }
    static uint32_t frames = 0;
    if (++frames % 30) return;
    for (const Car& car : cars) {
        uint32_t free_slot = kEmpty;
        bool listed = false;
        for (uint32_t i = 0; i < kEntries && !listed; i++) {
            uint32_t entry = table + i * kEntry;
            if (kernel::R32(entry) == kEmpty) {
                if (free_slot == kEmpty) free_slot = i;
            } else if (kernel::R32(entry + kVehicle) == car.vehicle) {
                listed = true;
            }
        }
        if (listed || free_slot == kEmpty) continue;
        uint32_t entry = table + free_slot * kEntry;
        kernel::W32(entry, free_slot);
        kernel::W32(entry + kFrontend, car.frontend);
        kernel::W32(entry + kVehicle, car.vehicle);
        uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
        ctx.ecx = entry;
        kernel::CallGuest(ctx, kSetUpEntry, {});
        ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4],
        ctx.edi = saved[5], ctx.ebp = saved[6];
    }
}

}  // namespace game

// Car types past the game's 84. The table is read from GLOBALB (0x25D279) and is longer for add-ons; these are the places
// that know its length: its two searches, and three that took 84 and up for no car. 0x2A33EA's loop, a list of their names, stays at 84:
// what reads it is not known. A guest function called from here keeps the registers the x86 conventions say it keeps.
X86_FUNC(sub_00244FE0) {  // thiscall (hash)
    ctx.eax = game::TypeOfName(kernel::R32(ctx.esp + 4));
    ctx.esp += 8;  // a ret pops its address too
}

X86_FUNC(sub_00245030) {  // cdecl (the name's other hash, +0x50): the type's record, 0 if none
    uint32_t hash = kernel::R32(ctx.esp + 4), table = kernel::R32(game::kCarTypes);
    ctx.eax = 0;
    for (uint32_t i = 0, n = game::CarTypes(); i < n && !ctx.eax; i++)
        if (i != game::kStockTypes && kernel::R32(table + i * game::kTypeSize + game::kTypeHash) == hash)
            ctx.eax = table + i * game::kTypeSize;
    ctx.esp += 4;
}

namespace {
bool RealType(uint32_t type) { return type != 0xFFFFFFFF && type != game::kStockTypes && type < game::CarTypes(); }

// 0x263F10 and 0x263FD0, cdecl (a): the car's two render connections, each made as new (size)(a, type, a's +4)
void MakeConnection(X86Context& ctx, uint32_t size, uint32_t ctor) {
    uint32_t a = kernel::R32(ctx.esp + 4), inner = kernel::R32(a + 4);
    uint32_t type = game::TypeOfName(kernel::R32(inner + 4));
    ctx.eax = 0;
    if (RealType(type)) {
        ctx.ecx = 0x4936E8;
        uint32_t mem = kernel::CallGuest(ctx, 0x168F10, {size, 0});
        ctx.eax = 0;
        if (mem) {
            ctx.ecx = mem;
            ctx.eax = kernel::CallGuest(ctx, ctor, {a, type, inner});
        }
    }
    ctx.esp += 4;
}
}  // namespace

X86_FUNC(sub_00263F10) { MakeConnection(ctx, 0x410, 0x262E10); }
X86_FUNC(sub_00263FD0) { MakeConnection(ctx, 0x1C0, 0x263300); }

X86_FUNC(sub_001C9AB0) {  // thiscall (a, flag, c), a car's model ID as this
    uint32_t self = ctx.ecx, a = kernel::R32(ctx.esp + 4), flag = kernel::R32(ctx.esp + 8), c = kernel::R32(ctx.esp + 12);
    kernel::W32(self + 8, 0);
    uint32_t name = kernel::R32(kernel::R32(a + 8) + 0x10 + 0xC);
    if (!name) name = 0x3CA680;
    uint32_t hash = 0xFFFFFFFF;
    for (const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(name)); *p; p++) hash = hash * 33 + *p;
    uint32_t type = game::TypeOfName(hash);
    kernel::W32(self, type);
    if (RealType(type)) {
        uint32_t flags = 0;
        if (kernel::CallGuest(ctx, 0x24FFB0, {type}) & 0xFF) flags |= 4;
        uint32_t one = kernel::CallGuest(ctx, 0x20C360, {}) == 1;
        uint32_t model = kernel::CallGuest(ctx, 0x245380, {type, c, one});
        kernel::W32(self + 4, model);
        if (flag & 0xFF) flags |= 2;
        if (model) flags |= 1;
        kernel::W32(self + 8, flags);
    }
    ctx.eax = self;
    ctx.esp += 16;
}

// The main pool is a fixed 55 MB taken from the kernel at the first allocation (0x421BB8, read by 0x54340), the car pool
// 7550 KB in a race and 6300 KB in the front end out of it (0x433A00, 0x433A04), sized for the disc's cars on a 64 MB box:
// add-on cars ran out of both. With twice the memory (kernel/memory.h) both grow; the track streamer's block is taken with the
// car pool's size added (0x1C7DC3), so its share stays. For every player, add-ons or not, so all play on the one layout.
namespace game {
namespace {

constexpr uint32_t kMainPoolSize = 0x421BB8, kMainPoolExtra = 48u << 20;
constexpr uint32_t kCarPoolRace = 0x433A00, kCarPoolFrontEnd = 0x433A04, kCarPoolExtraKb = 32u << 10;

}  // namespace
}  // namespace game

X86_EXTERN_FUNC(__imp__sub_00054340);
X86_FUNC(sub_00054340) {
    static bool done = false;
    if (!done) {
        done = true;
        kernel::W32(game::kMainPoolSize, kernel::R32(game::kMainPoolSize) + game::kMainPoolExtra);
        kernel::W32(game::kCarPoolRace, kernel::R32(game::kCarPoolRace) + game::kCarPoolExtraKb);
        kernel::W32(game::kCarPoolFrontEnd, kernel::R32(game::kCarPoolFrontEnd) + game::kCarPoolExtraKb);
    }
    __imp__sub_00054340(ctx, base);
}
