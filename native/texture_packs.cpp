// The texture packs' index (src/common/texture_packs.h): each folder of mods/textures is a pack, its info.txt written last
// by the Mods Loader (a pack without one is half written), its images named by the game texture's hash. Packs go in the
// order they came, info.txt's time, so a later pack's image is the one drawn.
#include "common/texture_packs.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>

#include "core/log.h"

namespace texpacks {
namespace {

std::map<uint32_t, std::string> g_paths;
std::vector<uint32_t> g_hashes;
std::mutex g_lock;
std::unordered_map<uint32_t, Bound> g_bound;
uint64_t g_generation = 0;

}  // namespace

void Load(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    struct Pack {
        fs::file_time_type time;
        fs::path path;
    };
    std::vector<Pack> packs;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        fs::path info = e.path() / "info.txt";
        if (!e.is_directory(ec) || !fs::exists(info, ec)) continue;
        packs.push_back({fs::last_write_time(info, ec), e.path()});
    }
    std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) {
        return a.time != b.time ? a.time < b.time : a.path < b.path;
    });
    for (const Pack& p : packs) {
        size_t n = 0;
        for (const fs::directory_entry& e : fs::directory_iterator(p.path, ec)) {
            std::string name = e.path().filename().string();
            uint32_t hash;
            char tail[8] = {};
            if (name.size() != 12 || std::sscanf(name.c_str(), "%8x.%3s", &hash, tail) != 2 || std::string(tail) != "dds") continue;
            g_paths[hash] = e.path().string();
            n++;
        }
        LOGR("texpacks: %s, %zu textures", p.path.filename().string().c_str(), n);
    }
    for (const auto& [hash, path] : g_paths) g_hashes.push_back(hash);
}

const std::vector<uint32_t>& Hashes() { return g_hashes; }

std::string PathOf(uint32_t hash) {
    auto it = g_paths.find(hash);
    return it == g_paths.end() ? std::string() : it->second;
}

void Publish(std::unordered_map<uint32_t, Bound>&& bound) {
    std::lock_guard<std::mutex> lock(g_lock);
    bool same = bound.size() == g_bound.size();
    for (auto it = bound.begin(); same && it != bound.end(); ++it) {
        auto o = g_bound.find(it->first);
        same = o != g_bound.end() && o->second.hash == it->second.hash && o->second.width == it->second.width &&
               o->second.height == it->second.height;
    }
    if (same) return;
    g_bound = std::move(bound);
    g_generation++;
}

uint64_t Generation() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_generation;
}

void Snapshot(std::unordered_map<uint32_t, Bound>& out, uint64_t& generation) {
    std::lock_guard<std::mutex> lock(g_lock);
    out = g_bound;
    generation = g_generation;
}

}  // namespace texpacks
