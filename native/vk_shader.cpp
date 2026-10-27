// HLSL to SPIR-V for the Vulkan renderer, through glslang's HLSL front end, with a cache on disk
#include "vk_shader.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "core/log.h"

namespace vkshader {

namespace {

std::filesystem::path g_dir;
std::mutex g_lock;
// bumped when the translation below changes, so older cached modules are not used
constexpr uint32_t kCacheVersion = 1;

// fxc's spellings glslang does not read: 1.#INF for infinity
std::string Adapt(const std::string& hlsl) {
    std::string s = hlsl;
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) s.replace(at, 6, "asfloat(0x7F800000u)");
    // the vertex inputs at the location of their register, ATTRn: the pipeline's attributes are numbered so
    static const std::regex attr(R"((\n[ \t]*)(\w+ v\d+ : ATTR(\d+);))");
    return std::regex_replace(s, attr, "$1[[vk::location($3)]] $2");
}

std::filesystem::path CachePath(uint64_t hash) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx_%u.spv", (unsigned long long)hash, kCacheVersion);
    return g_dir / name;
}

bool Load(uint64_t hash, std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return false;
    std::ifstream f(CachePath(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || n % 4) return false;
    spirv.resize(size_t(n) / 4);
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(spirv.data()), n));
}

void Store(uint64_t hash, const std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return;
    std::filesystem::path p = CachePath(hash), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
}

}  // namespace

void Init(const std::filesystem::path& cache_dir) {
    std::lock_guard lock(g_lock);
    glslang::InitializeProcess();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    g_dir = ec ? std::filesystem::path() : cache_dir;
}

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv) {
    // verbose builds: each shader's HLSL beside the cache, to read what a program became
    if (logx::kVerbose && !g_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_dir / "hlsl", ec);
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.hlsl", (unsigned long long)hash);
        std::filesystem::path p = g_dir / "hlsl" / name;
        if (!std::filesystem::exists(p, ec)) std::ofstream(p) << hlsl;
    }
    if (Load(hash, spirv)) return true;
    std::lock_guard lock(g_lock);
    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    std::string text = Adapt(hlsl);
    const char* src = text.c_str();
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingConstants);
    sh.setShiftBinding(glslang::EResTexture, kBindingTextures);
    sh.setShiftBinding(glslang::EResSampler, kBindingSamplers);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vulkan: %s %s does not compile: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, sh.getInfoLog());
        LOGE("%s", text.c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(messages) || !program.mapIO()) {
        LOGE("vulkan: %s does not link: %s", entry, program.getInfoLog());
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    if (spirv.empty()) return false;
    Store(hash, spirv);
    return true;
}

}  // namespace vkshader
