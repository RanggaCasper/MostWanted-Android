// The saves' guard, before the game boots. Every profile (E:\UDATA\4541007b\<folder>\<file>, the .xbx files being the
// dashboard's) is checked as the game checks it on loading: "MC02" at 0, the file's size at 4, the header's 0x38 bytes
// signed at 0x38 (0x30EC70 to 0x30ECE6) and the rest from 0x54 signed at 0x24, both HMAC-SHA1 with the title's signature
// key, which the kernel here leaves zero (XboxSignatureKey; XCalculateSignature reads it straight). A good one is copied to
// MostWanted/backups/saves/<folder>/<file>/<stamp> when it differs from the newest copy there, the newest ten kept. A bad
// one, a write cut short (the app killed while the game saved: the Mods Loader stops the game, Android ends apps in the
// background), is put back from its newest good copy, the bad one kept in the backups as <stamp>.bad. A profile the game
// itself turned down (its car list's hash, src/game/saves.cpp) is not bad here and is left alone
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "host.h"
#include "kernel/crypto.h"

namespace saves {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x4D433032;  // "20CM" in the file
constexpr size_t kHeader = 0x38, kHeaderSig = 0x38, kContentSig = 0x24, kContent = 0x54, kSig = 20;
constexpr size_t kKept = 10;

bool Read(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool Write(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::path temp = p;
    temp += ".new";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(temp, p, ec);
    return !ec;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

// what the game asks of a profile before it reads it
bool Good(const std::vector<uint8_t>& d) {
    if (d.size() < kContent || U32(d, 0) != kMagic || U32(d, 4) != d.size()) return false;
    static const uint8_t key[16] = {};
    uint8_t sig[kSig];
    crypto::Hmac(key, sizeof key, d.data(), kHeader, nullptr, 0, sig);
    if (std::memcmp(sig, d.data() + kHeaderSig, kSig)) return false;
    crypto::Hmac(key, sizeof key, d.data() + kContent, d.size() - kContent, nullptr, 0, sig);
    return std::memcmp(sig, d.data() + kContentSig, kSig) == 0;
}

std::string Stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char s[32];
    std::strftime(s, sizeof s, "%Y%m%d-%H%M%S", &tm);
    return s;
}

// a profile's copies, the newest last; the .bad ones left out
std::vector<fs::path> Copies(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() != ".bad" && e.path().extension() != ".new") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

void Guard(const fs::path& file, const fs::path& store) {
    std::vector<uint8_t> data;
    if (!Read(file, data)) return;
    std::error_code ec;
    fs::create_directories(store, ec);
    std::vector<fs::path> copies = Copies(store);
    if (Good(data)) {
        std::vector<uint8_t> last;
        if (!copies.empty() && Read(copies.back(), last) && last == data) return;
        if (!Write(store / Stamp(), data)) {
            LOGW("saves: %s could not be backed up", file.c_str());
            return;
        }
        copies = Copies(store);
        for (size_t i = 0; i + kKept < copies.size(); i++) fs::remove(copies[i], ec);
        return;
    }
    // newest good copy first
    for (auto it = copies.rbegin(); it != copies.rend(); ++it) {
        std::vector<uint8_t> copy;
        if (!Read(*it, copy) || !Good(copy)) continue;
        Write(store / (Stamp() + ".bad"), data);
        if (Write(file, copy)) {
            LOGR("saves: %s was damaged (%zu bytes): put back from the backup %s", file.c_str(), data.size(),
                 it->filename().c_str());
        } else {
            LOGE("saves: %s is damaged and could not be put back", file.c_str());
        }
        return;
    }
    LOGW("saves: %s is damaged (%zu bytes) and has no good backup", file.c_str(), data.size());
}

}  // namespace

void Guard(const std::filesystem::path& hdd) {
    fs::path title = hdd / "E" / "UDATA" / "4541007b";
    fs::path backups = fs::path(host::kShared) / "backups" / "saves";
    std::error_code ec;
    for (const auto& folder : fs::directory_iterator(title, ec)) {
        if (!folder.is_directory(ec)) continue;
        for (const auto& e : fs::directory_iterator(folder.path(), ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() == ".xbx") continue;
            Guard(e.path(), backups / folder.path().filename() / e.path().filename());
        }
        // the folder's dashboard files, for a profile put back by hand into a folder that went
        for (const char* meta : {"SaveMeta.xbx", "saveimage.xbx"}) {
            fs::path from = folder.path() / meta, to = backups / folder.path().filename() / meta;
            if (fs::exists(from, ec) && !fs::exists(to, ec)) fs::copy_file(from, to, ec);
        }
    }
}

}  // namespace saves
