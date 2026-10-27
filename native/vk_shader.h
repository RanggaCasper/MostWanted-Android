#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The renderer's HLSL (the SDK's translated shaders and the full screen passes) to SPIR-V with glslang, and a cache
// of the results on disk: compiling is slow on a phone and the same shaders come back every run.
// Bindings, one descriptor set: b0 and b1 at 0 and 1, t0 to t3 at 2 to 5, s0 to s3 at 6 to 9.
namespace vkshader {

constexpr uint32_t kBindingConstants = 0;  // b0
constexpr uint32_t kBindingTextures = 2;   // t0
constexpr uint32_t kBindingSamplers = 6;   // s0
constexpr uint32_t kBindings = 10;

enum class Stage { Vertex, Pixel };

void Init(const std::filesystem::path& cache_dir);
// the SPIR-V of hlsl's entry point, named "main" in the module; false after logging why
bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv);

}  // namespace vkshader
