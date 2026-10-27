#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

// state the headless host's parts share
namespace harness {

inline std::atomic<uint64_t> g_frames{0};  // frames presented

// an environment number, fallback when unset
inline long Env(const char* name, long fallback = 0) {
    const char* v = std::getenv(name);
    return v && *v ? std::atol(v) : fallback;
}

}  // namespace harness
