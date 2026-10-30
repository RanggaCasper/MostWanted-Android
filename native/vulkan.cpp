// The title's Direct3D 8 calls drawn with Vulkan 1.1, dynamic rendering and a timeline semaphore. The SDK's
// translated shaders are HLSL, which glslang turns into SPIR-V (vk_shader.cpp).
#include <vulkan/vulkan.h>
#include <pthread.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dxgiformat.h>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "common/aspect.h"
#include "core/diag.h"
#include "core/log.h"
#include "d3d/gpu_frame.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "vk_shader.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr VkDeviceSize kUploadSize = 32ull << 20;  // per frame: vertices, indices, constants, texture staging
constexpr VkDeviceSize kUboRange = 3072;           // b0 and b1: 192 vertex constants, DrawConstants under it
constexpr VkDeviceSize kUboAlign = 256;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kSetsPerPool = 1024;
constexpr uint32_t kPassStamps = 256;  // perf diagnostic: renderings timed in a listed frame
// the scale the game's effect buffers are drawn at, at most (VulkanBackend::TargetScale)
constexpr float kEffectScale = 1.0f;
static_assert(sizeof(DrawConstants) <= kUboRange);

// perf diagnostic: nanoseconds since t, for g_perf
uint64_t SinceNs(std::chrono::steady_clock::time_point t) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the present with the screen filter or anti-aliasing the player chose: the blit's, then post.x a filter (1 sharpen,
// 2 vivid, 3 scanlines, 4 CRT) and post.y FXAA; src.xy a texel, src.z the frame's lines
const char* kPresentHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 src; float4 post; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 Tap(float2 uv) { return img.SampleLevel(ls, uv, 0).rgb; }
// FXAA's console form: the edge's direction from the corners' lumas, two blends along it, the wider kept unless it
// leaves the local range
float3 Fxaa(float2 uv) {
    float2 r = src.xy;
    float3 m = Tap(uv);
    float nw = Luma(Tap(uv + float2(-1, -1) * r)), ne = Luma(Tap(uv + float2(1, -1) * r));
    float sw = Luma(Tap(uv + float2(-1, 1) * r)), se = Luma(Tap(uv + float2(1, 1) * r));
    float lm = Luma(m);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.0312, hi * 0.125)) return m;
    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 0.0078125);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * r;
    float3 a = 0.5 * (Tap(uv + dir * (1.0 / 3.0 - 0.5)) + Tap(uv + dir * (2.0 / 3.0 - 0.5)));
    float3 b = a * 0.5 + 0.25 * (Tap(uv - dir * 0.5) + Tap(uv + dir * 0.5));
    float lb = Luma(b);
    return (lb < lo || lb > hi) ? a : b;
}
// the frame at uv: through FXAA, or the box of n x n texels a supersampled frame has per pixel
float3 Source(float2 uv, float2 px) {
    if (post.y > 0) return Fxaa(uv);
    int n = max((int)opts.z, 1);
    float3 c = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += Tap(uv + px * ((float2(x, y) + 0.5) / n - 0.5));
    return c / (n * n);
}
// the cross's difference added back, held within the neighbours' range so edges get no halo
float3 Sharpen(float2 uv, float3 c) {
    float2 r = src.xy * max(opts.z, 1.0);
    float3 n = Tap(uv + float2(0, -r.y)), s = Tap(uv + float2(0, r.y));
    float3 w = Tap(uv + float2(-r.x, 0)), e = Tap(uv + float2(r.x, 0));
    float3 lo = min(c, min(min(n, s), min(w, e))), hi = max(c, max(max(n, s), max(w, e)));
    return clamp(c + (4.0 * c - n - s - w - e) * 0.35, lo, hi);
}
float3 Vivid(float3 c) {
    c = lerp(Luma(c).xxx, c, 1.3);
    return lerp(c, smoothstep(0.0, 1.0, saturate(c)), 0.35);
}
// overbright colours held back: Reinhard's curve over the picture, the shadows' saturation kept and their lift taken
// back down so they stay deep (a player's GLSL filter)
float3 Hdr(float3 c) {
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    float shadow = 1.0 - smoothstep(0.0, 0.8, l);
    c = lerp(l.xxx, c, 1.0 + shadow);
    float3 mapped = pow(max(c / (c + 1.0), 0.0), 1.0 / 2.2);
    c = lerp(c, mapped, 0.8);
    return c * (1.0 - shadow * 0.5);
}
// dark between the frame's lines, as a TV's beam leaves them
float Scan(float y, float depth) {
    float s = sin(frac(y * src.z) * 3.14159265);
    return lerp(1.0 - depth, 1.0, s * s);
}
float4 psmain(O i) : SV_Target {
    float2 q = (i.uv - uvrect.xy) / uvrect.zw;
    float2 px = fwidth(i.uv);
    int mode = (int)post.x;
    if (mode == 4) {
        // the tube's bulge, black past its edge
        float2 cq = q * 2.0 - 1.0;
        cq *= 1.0 + cq.yx * cq.yx * float2(0.045, 0.06);
        q = cq * 0.5 + 0.5;
        if (any(q < 0.0) || any(q > 1.0)) return float4(0, 0, 0, 1);
    }
    float2 uv = uvrect.xy + q * uvrect.zw;
    float3 c = Source(uv, px);
    if (mode == 1) c = Sharpen(uv, c);
    else if (mode == 2) c = Vivid(c);
    else if (mode == 3) c *= Scan(q.y, 0.3) * 1.12;
    else if (mode == 4) {
        // glow from the texels around, the lines, the shadow mask's stripes by screen pixel, the corners darker
        float2 r = src.xy * 2.5;
        float3 glow = (Tap(uv + float2(r.x, 0)) + Tap(uv - float2(r.x, 0)) + Tap(uv + float2(0, r.y)) +
                       Tap(uv - float2(0, r.y))) * 0.25;
        c = lerp(c, max(c, glow), 0.35);
        c *= Scan(q.y, 0.45);
        float k = fmod(floor(i.p.x), 3.0);
        c *= k < 1.0 ? float3(1.0, 0.82, 0.82) : (k < 2.0 ? float3(0.82, 1.0, 0.82) : float3(0.82, 0.82, 1.0));
        float v = pow(saturate(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y)), 0.18);
        c *= v * 1.38;
    }
    c = saturate(c) * opts.w;
    if (opts.x > 0) c = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    // after the game's gamma ramp, so a brightness set high is held back too
    if (mode == 5) c = saturate(Hdr(c));
    return float4(c, 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// ---- the console's values as Vulkan's

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to ALWAYS 0x207: VkCompareOp has the same order, and the shaders take the same numbers
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// ---- DXT on the CPU, for GPUs without BC textures (most phones)

void Rgb565(uint16_t c, uint8_t* bgr) {
    bgr[2] = uint8_t((c >> 11) * 255 / 31);
    bgr[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    bgr[0] = uint8_t((c & 31) * 255 / 31);
}

// one 4x4 block's colours, BGRA, out 16 x 4 bytes; dxt1 alpha for the 3 colour mode
void ColourBlock(const uint8_t* b, bool dxt1, uint8_t* out) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4] = {};
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int i = 0; i < 4; i++) pal[i][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out + i * 4, pal[(bits >> (i * 2)) & 3], 4);
}

// a level of DXT1, 3 or 5 blocks (bw x bh per slice, row_pitch bytes a block row) into w x h BGRA texels
void DecodeDxt(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h, uint32_t z,
               std::vector<uint8_t>& out) {
    uint32_t block = host == Host::BC1 ? 8 : 16;
    uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
    out.assign(size_t(w) * h * z * 4, 0);
    uint8_t px[64];
    for (uint32_t s = 0; s < z; s++)
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                size_t at = (size_t(s) * bh + by) * row_pitch + size_t(bx) * block;
                if (at + block > in.size()) continue;
                const uint8_t* b = &in[at];
                ColourBlock(host == Host::BC1 ? b : b + 8, host == Host::BC1, px);
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                } else if (host == Host::BC3) {
                    uint8_t a[8] = {b[0], b[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                    } else {
                        for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) px[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++)
                        std::memcpy(&out[((size_t(s) * h + by * 4 + y) * w + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
            }
}

// ---- device memory: 64 MB blocks per memory type, first fit inside them; big allocations get their own

class Allocator {
public:
    struct Alloc {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize off = 0, size = 0;
        uint8_t* map = nullptr;
        int block = -1;  // -1: memory of its own
    };

    void Init(VkPhysicalDevice pd, VkDevice dev) {
        dev_ = dev;
        vkGetPhysicalDeviceMemoryProperties(pd, &props_);
    }

    bool Get(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool linear, Alloc& out) {
        int type = Type(r.memoryTypeBits, want | nice);
        if (type < 0) type = Type(r.memoryTypeBits, want);
        if (type < 0) return false;
        bool host = props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        std::lock_guard lock(m_);
        if (r.size > kBlock / 4) {
            out = {};
            if (!Allocate(uint32_t(type), r.size, out.mem)) return false;
            out.size = r.size;
            if (host) vkMapMemory(dev_, out.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.map));
            return true;
        }
        for (size_t i = 0; i < blocks_.size(); i++) {
            Block& b = blocks_[i];
            if (b.type != uint32_t(type) || b.linear != linear) continue;
            if (Carve(b, r, out)) {
                out.block = int(i);
                return true;
            }
        }
        Block b;
        b.type = uint32_t(type);
        b.linear = linear;
        if (!Allocate(b.type, kBlock, b.mem)) return false;
        if (host) vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.map));
        b.free[0] = kBlock;
        blocks_.push_back(std::move(b));
        Carve(blocks_.back(), r, out);
        out.block = int(blocks_.size() - 1);
        return true;
    }

    void Free(Alloc& a) {
        if (!a.mem) return;
        std::lock_guard lock(m_);
        if (a.block < 0) {
            vkFreeMemory(dev_, a.mem, nullptr);
        } else {
            auto& free = blocks_[size_t(a.block)].free;
            auto it = free.emplace(a.off, a.size).first;
            if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
                it->second += next->second;
                free.erase(next);
            }
            if (it != free.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second == it->first) {
                    prev->second += it->second;
                    free.erase(it);
                }
            }
        }
        a = {};
    }

private:
    static constexpr VkDeviceSize kBlock = 64ull << 20;
    struct Block {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t type = 0;
        bool linear = false;
        uint8_t* map = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset to size
    };

    int Type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++)
            if ((bits >> i & 1) && (props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool Allocate(uint32_t type, VkDeviceSize size, VkDeviceMemory& mem) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        own_type_ = type;
        if (vkAllocateMemory(dev_, &ai, nullptr, &mem) == VK_SUCCESS) return true;
        LOGE("vulkan: out of device memory (%llu bytes)", (unsigned long long)size);
        return false;
    }

    bool Carve(Block& b, const VkMemoryRequirements& r, Alloc& out) {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            VkDeviceSize o = it->first, s = it->second;
            VkDeviceSize at = (o + r.alignment - 1) / r.alignment * r.alignment;
            if (at + r.size > o + s) continue;
            b.free.erase(it);
            if (at > o) b.free[o] = at - o;
            if (at + r.size < o + s) b.free[at + r.size] = o + s - at - r.size;
            out.mem = b.mem;
            out.off = at;
            out.size = r.size;
            out.map = b.map ? b.map + at : nullptr;
            return true;
        }
        return false;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    std::vector<Block> blocks_;
    uint32_t own_type_ = 0;
    std::mutex m_;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // sampled: every level, a depth image's depth alone
    VkImageView attach = VK_NULL_HANDLE;  // as a render target: level 0, depth and stencil
    Allocator::Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth = false, stencil = false;
    VkImageAspectFlags pending = 0;  // a whole clear left for the next rendering on it to load (ClearTarget)
    VkClearValue clear{};
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    Allocator::Alloc mem;
};

// a console surface the renderer draws into
struct Target {
    Image img;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    uint64_t writes = 0;          // draws, clears and copies into it: a cube of targets copies faces that moved on
    std::vector<uint8_t> read;    // what Sync left in guest memory
    Image fb;                     // copy read by draws that sample the target they render to
};

// a cube whose faces are targets (Most Wanted's car reflection, six 128x128 faces drawn each frame), copied together
struct CubeOfTargets {
    Image img;
    uint64_t writes[6] = {};  // each face's Target::writes when last copied
};

struct Tex {
    Image img;
    TextureDesc d;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool decode = false;  // DXT decoded on the CPU
    uint32_t levels = 1;
    uint64_t sig = 0;     // sampled, every frame it is drawn
    uint64_t whole = 0;   // all of it, every few frames (BindGuestTexture)
    uint8_t calm = 0;     // whole hashes in a row that found it the same
    uint64_t frame = 0;
    uint64_t palette = 0;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;  // Topology, strip at bit 4
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t strides[16];
    uint32_t buffers;
    uint32_t blend, src, dst, op, wmask;
    uint32_t zen, zwrite, zfunc;
    uint32_t sten, sfunc, smask, swmask, sfail, szfail, spass;
    uint32_t cull, ccw, fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    bool over = false;  // premultiplied over the target, which keeps its alpha
};

struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};
    VkFormat fmt[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    bool blur[4] = {};
    bool small = false;  // a stage reads a target smaller than the frame
};

struct DrawShaders {
    bool prog = false;
    bool fixed_transform = false;
    FixedKey fk;
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w, h;
};

// ---- what the console's state asks of a draw

void ReplaceOnce(std::string& src, const std::string& from, const std::string& to) {
    size_t at = src.find(from);
    if (at != std::string::npos) src.replace(at, from.size(), to);
}

// the game's 2D layer leaves oPos.w at 0: screen space, drawn with w = 1 as XYZRHW is
std::string VertexProgram(const VertexShader& vs, uint16_t regs, uint64_t decode) {
    std::string src = VertexProgramHlsl(vs.program, regs, decode);
    ReplaceOnce(src, "    p.w = magclamp(p.w);\n", "    p.w = p.w == 0 ? 1 : magclamp(p.w);\n");
    return src;
}

// a point list writes its size: Vulkan leaves it undefined otherwise; empty when the source has no main to wrap
std::string PointsHlsl(const std::string& src) {
    size_t at = src.find("VOut main(");
    if (at == std::string::npos) return {};
    size_t p = at + 10, sp = src.find(' ', p);
    std::string type = src.substr(p, sp - p);
    // the module's entry point is named main: the program's own main goes by another name
    std::string out = src;
    out.replace(at, 10, "VOut vs_body(");
    return out + "\nVOut main_points(" + type + " i, out float psize : PSIZE) { psize = 1; return vs_body(i); }\n";
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f, kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

// whole up to whole_max bytes, else sampled
uint64_t MemSig(uint32_t data, uint32_t total, uint32_t whole_max = kSigWholeMax) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= whole_max) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4, pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    return false;
}

bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++)
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    return px;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

DrawShaders ClassifyShaders(const State& s, const Assembled& a) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kPointsSalt = 0x9017;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (a.topology == Topology::Points) vkey ^= kPointsSalt;
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgram(*s.vs, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to Vulkan's RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    constexpr uint32_t kCullCw = 0x900;
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

constexpr float kAlphaTestAlways = 7.0f;
constexpr uint32_t kBlendOne = 1;  // D3DBLEND_ONE

void FillFogConstants(const State& s, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    // D3DFOG_NONE with fog on is vertex fog: a vertex program's oFog is the factor (4, sdk/runtime/gpu/shaders.cpp)
    c.fog[3] = s.rs[kRsFogTableMode] ? float(s.rs[kRsFogTableMode]) : 4.0f;
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;
constexpr float kBlurTapTexels = 7.0f / 6;

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, the host half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    c.texsize[i][2] = -0.5f / (float(d.width) * k);
    c.texsize[i][3] = -0.5f / (float(d.height) * k);
    if (stages.wide[i] > 0) {
        bool blur = stages.blur[i];
        float fw = blur ? kBlurTapTexels : kGlowBoxTexels;
        float per_texel = blur ? 1.0f : 0.5f;
        float n = std::ceil(fw * stages.wide[i] * per_texel);
        c.spread[i][0] = fw / (n * float(d.width) * k);
        c.spread[i][1] = fw / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void LightToView(const std::array<uint32_t, 26>& l, const float* vm, float (&o)[7][4]) {
    float f[26];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++)
        if ((fk.types >> (i * 2)) & 3) LightToView(s.lights[size_t(i)], vm, c.lt[i]);
}

struct GuestRect {
    int32_t left, top, right, bottom;
};
GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * 16;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}
void ReadGuestPoint(uint32_t points, uint32_t i, int32_t& x, int32_t& y) {
    x = int32_t(kernel::R32(points + i * 8));
    y = int32_t(kernel::R32(points + i * 8 + 4));
}
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// ---- layouts: the stages and accesses an image's layout stands for

void LayoutMasks(VkImageLayout l, VkPipelineStageFlags& st, VkAccessFlags& ac) {
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            ac = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            st = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            ac = VK_ACCESS_SHADER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            ac = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            st = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            ac = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            st = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            ac = 0;
            break;
        default:
            st = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            ac = 0;
            break;
    }
}

bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override { if (!gpu_failed_) Submit(true, false); }
    void SetGamma(const uint8_t* ramp) override {
        if (gpu_failed_) return;
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void WaitGpuFrame(uint64_t n);

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        Buffer upload;
        VkDeviceSize used = 0;
        uint64_t value = 0;  // the timeline's value once the GPU is past this frame
        std::vector<Image> images;
        std::vector<Buffer> buffers;
        std::vector<VkDescriptorPool> pools;
        size_t pool_at = 0;  // the descriptor pool sets come from
        // sets kept from frame to frame: the same views and samplers come back every frame, and writing a set was a
        // tenth of the render thread. A view destroyed may have its handle reused, so a set made before is let go
        // (set_gen against view_gen_) and the pools are reset once the GPU is past them (pools_dirty)
        std::unordered_map<uint64_t, VkDescriptorSet> sets;
        uint64_t set_gen = 0;
        bool pools_dirty = false;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkQueryPool times = VK_NULL_HANDLE;  // the command buffer's start and end on the GPU, for its load
        bool timed = false;                  // submitted with both written
        bool pool_clean = true;              // the pool reset, ready to record; under reset_m_
    };
    struct Alloc {
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* cpu;
    };
    struct Late {
        Buffer rb[kFrames];
        Image down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    VkCommandBuffer Cb() { return CurrentFrame().cb; }

    // A presented frame's pool is reset on a thread of its own once the GPU is past it. Adreno's reset gives the
    // buffer's GPU memory back to the kernel and recording maps it again: a tenth of the render thread in a race.
    // Only with the timeline semaphore: the fence path's wait holds the lock the render thread submits under
    void ResetPools();
    void QueuePoolReset(Frame& f);
    void AwaitPoolReset(Frame& f);
    std::mutex reset_m_;
    std::condition_variable reset_work_cv_;
    std::condition_variable reset_done_cv_;
    std::deque<Frame*> resets_;

    // Init, in order
    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayouts();
    bool CreateFrames();
    bool CreateFixedImages();
    bool InitUi();

    // memory and objects
    bool CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, VkImageViewType vt, bool cube = false);
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void Destroy(Image& im);
    void Destroy(Buffer& b);
    void Retire(Image& im) {
        if (rend_color_ == &im || rend_depth_ == &im) EndRender();
        if (im) CurrentFrame().images.push_back(im);
        im = {};
    }
    void Retire(Buffer& b) {
        if (b.buf) CurrentFrame().buffers.push_back(b);
        b = {};
    }
    Alloc Upload(VkDeviceSize size, VkDeviceSize align = 16);
    Alloc Constants(const void* data, size_t bytes);

    // command recording
    void Begin();
    bool Submit(bool wait, bool present);
    bool WaitValue(uint64_t v);
    uint64_t Completed();
    VkFence SubmitFence(VkResult& result);
    void FailGpu(const char* operation, VkResult result, uint64_t value);
    void PollFences();
    bool ResetFrame(Frame& f);
    void Transition(Image& im, VkImageLayout to);
    void BeginRender(Image* color, Image* depth);
    void EndRender();
    void FlushClear(Image& im);
    // without dynamic rendering: render passes that only load and store, and framebuffers per pair of views
    VkRenderPass Pass(VkFormat color, VkFormat depth, VkImageAspectFlags clears = 0);
    VkFramebuffer Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                              uint32_t w, uint32_t h);
    void DropFramebuffers(VkImageView v);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    VkDescriptorSet Set(const VkImageView views[4], const VkSampler samplers[4]);
    void BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc);

    // shaders and pipelines
    VkShaderModule Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    VkPipeline FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth, bool over);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    VkPipeline BuildPipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkRenderPass pass);
    VkRenderPass PipelinePass(const PsoKey& k);
    VkSampler Sampler(const VkSamplerCreateInfo& ci);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    float TargetScale(uint64_t key, uint32_t gw, uint32_t gh) const;
    Target* EnsureTarget(const Surface& s, bool depth);
    Target* FrameRegion(const Surface& s, uint32_t& rows, uint32_t& sub_h);
    Target* EnsureColor(const Surface& s, uint32_t& rows, uint32_t& sub_h);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    bool PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o);
    Image* Feedback(Target& t);
    bool DepthCopy(const State& s);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps = 1);
    Image* CopySource(Target& t, Image& down);
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    VkImageView BindTexture(const State& s, int stage, DrawTextures& stages);
    VkImageView BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                  DrawTextures& stages);
    VkImageView BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                 DrawTextures& stages);
    VkImageView BindCubeTargets(int stage, const TextureDesc& d, DrawTextures& stages);
    VkImageView BindFrameRegion(int stage, const TextureDesc& d, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil, uint32_t rows = 0, uint32_t sub_h = 0);

    // present
    bool AcquireImage(uint32_t& index);
    void DrawFrame(Target* t, uint32_t index);
    void RenderUi();
    void UploadGammaRamp();
    void ReleaseOldTextures();
    void ApplySettings();
    void SavePipelineCache();
    // SHADER PRELOADING
    void LoadPreloadList();
    void StartPreload();
    void Preload(std::vector<std::pair<uint64_t, uint64_t>> shaders, std::vector<PsoKey> pipes,
                 std::unordered_map<uint64_t, VkRenderPass> passes);
    bool TakePreloadedShader(uint64_t key, uint64_t& hash, VkShaderModule& module);
    void Record(uint32_t tag, const void* data, size_t size);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    uint32_t surface_epoch_ = 0;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceFeatures feats_{};
    uint32_t api_ = VK_API_VERSION_1_1;
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::vector<const char*> dev_exts_;
    bool bc_ = false;
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_linear_ = false;
    bool r32f_linear_ = false;
    Allocator alloc_;
    PFN_vkCmdBeginRenderingKHR begin_rendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR end_rendering_ = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_ = nullptr;
    PFN_vkGetSemaphoreCounterValueKHR counter_value_ = nullptr;
    bool dynamic_ = true;  // dynamic rendering; Vulkan 1.1 drivers (older Adrenos) only have render passes
    bool timeline_ok_ = true;  // without it a fence per submit stands in for the timeline's values
    std::atomic<bool> gpu_failed_{false};
    struct Pending {
        uint64_t value;
        VkFence fence;
    };
    std::mutex fence_lock_;  // the game thread waits on frames too
    std::vector<Pending> pending_;
    std::vector<VkFence> spare_fences_;
    uint64_t fenced_ = 0;  // the highest value whose fence signalled
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    struct Fb {
        VkImageView color, depth;
        VkFramebuffer fb;
    };
    std::unordered_map<uint64_t, Fb> framebuffers_;

    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent_{};
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    std::vector<VkSemaphore> swap_done_;
    bool vsync_ = true;
    bool swap_stale_ = false;
    bool surface_lost_ = false;  // the driver said the window is gone: the surface is made again too
    uint32_t surface_failures_ = 0;  // presents in a row without a surface, for the log
    uint32_t present_index_ = 0;  // the image the frame's submit signals the present of

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    size_t cache_saved_ = 0;
    // Shader preloading: the shaders (their key to their source's hash) and pipelines (their keys) drawn with in earlier
    // runs, listed in shadercache/preload.bin as they are first made. With SHADER PRELOADING on, a thread of its own
    // makes them all from the cached SPIR-V once the game starts, and the render thread takes them as it meets them
    // instead of making them mid-race (what stutters). The list is kept whether it is on or not
    std::vector<std::pair<uint64_t, uint64_t>> listed_shaders_;
    std::vector<PsoKey> listed_pipes_;
    std::unordered_set<uint64_t> listed_;  // the shaders' keys and the pipelines' hashes in the list
    std::ofstream list_;
    bool preload_started_ = false;
    std::mutex pre_lock_;  // the preload thread's, below
    std::unordered_map<uint64_t, uint64_t> pre_shaders_;  // a shader's key to its source's hash, its module made
    std::unordered_map<uint64_t, VkShaderModule> pre_modules_;
    std::unordered_map<uint64_t, VkPipeline> pre_pipes_;
    std::unordered_set<uint64_t> pre_claimed_;  // pipelines the render thread made itself first
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> value_{0};
    Frame frames_[kFrames];
    uint64_t frame_ = 0;

    // the rendering under way: its attachments
    bool rendering_ = false;
    Image* rend_color_ = nullptr;
    Image* rend_depth_ = nullptr;
    uint32_t rend_w_ = 0, rend_h_ = 0;  // its area

    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_set<uint64_t> full_scale_;  // effect sized targets drawn with a frame sized one: at its scale
    uint32_t presented_[4] = {};  // the frame buffers presented, their data: FrameRegion's parents
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint32_t, CubeOfTargets> cubes_;
    std::unordered_map<uint32_t, Image> regions_;  // a frame's rows read as a texture, by its data (BindFrameRegion)
    uint64_t view_gen_ = 1;  // image views destroyed so far (Frame::sets)
    // the last draw's keys, results and vertex constants: consecutive draws mostly share their shaders and pipeline,
    // and many their constants, so the hashing, the lookups and the 3 KB upload are skipped for them
    PixelKey last_pk_{};
    uint64_t last_pkey_ = 0, last_ph_ = 0;
    VkShaderModule last_ps_ = VK_NULL_HANDLE;
    PsoKey last_pso_{};
    VkPipeline last_pipe_ = VK_NULL_HANDLE;
    std::array<float, 192 * 4> last_vsc_{};  // without the render thread's count (g_vsc_counted) only
    uint64_t last_vsc_changes_ = 0;
    Alloc last_vc_{};
    const void* last_vc_frame_ = nullptr;
    uint64_t last_vc_epoch_ = 0, upload_epoch_ = 1;
    // samplers lately asked for, most recent first: a draw asks for four
    struct RecentSampler {
        uint8_t ci[sizeof(VkSamplerCreateInfo)];
        VkSampler sampler;
    };
    std::array<RecentSampler, 8> recent_samplers_{};
    size_t recent_count_ = 0;
    Image lut_;
    Image dummy_[3];  // 2D, 3D, cube
    // what a reflection the game no longer draws is read as, opaque: the car paint and road shaders weigh by its alpha.
    // A dark grey cube for CAR REFLECTIONS off, black for ROAD REFLECTIONS off
    Image env_off_;
    Image road_off_;
    VkSampler linear_ = VK_NULL_HANDLE, point_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ = false;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    Assembled asm_;
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_value_[kFrames + 1] = {};
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
    uint32_t time_bits_ = 0;  // the queue's valid timestamp bits, 0 without timestamps
    double gpu_ns_ = 0;       // GPU time of the command buffers done since the load was last worked out
    bool pass_log_ = false;   // perf diagnostic: this frame's renderings are listed, with their GPU time
    std::vector<std::string> pass_list_;
    VkQueryPool pass_times_ = VK_NULL_HANDLE;
    uint32_t pass_stamps_ = 0;
};

// percent of the time the GPU was running the frames' commands, -1 without timestamps; host::VulkanGpuLoad
std::atomic<float> g_gpu_load{-1};

#define VK_CHECK(x)                                                    \
    do {                                                               \
        VkResult _r = (x);                                             \
        if (_r != VK_SUCCESS) {                                        \
            LOGE("vulkan: %s failed (%d)", #x, int(_r));               \
            return false;                                              \
        }                                                              \
    } while (0)

// ---- Init

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    vkshader::Init(app::ExeDir() / "shadercache");
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    alloc_.Init(pd_, dev_);
    if (!CreateLayouts() || !CreateFrames() || !CreateSwapchain() || !CreateFixedImages()) return false;
    Begin();
    if (gpu_failed_) return false;
    ui_ = InitUi();
    scale_ = app::RenderScale(guest_h_);
    LOGI("vulkan: %ux%u guest, %ux%u screen, render scale %.3f", width, height, swap_extent_.width,
         swap_extent_.height, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    uint32_t n = 0;
    const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> exts(sdl, sdl + n);
    uint32_t loader = VK_API_VERSION_1_1;
    vkEnumerateInstanceVersion(&loader);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app::kGame.log;
    ai.pEngineName = "XboxXBE-Recomp";
    ai.apiVersion = std::min<uint32_t>(loader, VK_API_VERSION_1_3);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

// the epoch is taken only once the surface is made: until then each present tries again
bool VulkanBackend::CreateSurface() {
    uint32_t epoch = host::SurfaceEpoch();
    // Android has not handed the window back yet (resuming, or a surface change): wait for it without asking SDL,
    // which only fails, and log the outage once, not every present
    SDL_PropertiesID props = SDL_GetWindowProperties(host::Window());
    if (!SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr)) {
        if (surface_failures_++ == 0) LOGI("vulkan: waiting for Android's window");
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(host::Window(), instance_, nullptr, &surface_)) {
        surface_ = VK_NULL_HANDLE;
        if (surface_failures_++ == 0) LOGE("vulkan: no surface: %s", SDL_GetError());
        return false;
    }
    if (surface_failures_) LOGI("vulkan: surface made after %u tries", surface_failures_ + 1);
    surface_failures_ = 0;
    surface_epoch_ = epoch;
    surface_lost_ = false;
    return true;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}

bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    // rejections are reported, not warned: release logs drop warnings and these are what a bug report needs
    if (pds.empty()) LOGE("vulkan: the driver lists no GPU");
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        LOGR("GPU: %s, Vulkan %u.%u.%u, driver %08X", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.apiVersion < VK_API_VERSION_1_1) {
            LOGE("vulkan: %s is below Vulkan 1.1", p.deviceName);
            continue;
        }
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ext(en);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ext.data());
        uint32_t api = std::min<uint32_t>(p.apiVersion, VK_API_VERSION_1_3);
        bool swapchain = HasExtension(ext, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        bool dynamic = api >= VK_API_VERSION_1_3 || HasExtension(ext, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        // Adreno's vkCmdBeginRendering allocates on every call: a sixth of the render thread in the rain, where Most
        // Wanted's screen drops ping-pong between the frame and a 32x32 target 20 times a frame. Its render passes,
        // made once and cleared by their load ops, cost a quarter of that
        constexpr uint32_t kQualcomm = 0x5143;
        if (dynamic && p.vendorID == kQualcomm) {
            dynamic = false;
            LOGR("vulkan: Adreno, render passes over dynamic rendering");
        }
        if (!swapchain) {
            LOGE("vulkan: %s lacks swapchain", p.deviceName);
            continue;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            pd_ = pd;
            props_ = p;
            api_ = api;
            qfam_ = q;
            time_bits_ = qs[q].timestampValidBits;
            dev_exts_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            auto want = [&](const char* name) {
                if (HasExtension(ext, name)) dev_exts_.push_back(name);
            };
            if (api < VK_API_VERSION_1_3) {
                want(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                want(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                want(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
            }
            dynamic_ = dynamic;
            // the 1.1 drivers' timeline extension lost the device on the first submit (Mali-G76): fences there
            timeline_ok_ = api >= VK_API_VERSION_1_2;
            if (!dynamic_) LOGR("vulkan: no dynamic rendering, using render passes");
            if (!timeline_ok_) LOGR("vulkan: Vulkan 1.1, using fences for timeline semaphores");
            return true;
        }
        LOGE("vulkan: %s has no graphics queue that can present", p.deviceName);
    }
    LOGE("vulkan: no GPU with Vulkan 1.1 that can present");
    return false;
}

bool VulkanBackend::CreateDevice() {
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR ts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    // the chain holds only what is used: a 1.1 driver may not know the rest
    auto chain = [&]() -> void* {
        ts.pNext = nullptr;
        dr.pNext = timeline_ok_ ? &ts : nullptr;
        return dynamic_ ? (void*)&dr : timeline_ok_ ? (void*)&ts : nullptr;
    };
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    vkGetPhysicalDeviceFeatures2(pd_, &f2);
    if (timeline_ok_ && !ts.timelineSemaphore) {
        LOGR("vulkan: timeline semaphores not supported, using fences");
        timeline_ok_ = false;
    }
    if (dynamic_ && !dr.dynamicRendering) {
        LOGR("vulkan: dynamic rendering listed but not supported, using render passes");
        dynamic_ = false;
    }
    feats_ = {};
    feats_.samplerAnisotropy = f2.features.samplerAnisotropy;
    feats_.fillModeNonSolid = f2.features.fillModeNonSolid;
    feats_.textureCompressionBC = f2.features.textureCompressionBC;
    feats_.depthClamp = f2.features.depthClamp;
    bc_ = feats_.textureCompressionBC;
    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain()};
    on.features = feats_;
    dr.dynamicRendering = VK_TRUE;
    ts.timelineSemaphore = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &on};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(dev_exts_.size());
    ci.ppEnabledExtensionNames = dev_exts_.data();
    VK_CHECK(vkCreateDevice(pd_, &ci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    bool core13 = api_ >= VK_API_VERSION_1_3, core12 = api_ >= VK_API_VERSION_1_2;
    begin_rendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR"));
    end_rendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(dev_, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR"));
    wait_semaphores_ = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkWaitSemaphores" : "vkWaitSemaphoresKHR"));
    counter_value_ = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
        vkGetDeviceProcAddr(dev_, core12 ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR"));
    if ((dynamic_ && (!begin_rendering_ || !end_rendering_)) || (timeline_ok_ && (!wait_semaphores_ || !counter_value_))) {
        LOGE("vulkan: device functions missing");
        return false;
    }
    // the depth buffer: 24 bit with stencil as on the console where the GPU has it
    auto usable = [&](VkFormat f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd_, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (fp.optimalTilingFeatures & need) == need;
    };
    depth_fmt_ = usable(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd_, depth_fmt_, &fp);
    depth_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    vkGetPhysicalDeviceFormatProperties(pd_, VK_FORMAT_R32_SFLOAT, &fp);
    r32f_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    LOGI("vulkan: depth %s, BC textures %s", depth_fmt_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         bc_ ? "on the GPU" : "decoded on the CPU");
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    if (!surface_) return false;
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surface_, &caps);
    if (cr != VK_SUCCESS) {
        if (cr == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
        LOGE("vulkan: surface capabilities failed (%d)", int(cr));
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    // the window's own size, landscape; Android's compositor turns the picture with the screen
    int w, h;
    host::WindowPixels(w, h);
    if (w <= 0 || h <= 0) {
        w = int(caps.currentExtent.width);
        h = int(caps.currentExtent.height);
    }
    if (w <= 0 || h <= 0) return false;
    vsync_ = app::Get().vsync;
    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = {uint32_t(w), uint32_t(h)};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    // retiring the old chain gives its images back to the window: the GPU must be done drawing them
    if (swap_) vkQueueWaitIdle(queue_);
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult sr = vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh);
    if (sr != VK_SUCCESS) {
        if (sr == VK_ERROR_SURFACE_LOST_KHR || sr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) surface_lost_ = true;
        LOGE("vulkan: swap chain %dx%d failed (%d)", w, h, int(sr));
        return false;
    }
    DestroySwapchain();
    swap_ = fresh;
    swap_fmt_ = chosen.format;
    swap_extent_ = ci.imageExtent;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    swap_images_.resize(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, swap_images_.data());
    for (VkImage im : swap_images_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev_, &vi, nullptr, &v);
        swap_views_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        swap_done_.push_back(s);
    }
    swap_stale_ = false;
    LOGI("vulkan: swap chain %ux%u, %u images, %s", w, h, count, vsync_ ? "vsync" : "no vsync");
    return true;
}

// the old chain's views and semaphores; the chain itself when it is not being replaced
void VulkanBackend::DestroySwapchain() {
    if (!swap_) return;
    vkQueueWaitIdle(queue_);
    for (VkImageView v : swap_views_) {
        DropFramebuffers(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    for (VkSemaphore s : swap_done_) vkDestroySemaphore(dev_, s, nullptr);
    swap_views_.clear();
    swap_done_.clear();
    swap_images_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[vkshader::kBindings] = {};
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                              : i < vkshader::kBindingSamplers ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                               : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = vkshader::kBindings;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_));
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_));
    // pipelines from earlier runs: building them is what stutters on a phone
    std::vector<char> blob;
    std::ifstream f(app::ExeDir() / "shadercache" / "pipelines.bin", std::ios::binary);
    if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    if (vkCreatePipelineCache(dev_, &ci, nullptr, &cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(dev_, &ci, nullptr, &cache_));
    }
    cache_saved_ = blob.size();
    LoadPreloadList();
    return true;
}

bool VulkanBackend::CreateFrames() {
    VkSemaphoreTypeCreateInfoKHR ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (timeline_ok_) VK_CHECK(vkCreateSemaphore(dev_, &si, nullptr, &timeline_));
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        // its buffer is recorded anew each frame: the driver's reset of the pool was a sixth of a present
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = qfam_;
        VK_CHECK(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev_, &ai, &f.cb));
        if (!CreateBuffer(f.upload, kUploadSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return false;
        VkSemaphoreCreateInfo bi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev_, &bi, nullptr, &f.acquire));
        if (time_bits_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(dev_, &qi, nullptr, &f.times) != VK_SUCCESS) f.times = VK_NULL_HANDLE;
        }
    }
    if (timeline_ok_) std::thread([this] { ResetPools(); }).detach();
    return true;
}

void VulkanBackend::ResetPools() {
    pthread_setname_np(pthread_self(), "vk pool reset");
    for (;;) {
        Frame* f;
        {
            std::unique_lock lock(reset_m_);
            reset_work_cv_.wait(lock, [&] { return !resets_.empty(); });
            f = resets_.front();
            resets_.pop_front();
        }
        if (WaitValue(f->value)) {
            VkResult r = vkResetCommandPool(dev_, f->pool, 0);
            if (r != VK_SUCCESS) FailGpu("vkResetCommandPool", r, f->value);
        }
        {
            std::lock_guard lock(reset_m_);
            f->pool_clean = true;
        }
        reset_done_cv_.notify_all();
    }
}

// f was just submitted: the render thread records into it again only after AwaitPoolReset
void VulkanBackend::QueuePoolReset(Frame& f) {
    {
        std::lock_guard lock(reset_m_);
        f.pool_clean = false;
        resets_.push_back(&f);
    }
    reset_work_cv_.notify_one();
}

void VulkanBackend::AwaitPoolReset(Frame& f) {
    std::unique_lock lock(reset_m_);
    reset_done_cv_.wait(lock, [&] { return f.pool_clean || gpu_failed_; });
}

bool VulkanBackend::CreateFixedImages() {
    constexpr VkImageUsageFlags kSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!CreateImage(lut_, kBgra, 256, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[0], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D) ||
        !CreateImage(dummy_[1], kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_3D) ||
        !CreateImage(dummy_[2], kBgra, 1, 1, 1, 6, 1, kSampled, VK_IMAGE_VIEW_TYPE_CUBE, true) ||
        !CreateImage(env_off_, kBgra, 1, 1, 1, 6, 1, kSampled, VK_IMAGE_VIEW_TYPE_CUBE, true) ||
        !CreateImage(road_off_, kBgra, 1, 1, 1, 1, 1, kSampled, VK_IMAGE_VIEW_TYPE_2D))
        return false;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    linear_ = Sampler(sc);
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    point_ = Sampler(sc);
    // the first frame's commands: the fixed images cleared to black, the ramp the identity
    Begin();
    if (gpu_failed_) return false;
    VkClearColorValue black{};
    for (Image* im : {&lut_, &dummy_[0], &dummy_[1], &dummy_[2]}) {
        Transition(*im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cb(), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        Transition(*im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const VkClearColorValue grey{{0.2f, 0.2f, 0.2f, 1.0f}}, opaque_black{{0.0f, 0.0f, 0.0f, 1.0f}};
    for (auto [im, c] : {std::pair{&env_off_, &grey}, std::pair{&road_off_, &opaque_black}}) {
        Transition(*im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers};
        vkCmdClearColorImage(Cb(), im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, c, 1, &r);
        Transition(*im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i & 255);
    UploadGammaRamp();
    if (gpu_failed_ || !Submit(true, false)) return false;
    // Submit(true) began the next recording; Init begins once more
    VkResult end = vkEndCommandBuffer(Cb());
    if (end != VK_SUCCESS) {
        FailGpu("vkEndCommandBuffer", end, value_);
        return false;
    }
    VkResult reset = vkResetCommandPool(dev_, CurrentFrame().pool, 0);
    if (reset != VK_SUCCESS) {
        FailGpu("vkResetCommandPool", reset, value_);
        return false;
    }
    return true;
}

bool VulkanBackend::InitUi() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return false;
    std::lock_guard lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = instance_;
    ii.PhysicalDevice = pd_;
    ii.Device = dev_;
    ii.QueueFamily = qfam_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = uint32_t(std::max<size_t>(swap_images_.size(), 2));
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = dynamic_;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_fmt_;
    if (!dynamic_) ii.RenderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED);
    ImGui::GetIO().BackendRendererName = "vulkan";
    return ImGui_ImplVulkan_Init(&ii);
}

// ---- memory and objects

bool VulkanBackend::CreateImage(Image& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                                uint32_t levels, VkImageUsageFlags usage, VkImageViewType vt, bool cube) {
    im = {};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.layers = layers;
    im.levels = levels;
    im.depth = fmt == depth_fmt_;
    im.stencil = im.depth && HasStencil(fmt);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev_, im.image, &r);
    if (!alloc_.Get(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, im.mem) ||
        vkBindImageMemory(dev_, im.image, im.mem.mem, im.mem.off) != VK_SUCCESS) {
        Destroy(im);
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = vt;
    vi.format = fmt;
    vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                           levels, 0, layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) vkCreateImageView(dev_, &vi, nullptr, &im.view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {VkImageAspectFlags(im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                                           : VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &im.attach);
    }
    return true;
}

bool VulkanBackend::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    b = {};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(dev_, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev_, b.buf, &r);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryPropertyFlags nice = host && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    if (!alloc_.Get(r, want, nice, true, b.mem) || vkBindBufferMemory(dev_, b.buf, b.mem.mem, b.mem.off) != VK_SUCCESS) {
        Destroy(b);
        return false;
    }
    return true;
}

void VulkanBackend::Destroy(Image& im) {
    if (im.view || im.attach) view_gen_++;  // kept descriptor sets may name it (Set)
    if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    DropFramebuffers(im.attach);
    if (im.attach) vkDestroyImageView(dev_, im.attach, nullptr);
    if (im.image) vkDestroyImage(dev_, im.image, nullptr);
    alloc_.Free(im.mem);
    im = {};
}

void VulkanBackend::Destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    alloc_.Free(b.mem);
    b = {};
}

// bytes in this frame's ring; a big upload gets a buffer of its own, released with the frame. A full ring flushes:
// callers upload before they begin rendering and bind
VulkanBackend::Alloc VulkanBackend::Upload(VkDeviceSize size, VkDeviceSize align) {
    if (gpu_failed_) return {VK_NULL_HANDLE, 0, nullptr};
    if (size > kUploadSize / 4) {
        Buffer b;
        if (!CreateBuffer(b, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true))
            return {VK_NULL_HANDLE, 0, nullptr};
        Alloc a{b.buf, 0, b.mem.map};
        CurrentFrame().buffers.push_back(b);
        return a;
    }
    Frame* f = &CurrentFrame();
    VkDeviceSize o = (f->used + align - 1) / align * align;
    if (o + size > kUploadSize) {
        if (!Submit(true, false)) return {VK_NULL_HANDLE, 0, nullptr};
        f = &CurrentFrame();
        o = 0;
    }
    f->used = o + size;
    return {f->upload.buf, o, f->upload.mem.map + o};
}

VulkanBackend::Alloc VulkanBackend::Constants(const void* data, size_t bytes) {
    Alloc a = Upload(kUboRange, kUboAlign);
    if (!a.cpu) return a;
    std::memcpy(a.cpu, data, std::min<size_t>(bytes, kUboRange));
    return a;
}

// ---- command recording

void VulkanBackend::Begin() {
    if (gpu_failed_) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = vkBeginCommandBuffer(Cb(), &bi);
    if (r != VK_SUCCESS) {
        FailGpu("vkBeginCommandBuffer", r, value_);
        return;
    }
    if (VkQueryPool q = CurrentFrame().times) {
        vkCmdResetQueryPool(Cb(), q, 0, 2);
        vkCmdWriteTimestamp(Cb(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, q, 0);
    }
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

void VulkanBackend::FailGpu(const char* operation, VkResult result, uint64_t value) {
    if (!gpu_failed_.exchange(true, std::memory_order_relaxed))
        LOGE("vulkan: %s failed for GPU value %llu (%d); rendering stopped", operation,
             (unsigned long long)value, int(result));
}

uint64_t VulkanBackend::Completed() {
    if (gpu_failed_) return 0;
    if (!timeline_ok_) {
        std::lock_guard lock(fence_lock_);
        PollFences();
        return fenced_;
    }
    uint64_t v = 0;
    VkResult r = counter_value_(dev_, timeline_, &v);
    if (r != VK_SUCCESS) FailGpu("vkGetSemaphoreCounterValue", r, 0);
    return v;
}

// the signalled fences, oldest first, back to the spares; called with fence_lock_ held
void VulkanBackend::PollFences() {
    size_t done = 0;
    while (done < pending_.size()) {
        VkResult r = vkGetFenceStatus(dev_, pending_[done].fence);
        if (r == VK_NOT_READY) break;
        if (r != VK_SUCCESS) {
            FailGpu("vkGetFenceStatus", r, pending_[done].value);
            break;
        }
        VkResult reset = vkResetFences(dev_, 1, &pending_[done].fence);
        if (reset != VK_SUCCESS) {
            FailGpu("vkResetFences", reset, pending_[done].value);
            break;
        }
        fenced_ = pending_[done].value;
        spare_fences_.push_back(pending_[done].fence);
        done++;
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
}

// an unsignalled fence for the next submit; called with fence_lock_ held
VkFence VulkanBackend::SubmitFence(VkResult& result) {
    VkFence f = VK_NULL_HANDLE;
    if (!spare_fences_.empty()) {
        f = spare_fences_.back();
        spare_fences_.pop_back();
        result = VK_SUCCESS;
    } else {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(dev_, &ci, nullptr, &f);
        if (result != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    return f;
}

bool VulkanBackend::WaitValue(uint64_t v) {
    constexpr uint64_t kGpuWaitTimeoutNs = 5'000'000'000ull;
    if (gpu_failed_) return false;
    if (!v) return true;
    if (Completed() >= v) return !gpu_failed_;
    if (gpu_failed_) return false;
    if (!timeline_ok_) {
        // The lock stays held through the bounded wait so no other thread can reset this fence.
        std::lock_guard lock(fence_lock_);
        if (fenced_ >= v) return true;
        auto it = std::find_if(pending_.begin(), pending_.end(), [v](const Pending& p) { return p.value >= v; });
        if (it == pending_.end()) {
            FailGpu("missing pending fence", VK_NOT_READY, v);
            return false;
        }
        VkResult r = vkWaitForFences(dev_, 1, &it->fence, VK_TRUE, kGpuWaitTimeoutNs);
        if (r != VK_SUCCESS) {
            FailGpu(r == VK_TIMEOUT ? "GPU fence wait timed out" : "vkWaitForFences", r, v);
            return false;
        }
        PollFences();
        return !gpu_failed_ && fenced_ >= v;
    }
    VkSemaphoreWaitInfoKHR wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO_KHR};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &v;
    VkResult r = wait_semaphores_(dev_, &wi, kGpuWaitTimeoutNs);
    if (r != VK_SUCCESS) {
        FailGpu(r == VK_TIMEOUT ? "GPU timeline wait timed out" : "vkWaitSemaphores", r, v);
        return false;
    }
    return true;
}

// the frame's garbage freed and its pools reset, once the GPU is past it
bool VulkanBackend::ResetFrame(Frame& f) {
    if (!WaitValue(f.value)) return false;
    if (f.timed) {
        f.timed = false;
        uint64_t t[2];
        if (vkGetQueryPoolResults(dev_, f.times, 0, 2, sizeof(t), t, sizeof(t[0]), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            uint64_t mask = time_bits_ >= 64 ? ~0ull : (1ull << time_bits_) - 1;
            double ns = double((t[1] - t[0]) & mask) * props_.limits.timestampPeriod;
            gpu_ns_ += ns;
            g_perf.gpu_ns += uint64_t(ns);
        }
    }
    for (Image& im : f.images) Destroy(im);
    for (Buffer& b : f.buffers) Destroy(b);
    f.images.clear();
    f.buffers.clear();
    constexpr size_t kMaxKeptSets = 4096;
    if (f.pools_dirty || f.set_gen != view_gen_ || f.sets.size() > kMaxKeptSets) {
        for (VkDescriptorPool p : f.pools) vkResetDescriptorPool(dev_, p, 0);
        f.pool_at = 0;
        f.sets.clear();
        f.set_gen = view_gen_;
        f.pools_dirty = false;
    }
    f.used = 0;
    upload_epoch_++;
    return true;
}

// present: the frame's last commands, waiting on the acquired image and signalling its present semaphore
bool VulkanBackend::Submit(bool wait, bool present) {
    if (gpu_failed_) return false;
    EndRender();
    Frame& f = CurrentFrame();
    if (f.times) {
        vkCmdWriteTimestamp(f.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.times, 1);
        f.timed = true;
    }
    VkResult end = vkEndCommandBuffer(f.cb);
    if (end != VK_SUCCESS) {
        FailGpu("vkEndCommandBuffer", end, value_);
        return false;
    }
    uint64_t signal = value_ + 1;
    // without the timeline only the present semaphore is signalled, and a fence tells when the GPU is past
    VkSemaphore signals[2] = {timeline_, VK_NULL_HANDLE};
    uint64_t values[2] = {signal, 0};
    uint32_t first = timeline_ok_ ? 0 : 1;
    VkTimelineSemaphoreSubmitInfoKHR ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
    ti.signalSemaphoreValueCount = present ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    uint64_t wait_value = 0;
    ti.waitSemaphoreValueCount = present ? 1 : 0;
    ti.pWaitSemaphoreValues = &wait_value;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline_ok_ ? &ti : nullptr};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (present) {
        signals[1] = swap_done_[present_index_];
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquire;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cb;
    si.signalSemaphoreCount = (present ? 2 : 1) - first;
    si.pSignalSemaphores = signals + first;
    VkResult r = VK_SUCCESS;
    const char* failed_operation = "vkQueueSubmit";
    if (timeline_ok_) {
        r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    } else {
        std::lock_guard lock(fence_lock_);
        VkFence fence = SubmitFence(r);
        if (fence) {
            r = vkQueueSubmit(queue_, 1, &si, fence);
            if (r == VK_SUCCESS) pending_.push_back({signal, fence});
            else spare_fences_.push_back(fence);
        } else failed_operation = "vkCreateFence";
    }
    if (r != VK_SUCCESS) {
        FailGpu(failed_operation, r, signal);
        return false;
    }
    value_ = signal;
    f.value = signal;
    g_perf.submits++;
    if (wait) {
        auto t = std::chrono::steady_clock::now();
        if (!ResetFrame(f)) return false;
        g_perf.stall_ns += SinceNs(t);
        g_perf.stalls++;
        VkResult reset = vkResetCommandPool(dev_, f.pool, 0);
        if (reset != VK_SUCCESS) {
            FailGpu("vkResetCommandPool", reset, signal);
            return false;
        }
        Begin();
    }
    return !gpu_failed_;
}

void VulkanBackend::Transition(Image& im, VkImageLayout to) {
    // read, copied or written another way: a clear left for a rendering is done first
    if (im.pending && to != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && to != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        FlushClear(im);
    if (im.layout == to) return;
    EndRender();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkPipelineStageFlags src, dst;
    LayoutMasks(im.layout, src, b.srcAccessMask);
    LayoutMasks(to, dst, b.dstAccessMask);
    if (im.layout == VK_IMAGE_LAYOUT_UNDEFINED) src = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    VkImageAspectFlags aspect = im.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (im.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                                         : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange = {aspect, 0, im.levels, 0, im.layers};
    vkCmdPipelineBarrier(Cb(), src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
}

// a clear left on the image, in a rendering of its own
void VulkanBackend::FlushClear(Image& im) {
    if (im.depth) BeginRender(nullptr, &im);
    else BeginRender(&im, nullptr);
    EndRender();
}

// a rendering over the whole of the attachments, which keep what they hold or load a clear left on them; the one under
// way carries on when it has the same ones
void VulkanBackend::BeginRender(Image* color, Image* depth) {
    if (rendering_ && color == rend_color_ && depth == rend_depth_) return;
    EndRender();
    // the area is the target's: a depth larger than it is cleared whole on its own
    if (depth && depth->pending && color && (depth->w != color->w || depth->h != color->h)) FlushClear(*depth);
    if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    // the renderings before wrote the same attachments: their writes come first
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(Cb(), st, st, 0, 1, &mb, 0, nullptr, 0, nullptr);
    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
    rend_w_ = color ? color->w : depth->w;
    rend_h_ = color ? color->h : depth->h;
    g_perf.passes++;
    {
        Image* any = color ? color : depth;
        g_perf.pass_px += uint64_t(any->w) * any->h * ((color ? 1 : 0) + (depth ? 1 : 0));
        if (pass_log_ && pass_times_ && pass_stamps_ < kPassStamps - 1) {
            char line[64];
            std::snprintf(line, sizeof(line), "%s%s", color ? "c" : "", depth ? "d" : "");
            std::snprintf(line + std::strlen(line), sizeof(line) - std::strlen(line), " %ux%u", any->w, any->h);
            pass_list_.push_back(line);
            vkCmdWriteTimestamp(Cb(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_times_, pass_stamps_++);
        }
    }
    if (!dynamic_) {
        Image* any = color ? color : depth;
        VkFormat cf = color ? color->fmt : VK_FORMAT_UNDEFINED, df = depth ? depth->fmt : VK_FORMAT_UNDEFINED;
        // a clear left on an attachment is the pass's load op, as with dynamic rendering: a rendering of its own for
        // it loaded and stored the whole target once more
        VkImageAspectFlags clears = 0;
        VkClearValue values[2] = {};
        uint32_t n = 0;
        if (color) {
            clears |= color->pending & VK_IMAGE_ASPECT_COLOR_BIT;
            values[n++] = color->clear;
            color->pending = 0;
        }
        if (depth) {
            clears |= depth->pending & (VK_IMAGE_ASPECT_DEPTH_BIT | (depth->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
            values[n++] = depth->clear;
            depth->pending = 0;
        }
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(cf, df, clears);
        bi.framebuffer = Framebuffer(cf, df, color ? color->attach : VK_NULL_HANDLE, depth ? depth->attach : VK_NULL_HANDLE,
                                     any->w, any->h);
        bi.renderArea = {{0, 0}, {any->w, any->h}};
        bi.clearValueCount = clears ? n : 0;
        bi.pClearValues = values;
        vkCmdBeginRenderPass(Cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingAttachmentInfoKHR da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    Image* any = color ? color : depth;
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    auto load = [](const Image& im, VkImageAspectFlags aspect) {
        return (im.pending & aspect) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    };
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = load(*color, VK_IMAGE_ASPECT_COLOR_BIT);
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue = color->clear;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        color->pending = 0;
    }
    VkRenderingAttachmentInfoKHR sa;
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = load(*depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue = depth->clear;
        ri.pDepthAttachment = &da;
        sa = da;
        sa.loadOp = load(*depth, VK_IMAGE_ASPECT_STENCIL_BIT);
        if (depth->stencil) ri.pStencilAttachment = &sa;
        depth->pending = 0;
    }
    begin_rendering_(Cb(), &ri);
}

void VulkanBackend::EndRender() {
    if (!rendering_) return;
    if (dynamic_)
        end_rendering_(Cb());
    else
        vkCmdEndRenderPass(Cb());
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
}

// the layouts stay the attachment ones throughout, as with dynamic rendering: barriers outside do the transitions.
// pipelines only need a compatible pass, which load ops are not part of
VkRenderPass VulkanBackend::Pass(VkFormat color, VkFormat depth, VkImageAspectFlags clears) {
    const uint64_t k[3] = {uint64_t(color), uint64_t(depth), uint64_t(clears)};
    auto load = [&](VkImageAspectFlags aspect) {
        return (clears & aspect) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    };
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = passes_.find(key); it != passes_.end()) return it->second;
    VkAttachmentDescription at[2] = {};
    VkAttachmentReference cr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t n = 0;
    if (color != VK_FORMAT_UNDEFINED) {
        at[n] = {0, color, VK_SAMPLE_COUNT_1_BIT, load(VK_IMAGE_ASPECT_COLOR_BIT), VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        cr.attachment = n++;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &cr;
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        at[n] = {0, depth, VK_SAMPLE_COUNT_1_BIT, load(VK_IMAGE_ASPECT_DEPTH_BIT), VK_ATTACHMENT_STORE_OP_STORE,
                 load(VK_IMAGE_ASPECT_STENCIL_BIT), VK_ATTACHMENT_STORE_OP_STORE,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        dr.attachment = n++;
        sp.pDepthStencilAttachment = &dr;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev_, &ci, nullptr, &rp) != VK_SUCCESS) LOGE("vulkan: render pass failed");
    passes_[key] = rp;
    return rp;
}

VkFramebuffer VulkanBackend::Framebuffer(VkFormat color_fmt, VkFormat depth_fmt, VkImageView color, VkImageView depth,
                                         uint32_t w, uint32_t h) {
    const uint64_t k[4] = {uint64_t(color), uint64_t(depth), w, h};
    uint64_t key = Hash(k, sizeof(k));
    if (auto it = framebuffers_.find(key); it != framebuffers_.end()) return it->second.fb;
    VkImageView views[2];
    uint32_t n = 0;
    if (color) views[n++] = color;
    if (depth) views[n++] = depth;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = Pass(color_fmt, depth_fmt);
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev_, &ci, nullptr, &fb) != VK_SUCCESS) LOGE("vulkan: framebuffer %ux%u failed", w, h);
    framebuffers_[key] = {color, depth, fb};
    return fb;
}

// a view's framebuffers go with it, once the GPU is past them
void VulkanBackend::DropFramebuffers(VkImageView v) {
    if (!v || framebuffers_.empty()) return;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->second.color == v || it->second.depth == v) {
            vkDestroyFramebuffer(dev_, it->second.fb, nullptr);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// in the D3D convention, y down from the top: a negative height turns Vulkan's clip space the same way up
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    VkViewport vp{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(Cb(), 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    VkRect2D r{{std::max(x0, 0), std::max(y0, 0)}, {uint32_t(std::max(x1 - std::max(x0, 0), 0)), uint32_t(std::max(y1 - std::max(y0, 0), 0))}};
    vkCmdSetScissor(Cb(), 0, 1, &r);
}

// the draw's descriptor set: the ring at b0 and b1 (offsets bound with it), the views and samplers; one per
// combination a frame
VkDescriptorSet VulkanBackend::Set(const VkImageView views[4], const VkSampler samplers[4]) {
    Frame& f = CurrentFrame();
    if (f.set_gen != view_gen_) {
        // a view went since these were made: new ones from here, the pools reset with the frame
        f.sets.clear();
        f.set_gen = view_gen_;
        f.pools_dirty = true;
    }
    uint64_t key = Hash(views, sizeof(VkImageView) * 4, Hash(samplers, sizeof(VkSampler) * 4));
    if (auto it = f.sets.find(key); it != f.sets.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (f.pool_at == f.pools.size()) {
            VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 2},
                                             {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 4},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 4}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSetsPerPool;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(dev_, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(dev_, &ai, &set) == VK_SUCCESS) break;
        f.pool_at++;
    }
    VkDescriptorBufferInfo bi{f.upload.buf, 0, kUboRange};
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[vkshader::kBindings];
    for (uint32_t i = 0; i < vkshader::kBindings; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        if (i < 2) {
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi;
        } else if (i < vkshader::kBindingSamplers) {
            uint32_t s = i - vkshader::kBindingTextures;
            ii[s] = {VK_NULL_HANDLE, views[s] ? views[s] : dummy_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = &ii[s];
        } else {
            uint32_t s = i - vkshader::kBindingSamplers;
            si[s] = {samplers[s] ? samplers[s] : point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &si[s];
        }
    }
    vkUpdateDescriptorSets(dev_, vkshader::kBindings, w, 0, nullptr);
    f.sets[key] = set;
    return set;
}

void VulkanBackend::BindSet(VkDescriptorSet set, VkDeviceSize vc, VkDeviceSize dc) {
    uint32_t offs[2] = {uint32_t(vc), uint32_t(dc)};
    vkCmdBindDescriptorSets(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, offs);
}

// ---- shaders and pipelines

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), stage == vkshader::Stage::Vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = modules_.find(hash); it != modules_.end()) return it->second;
    auto t = std::chrono::steady_clock::now();
    std::vector<uint32_t> spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkshader::Compile(src, entry, stage, hash, spirv)) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    }
    modules_[hash] = m;
    g_perf.shader_ns += SinceNs(t);
    g_perf.shaders++;
    return m;
}

// a full screen pass: the triangle from SV_VertexID, solid, no culling; depth set: writes depth only
VkPipeline VulkanBackend::FullScreenPipeline(const char* hlsl, const char* ps_entry, VkFormat color, VkFormat depth,
                                             bool over) {
    uint64_t key = Hash(hlsl, std::strlen(hlsl), uint64_t(color) << 32 | uint64_t(depth) << 1 | (over ? 1 : 0));
    key = Hash(ps_entry, std::strlen(ps_entry), key);
    if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
    uint64_t hv, hp;
    VkShaderModule vs = Module(hlsl, "vs", vkshader::Stage::Vertex, hv);
    VkShaderModule ps = Module(hlsl, ps_entry, vkshader::Stage::Pixel, hp);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = depth != VK_FORMAT_UNDEFINED;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        if (over) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = color != VK_FORMAT_UNDEFINED ? 1 : 0;
        cb.pAttachments = &ba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        rinfo.colorAttachmentCount = cb.attachmentCount;
        rinfo.pColorAttachmentFormats = &color;
        rinfo.depthAttachmentFormat = depth;
        rinfo.stencilAttachmentFormat = HasStencil(depth) ? depth : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
        if (!dynamic_) pi.renderPass = Pass(color, depth);
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    }
    pipelines_[key] = pipe;
    return pipe;
}

// shadercache/preload.bin: its header, then records, each a tag and what it lists
constexpr uint32_t kListMagic = 0x4C50574D;  // "MWPL"
constexpr uint32_t kListVersion = 1;
constexpr uint32_t kListShader = 1, kListPipeline = 2;  // a record's tag: a shader's key and hash, a PsoKey

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    if (auto it = pipelines_.find(h); it != pipelines_.end()) return it->second;
    {
        std::lock_guard lock(pre_lock_);
        if (auto it = pre_pipes_.find(h); it != pre_pipes_.end()) return pipelines_[h] = it->second;
        pre_claimed_.insert(h);
    }
    auto t = std::chrono::steady_clock::now();
    VkPipeline pipe = BuildPipeline(k, vs, ps, PipelinePass(k));
    if (!pipe) LOGE("vulkan: pipeline creation failed");
    g_perf.pipe_ns += SinceNs(t);
    g_perf.pipes++;
    pipelines_[h] = pipe;
    if (pipe && listed_.insert(h).second) Record(kListPipeline, &k, sizeof(k));
    return pipe;
}

// the render pass a pipeline is made for, with render passes; none with dynamic rendering. Render thread
VkRenderPass VulkanBackend::PipelinePass(const PsoKey& k) {
    if (dynamic_) return VK_NULL_HANDLE;
    return Pass(k.has_rt ? VkFormat(k.rtfmt) : VK_FORMAT_UNDEFINED, k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED);
}

// a pipeline as its key says: what it reads is set at Init, so the preload thread makes them too
VkPipeline VulkanBackend::BuildPipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkRenderPass pass) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = ps;
    st[1].pName = "main";
    // the attributes at their register's location, as vk_shader.cpp numbers the inputs
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.buffers; i++) vb[i] = {i, k.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        va[na++] = {r, (e >> 8) & 0xFF, VertexFormat(e & 0xFF), e >> 16};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.buffers;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    Topology topo = Topology(k.topo & 0xF);
    bool strip = k.topo >> 4;
    ia.topology = topo == Topology::Points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topo == Topology::Lines ? (strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                                            : (strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    constexpr uint32_t kFillWireframe = 0x1B01;
    rs.polygonMode = k.fill == kFillWireframe && feats_.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias != 0 || k.zslope != 0;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = Cmp(k.zfunc);
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = k.blend;
    ba.srcColorBlendFactor = Blend(k.src, false);
    ba.dstColorBlendFactor = Blend(k.dst, false);
    ba.colorBlendOp = BlendOp(k.op);
    ba.srcAlphaBlendFactor = Blend(k.src, true);
    ba.dstAlphaBlendFactor = Blend(k.dst, true);
    ba.alphaBlendOp = BlendOp(k.op);
    ba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &ba;
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfoKHR rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rinfo.colorAttachmentCount = cb.attachmentCount;
    rinfo.pColorAttachmentFormats = &color;
    rinfo.depthAttachmentFormat = k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    rinfo.stencilAttachmentFormat = k.has_ds && HasStencil(depth_fmt_) ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, dynamic_ ? &rinfo : nullptr};
    if (!dynamic_) pi.renderPass = pass;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, cache_, 1, &pi, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    return pipe;
}

// ---- Shader preloading

std::filesystem::path PreloadListPath() { return app::ExeDir() / "shadercache" / "preload.bin"; }

// the list as earlier runs left it, then open to add to; one of another version or key size is started again
void VulkanBackend::LoadPreloadList() {
    const uint32_t header[4] = {kListMagic, kListVersion, uint32_t(sizeof(PsoKey)), 0};
    std::filesystem::path p = PreloadListPath();
    bool fresh = true;
    if (std::ifstream f{p, std::ios::binary}) {
        uint32_t h[4] = {};
        if (f.read(reinterpret_cast<char*>(h), sizeof(h)) && !std::memcmp(h, header, sizeof(h))) {
            fresh = false;
            uint32_t tag;
            while (f.read(reinterpret_cast<char*>(&tag), 4)) {
                if (tag == kListShader) {
                    uint64_t rec[2];
                    if (!f.read(reinterpret_cast<char*>(rec), sizeof(rec))) break;
                    if (listed_.insert(rec[0]).second) listed_shaders_.emplace_back(rec[0], rec[1]);
                } else if (tag == kListPipeline) {
                    PsoKey k;
                    if (!f.read(reinterpret_cast<char*>(&k), sizeof(k))) break;
                    if (listed_.insert(Hash(&k, sizeof(k))).second) listed_pipes_.push_back(k);
                } else {
                    break;  // a record cut short by the app's end: the rest is written again as it is met
                }
            }
        }
    }
    if (fresh) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        list_.open(p, std::ios::binary | std::ios::trunc);
        list_.write(reinterpret_cast<const char*>(header), sizeof(header));
    } else {
        list_.open(p, std::ios::binary | std::ios::app);
    }
    list_.flush();
    LOGR("vulkan: %zu shaders and %zu pipelines listed for preloading", listed_shaders_.size(), listed_pipes_.size());
}

// a shader or pipeline made for the first time, added to the list; written at once, as the app may be killed
void VulkanBackend::Record(uint32_t tag, const void* data, size_t size) {
    if (!list_) return;
    list_.write(reinterpret_cast<const char*>(&tag), 4);
    list_.write(static_cast<const char*>(data), std::streamsize(size));
    list_.flush();
}

// the preload thread's module for a shader's key, now the render thread's
bool VulkanBackend::TakePreloadedShader(uint64_t key, uint64_t& hash, VkShaderModule& module) {
    std::lock_guard lock(pre_lock_);
    auto it = pre_shaders_.find(key);
    if (it == pre_shaders_.end()) return false;
    hash = it->second;
    VkShaderModule& m = modules_[hash];
    if (!m) m = pre_modules_[hash];
    module = m;
    return module != VK_NULL_HANDLE;
}

// render thread, once SHADER PRELOADING is on: the render passes the listed pipelines need, made here as Pass is the
// render thread's, then the thread that makes the rest
void VulkanBackend::StartPreload() {
    preload_started_ = true;
    if (listed_shaders_.empty() && listed_pipes_.empty()) return;
    std::unordered_map<uint64_t, VkRenderPass> passes;
    for (const PsoKey& k : listed_pipes_) {
        uint64_t key = uint64_t(k.has_rt ? k.rtfmt : 0) << 1 | (k.has_ds ? 1 : 0);
        if (!passes.count(key)) passes[key] = PipelinePass(k);
    }
    std::thread([this, shaders = listed_shaders_, pipes = listed_pipes_, passes = std::move(passes)]() mutable {
        Preload(std::move(shaders), std::move(pipes), std::move(passes));
    }).detach();
    listed_shaders_.clear();
    listed_pipes_.clear();
}

// the preload thread: the shaders' modules from the cached SPIR-V, then the pipelines; one the render thread made
// first is skipped, and one it makes meanwhile wins over this thread's
void VulkanBackend::Preload(std::vector<std::pair<uint64_t, uint64_t>> shaders, std::vector<PsoKey> pipes,
                            std::unordered_map<uint64_t, VkRenderPass> passes) {
    auto t = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, VkShaderModule> modules;
    auto module = [&](uint64_t hash) {
        if (auto it = modules.find(hash); it != modules.end()) return it->second;
        std::vector<uint32_t> spirv;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkshader::Cached(hash, spirv)) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = spirv.size() * 4;
            ci.pCode = spirv.data();
            if (vkCreateShaderModule(dev_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        }
        return modules[hash] = m;
    };
    size_t made_shaders = 0, made_pipes = 0;
    for (const auto& [key, hash] : shaders) {
        VkShaderModule m = module(hash);
        if (!m) continue;
        std::lock_guard lock(pre_lock_);
        pre_modules_.emplace(hash, m);
        pre_shaders_.emplace(key, hash);
        made_shaders++;
    }
    for (const PsoKey& k : pipes) {
        uint64_t h = Hash(&k, sizeof(k));
        {
            std::lock_guard lock(pre_lock_);
            if (pre_claimed_.count(h)) continue;
        }
        VkShaderModule vs = module(k.vs), ps = module(k.ps);
        if (!vs || !ps) continue;
        uint64_t pass = uint64_t(k.has_rt ? k.rtfmt : 0) << 1 | (k.has_ds ? 1 : 0);
        VkPipeline pipe = BuildPipeline(k, vs, ps, passes[pass]);
        if (!pipe) continue;
        std::lock_guard lock(pre_lock_);
        if (pre_claimed_.count(h)) {
            vkDestroyPipeline(dev_, pipe, nullptr);
            continue;
        }
        pre_pipes_[h] = pipe;
        made_pipes++;
    }
    LOGR("vulkan: preloaded %zu shaders and %zu pipelines in %.1f s", made_shaders, made_pipes,
         double(SinceNs(t)) / 1e9);
}

VkSampler VulkanBackend::Sampler(const VkSamplerCreateInfo& ci) {
    constexpr size_t kAt = offsetof(VkSamplerCreateInfo, magFilter), kBytes = sizeof(VkSamplerCreateInfo) - kAt;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&ci) + kAt;
    for (size_t i = 0; i < recent_count_; i++) {
        if (std::memcmp(recent_samplers_[i].ci, bytes, kBytes)) continue;
        RecentSampler hit = recent_samplers_[i];
        std::memmove(&recent_samplers_[1], &recent_samplers_[0], i * sizeof(RecentSampler));
        recent_samplers_[0] = hit;
        return hit.sampler;
    }
    uint64_t key = Hash(bytes, kBytes);
    VkSampler s = VK_NULL_HANDLE;
    if (auto it = samplers_.find(key); it != samplers_.end()) {
        s = it->second;
    } else {
        vkCreateSampler(dev_, &ci, nullptr, &s);
        samplers_[key] = s;
    }
    recent_count_ = std::min(recent_count_ + 1, recent_samplers_.size());
    std::memmove(&recent_samplers_[1], &recent_samplers_[0], (recent_count_ - 1) * sizeof(RecentSampler));
    std::memcpy(recent_samplers_[0].ci, bytes, kBytes);
    recent_samplers_[0].sampler = s;
    return s;
}

// fixed border colours only: the console's nearest
VkBorderColor Border(uint32_t argb) {
    if ((argb >> 24) < 128) return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
    return sum >= 384 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    VkSamplerCreateInfo sd{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    bool filterable = stages.fmt[i] != VK_FORMAT_R32_SFLOAT || r32f_linear_;
    sd.minFilter = lin(min) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.magFilter = lin(mag) && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    bool mipped = mip && stages.levels[i] > 1;
    if (aniso && lin(min) && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1 && feats_.samplerAnisotropy) {
        sd.anisotropyEnable = VK_TRUE;
        sd.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
    }
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    sd.addressModeU = Address(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.addressModeV = Address(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.addressModeW = Address(t[kTssAddressW]);
    float limit = props_.limits.maxSamplerLodBias;
    sd.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -limit, limit);
    sd.minLod = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    sd.maxLod = mip ? VK_LOD_CLAMP_NONE : sd.minLod;
    sd.borderColor = Border(t[kTssBorderColor]);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        bool linear = (lin(min) || lin(mag)) && depth_linear_;
        sd.minFilter = sd.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.compareEnable = VK_TRUE;
        sd.compareOp = VkCompareOp(swap[CmpIndex(s.rs[kRsShadowFunc])]);
        sd.anisotropyEnable = VK_FALSE;
        sd.maxAnisotropy = 1;
    }
    return Sampler(sd);
}

// ---- targets

Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (draw_rt_ == &it->second) draw_rt_ = nullptr;
    if (draw_ds_ == &it->second) draw_ds_ = nullptr;
    Retire(it->second.img);
    Retire(it->second.fb);
    targets_.erase(it);
}

// The game's effect buffers, smaller than the frame and of its shape (Most Wanted's 320x240 glow and blur, its 80x60),
// near the console's size. Their passes sample the frame and each other with a box of taps that grows with the scale
// (FillTexelConstants), so at the frame's scale they cost its fourth power: most of the GPU's frame at 3x. They are
// blurred anyway; square ones (reflections) and those drawn with a frame sized depth keep the frame's scale
float VulkanBackend::TargetScale(uint64_t key, uint32_t gw, uint32_t gh) const {
    bool effect = gw < guest_w_ && uint64_t(gw) * guest_h_ == uint64_t(gh) * guest_w_ && !full_scale_.count(key);
    return effect ? std::min(scale_, kEffectScale) : scale_;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) {
        LOG_FIRST(40, "vulkan: target %08X%s %ux%u made again as %ux%u (header %08X size %08X)", data, depth ? " depth" : "", it->second.gw,
                  it->second.gh, gw, gh, s.header, s.size);
        ReleaseTarget(it);
    }
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    float sc = TargetScale(key, gw, gh);
    t.w = std::max(1u, uint32_t(std::lround(gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sc)));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!CreateImage(t.img, depth ? depth_fmt_ : kBgra, t.w, t.h, 1, 1, 1, usage, VK_IMAGE_VIEW_TYPE_2D)) {
        LOGE("vulkan: no %ux%u target", t.w, t.h);
        return nullptr;
    }
    // a new target starts cleared
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (depth) {
        VkClearDepthStencilValue one{1, 0};
        VkImageSubresourceRange r{VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    return &ref;
}

// A colour surface over rows of a presented frame buffer, as wide and so of its pitch: split screen's halves, the top
// one at the frame's own address and the bottom one 240 rows in, where each player's visual treatment is drawn. A
// target of their own never reached the frame, and the top one, at the frame's key, made the frame again every frame
// (black under the HUD). Drawn into the frame instead, rows down; sub_h is the surface's height
Target* VulkanBackend::FrameRegion(const Surface& s, uint32_t& rows, uint32_t& sub_h) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    for (uint32_t f : presented_) {
        if (!f || data < f) continue;
        auto it = targets_.find(TargetMapKey(f, false));
        if (it == targets_.end()) continue;
        Target& t = it->second;
        uint32_t pitch = GuestPitch(gw), off = data - f;
        if (t.gw != gw || gh >= t.gh || off % pitch || off / pitch + gh > t.gh) continue;
        rows = off / pitch;
        sub_h = gh;
        return &t;
    }
    return nullptr;
}

// a colour target to draw or clear: a presented frame's rows (FrameRegion), else the surface's own
Target* VulkanBackend::EnsureColor(const Surface& s, uint32_t& rows, uint32_t& sub_h) {
    rows = sub_h = 0;
    if (Target* t = FrameRegion(s, rows, sub_h)) return t;
    return EnsureTarget(s, false);
}

// BGRA pixels into a new w x h image, left readable
bool VulkanBackend::PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(im, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_VIEW_TYPE_2D))
        return false;
    Alloc a = Upload(VkDeviceSize(w) * h * 4, 16);
    if (!a.cpu) return false;
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// CPU pixels of a colour surface into its target, scaled; after a Sync only the changed ones, over the picture
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!PixelImage(tex, px, pitch, d.width, d.height)) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, {{0, 0}, {t.w, t.h}}, tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// src over the rect of dst through the blit shader: its constants first, then the rendering
void VulkanBackend::Blit(Image& dst, const VkRect2D& to, Image& src, const float uvrect[4], const BlitOptions& o) {
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    Alloc cb = Constants(c, sizeof(c));
    if (!cb.cpu) return;
    VkPipeline pipe = FullScreenPipeline(kBlitHlsl, "psmain", dst.fmt, VK_FORMAT_UNDEFINED, o.over);
    if (!pipe) return;
    Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(&dst, nullptr);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(float(to.offset.x), float(to.offset.y), float(to.extent.width), float(to.extent.height));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(Target& t) {
    if (!t.fb && !CreateImage(t.fb, t.img.fmt, t.w, t.h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_VIEW_TYPE_2D))
        return nullptr;
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCopy c{{aspect, 0, 0, 1}, {0, 0, 0}, {aspect, 0, 0, 1}, {0, 0, 0}, {t.w, t.h, 1}};
    vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &t.fb;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    Alloc cb = Constants(nullptr, 0);
    if (!cb.cpu) return false;
    VkPipeline pipe = FullScreenPipeline(kDepthHlsl, "psmain", VK_FORMAT_UNDEFINED, depth_fmt_, false);
    if (!pipe) return false;
    Transition(src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageView views[4] = {src.img.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkSampler samplers[4] = {linear_, point_, point_, point_};
    VkDescriptorSet set = Set(views, samplers);
    BeginRender(nullptr, &dst.img);
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, cb.off, cb.off);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    vkCmdDraw(Cb(), 3, 1, 0, 0);
    dst.drawn = true;
    return true;
}

// src filtered into a new w x h BGRA image, left as a copy source
bool VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps) {
    if (!down || down.w != w || down.h != h) {
        Retire(down);
        if (!CreateImage(down, kBgra, w, h, 1, 1, 1,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_VIEW_TYPE_2D))
            return false;
    }
    float uv[4] = {0, 0, 1, 1};
    Blit(down, {{0, 0}, {w, h}}, src, uv, {.alpha = true, .taps = taps});
    Transition(down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return true;
}

// the target as a guest sized BGRA copy source: itself, or shrunk into down
Image* VulkanBackend::CopySource(Target& t, Image& down) {
    if (t.depth) return nullptr;
    if (t.w != t.gw || t.h != t.gh) {
        uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
        return Shrink(t.img, t.gw, t.gh, down, std::max(taps, 1u)) ? &down : nullptr;
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return &t.img;
}

// a copy source to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    Buffer rb;
    if (!CreateBuffer(rb, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(Cb(), im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if (!Submit(true, false)) return false;
    out.assign(rb.mem.map, rb.mem.map + size_t(w) * h * 4);
    Destroy(rb);
    return true;
}

// ---- draws

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        sh.vs = modules_[sh.vh];
        return;
    }
    if (TakePreloadedShader(sh.vkey, sh.vh, sh.vs)) {
        shader_src_[sh.vkey] = sh.vh;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(src);
        if (!points.empty()) {
            src = std::move(points);
            entry = "main_points";
        }
    }
    sh.vs = Module(src, entry, vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
    if (sh.vs && listed_.insert(sh.vkey).second) {
        const uint64_t rec[2] = {sh.vkey, sh.vh};
        Record(kListShader, rec, sizeof(rec));
    }
}

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        sh.ps = modules_[sh.ph];
        return;
    }
    uint64_t key = sh.pkey ^ kPixelKeySalt;
    if (TakePreloadedShader(key, sh.ph, sh.ps)) {
        shader_src_[key] = sh.ph;
        return;
    }
    sh.ps = Module(PixelHlsl(pk), "main", vkshader::Stage::Pixel, sh.ph);
    shader_src_[key] = sh.ph;
    if (sh.ps && listed_.insert(key).second) {
        const uint64_t rec[2] = {key, sh.ph};
        Record(kListShader, rec, sizeof(rec));
    }
}

constexpr float kUpscaledMin = 1.01f;

VkImageView VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                             DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return VK_NULL_HANDLE;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = draw_rt_;
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && t.gw < guest_w_) stages.small = true;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    stages.fmt[stage] = t.img.fmt;
    if (feedback) {
        Image* fb = Feedback(t);
        return fb ? fb->view : VK_NULL_HANDLE;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.img.view;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    tx.decode = bc && !bc_;
    switch (fi.host) {
        case Host::BC1: tx.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case Host::BC2: tx.fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case Host::BC3: tx.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case Host::R32F: tx.fmt = VK_FORMAT_R32_SFLOAT; break;
        default: tx.fmt = kBgra; break;
    }
    if (tx.decode) tx.fmt = kBgra;
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    VkImageViewType vt = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (!CreateImage(tx.img, tx.fmt, w, h, d.cube ? 1 : d.depth, d.cube ? 6 : 1, tx.levels,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, vt, d.cube)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                  const uint32_t* colours) {
    auto t = std::chrono::steady_clock::now();
    Transition(tx.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px, decoded;
    bool bc = fi.compressed && !tx.decode;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t mw = std::max(tx.img.w >> l, 1u), mh = std::max(tx.img.h >> l, 1u), md = std::max(tx.img.d >> l, 1u);
            const std::vector<uint8_t>* src = &px;
            if (tx.decode) {
                DecodeDxt(fi.host, px, pitch, mw, mh, md, decoded);
                pitch = mw * 4;
                src = &decoded;
            }
            // rows as Vulkan reads them packed: block rows for DXT, texel rows else
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = pitch ? src->size() / pitch / md : 0;
            Alloc a = Upload(VkDeviceSize(row) * rows * md, 16);
            if (!a.cpu) {
                if (gpu_failed_) return;
                continue;
            }
            g_perf.tex_bytes += uint64_t(row) * rows * md;
            for (uint32_t z = 0; z < md; z++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
                    if (y < src_rows) std::memcpy(dst, &(*src)[(size_t(z) * src_rows + y) * pitch], std::min(pitch, row));
                    else std::memset(dst, 0, row);
                }
            VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1}, {0, 0, 0}, {mw, mh, md}};
            vkCmdCopyBufferToImage(Cb(), a.buf, tx.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    Transition(tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_perf.tex_ns += SinceNs(t);
    g_perf.texes++;
}

VkImageView VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                            DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return VK_NULL_HANDLE;
    Tex& t = textures_[key];
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        // sampled each frame, whole every kWholeEvery frames, staggered: every texture hashed whole each frame was most
        // of the render thread's hashing. A change in place without a lock that the samples miss shows a few frames late.
        // One whole hash found the same kCalmTurns times running waits kWholeCalm frames: most of a race's textures never
        // change, and hashing them whole was still a twentieth of the render thread on a Mali phone
        constexpr uint64_t kWholeEvery = 8, kWholeCalm = 32;
        constexpr uint8_t kCalmTurns = 4;
        uint32_t total = TotalSize(d);
        uint64_t extra = DirtyEpoch(d.data) ^ palette.hash;
        uint64_t sig = MemSig(d.data, total, kSigSamples * kSigSampleBytes) ^ extra;
        uint64_t every = t.calm >= kCalmTurns ? kWholeCalm : kWholeEvery;
        bool turn = total > kSigSamples * kSigSampleBytes && (presents_ + (d.data >> 7)) % every == 0;
        uint64_t whole = turn ? MemSig(d.data, total) ^ extra : t.whole;
        if (sig != t.sig || whole != t.whole || !same) {
            t.sig = sig;
            t.whole = turn ? whole : MemSig(d.data, total) ^ extra;
            t.calm = 0;
            UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        } else if (turn && t.calm < kCalmTurns) {
            t.calm++;
        }
    }
    Transition(t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.fmt[stage] = t.fmt;
    stages.levels[stage] = t.levels;
    return t.img.view;
}

VkImageView VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return VK_NULL_HANDLE;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    if (!d.cube && d.depth == 1 && !fi.compressed && !fi.depth)
        if (VkImageView v = BindFrameRegion(stage, d, stages)) return v;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    // CAR REFLECTIONS / ROAD REFLECTIONS off: the game draws them no more, and the stage reads a fixed opaque image
    // instead of the last one drawn
    constexpr uint32_t kReflectionsOff = 3;
    // by address too: a cube never drawn since the start would read its memory, never written
    if (d.cube && game::Opt().reflections == kReflectionsOff &&
        (tit != targets_.end() || cubes_.count(d.data) || d.data == game::g_env_cube.load()))
        return env_off_.view;
    if (!d.cube && !game::Opt().road_reflections && d.data && d.data == game::g_road_reflection.load())
        return road_off_.view;
    if (tit != targets_.end() && d.cube && !fi.depth && !fi.compressed)
        if (VkImageView v = BindCubeTargets(stage, d, stages)) return v;
    return BindGuestTexture(s, stage, d, fi, stages);
}

// A presented frame's rows read as a texture of their own (FrameRegion's surfaces: split screen's visual treatment
// reads each player's half of the frame): the top half has the frame's address and bound the whole frame, both views
// squeezed into it, and the bottom one matched no target and read guest memory, black. Copied out of the frame, at
// each bind as Feedback is, since the pass draws over the rows it reads
VkImageView VulkanBackend::BindFrameRegion(int stage, const TextureDesc& d, DrawTextures& stages) {
    for (uint32_t f : presented_) {
        if (!f || d.data < f) continue;
        auto it = targets_.find(TargetMapKey(f, false));
        if (it == targets_.end()) continue;
        Target& t = it->second;
        uint32_t pitch = GuestPitch(d.width), off = d.data - f;
        if (t.gw != d.width || d.height >= t.gh || off % pitch || off / pitch + d.height > t.gh) continue;
        float sc = float(t.h) / t.gh;
        uint32_t y = std::min(t.h - 1, uint32_t(std::lround(off / pitch * sc)));
        uint32_t h = std::clamp(uint32_t(std::lround(d.height * sc)), 1u, t.h - y);
        Image& im = regions_[d.data];
        if (im && (im.w != t.w || im.h != h || im.fmt != t.img.fmt)) Retire(im);
        if (!im && !CreateImage(im, t.img.fmt, t.w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_VIEW_TYPE_2D))
            return VK_NULL_HANDLE;
        Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, int32_t(y), 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      {0, 0, 0}, {t.w, h, 1}};
        vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        stages.rt[stage] = true;
        stages.info[stage].kind = kTex2D;
        stages.fmt[stage] = im.fmt;
        return im.view;
    }
    return VK_NULL_HANDLE;
}

// a cube drawn face by face as six targets: guest memory never sees those draws, so the faces are copied into a cube
// of their scale, again when any of them was drawn since
VkImageView VulkanBackend::BindCubeTargets(int stage, const TextureDesc& d, DrawTextures& stages) {
    Target* faces[6];
    for (uint32_t f = 0; f < 6; f++) {
        auto it = targets_.find(TargetMapKey(d.data + f * FaceSize(d), false));
        if (it == targets_.end() || it->second.gw != d.width || it->second.gh != d.height) return VK_NULL_HANDLE;
        faces[f] = &it->second;
        if (faces[f]->w != faces[0]->w || faces[f]->h != faces[0]->h) return VK_NULL_HANDLE;
    }
    CubeOfTargets& c = cubes_[d.data];
    uint32_t w = faces[0]->w, h = faces[0]->h;
    if (!c.img || c.img.w != w || c.img.h != h) {
        Retire(c.img);
        c = CubeOfTargets{};
        if (!CreateImage(c.img, kBgra, w, h, 1, 6, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_VIEW_TYPE_CUBE, true)) {
            cubes_.erase(d.data);
            return VK_NULL_HANDLE;
        }
        for (uint64_t& n : c.writes) n = ~0ull;
    }
    for (uint32_t f = 0; f < 6; f++) {
        Target& t = *faces[f];
        if (c.writes[f] == t.writes) continue;
        Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy r{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(Cb(), t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        c.writes[f] = t.writes;
    }
    Transition(c.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    return c.img.view;
}

bool VulkanBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) stages.view[i] = BindTexture(s, i, stages);
    // a stage without a texture reads a black one of its kind
    for (int i = 0; i < 4; i++) {
        if (stages.view[i]) continue;
        uint8_t kind = stages.info[i].kind;
        stages.view[i] = dummy_[kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0].view;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

constexpr float kDepth24Max = 16777215.0f;

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (draw_ds_ ? float(draw_ds_->w) / draw_ds_->gw : 1.0f);
    }
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, const DrawShaders& sh, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = sh.vh;
    k.ps = sh.ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++)
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    k.buffers = a.buffer_count;
    for (uint32_t i = 0; i < a.buffer_count; i++) k.strides[i] = a.buffers[i].stride;
    k.topo = uint32_t(a.topology) | (a.strip ? 0x10 : 0);
    k.has_rt = draw_rt_ != nullptr;
    k.has_ds = draw_ds_ != nullptr;
    k.rtfmt = draw_rt_ ? uint32_t(draw_rt_->img.fmt) : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (gpu_failed_) return;
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;

    // the targets, CPU writes to them first
    uint32_t rows, sub_h;
    Target* rt = EnsureColor(s.rt, rows, sub_h);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    // the zeta has a pitch of its own: a depth buffer wider than the target (Most Wanted's is the multisampled 1280x480
    // under its 640x480 frame) serves its top left corner; one smaller cannot
    if (rt && ds && (rt->w > ds->w || rt->h > ds->h)) {
        LOG_FIRST(16, "vulkan: depth %08X %ux%u dropped for the larger target %08X %ux%u", s.zs.data, ds->w, ds->h, s.rt.data, rt->w, rt->h);
        ds = nullptr;
    }
    // a target and depth of different scales (an effect sized one with the other): the smaller is made again at the
    // frame's scale, as it is from then on
    if (rt && ds && std::fabs(float(rt->w) / rt->gw - float(ds->w) / ds->gw) > 0.01f) {
        bool depth_smaller = float(ds->w) / ds->gw < float(rt->w) / rt->gw;
        const Surface& su = depth_smaller ? s.zs : s.rt;
        uint64_t key = TargetMapKey(mem::VirtualFromPhysical(su.data), depth_smaller);
        LOG_FIRST(16, "vulkan: %s %08X drawn with a %s of another scale, made at the frame's", depth_smaller ? "depth" : "target",
                  su.data, depth_smaller ? "target" : "depth");
        full_scale_.insert(key);
        ReleaseTarget(targets_.find(key));
        rt = EnsureColor(s.rt, rows, sub_h);
        ds = EnsureTarget(s.zs, true);
        if (rt && ds && (rt->w > ds->w || rt->h > ds->h)) ds = nullptr;
    }
    if (!rt && !ds) return;
    // a frame's rows: the CPU's pixels are the frame's, its own upload
    if (rt && !sub_h && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    draw_rt_ = rt;
    draw_ds_ = ds;

    DrawShaders sh = ClassifyShaders(s, a);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTextures(s, stages) || !draw_rt_ && !draw_ds_) return;
    // BLOOM off: the glow, the frame shrunk to 160x120 and blurred, is not added back onto the frame (ONE + ONE)
    if (!game::Opt().bloom && stages.small && draw_rt_ && draw_rt_->gw == guest_w_ && draw_rt_->gh == guest_h_ &&
        SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == kBlendOne && SimpleState(s, kMDestBlend) == kBlendOne)
        return;
    // SHADOWS off: the trees' and buildings' shadows on the road are the frame's one 2x multiply (DST_COLOR, SRC_COLOR)
    constexpr uint32_t kBlendDstColor = 0x306, kBlendSrcColor = 0x300;
    if (!game::Opt().shadows && draw_rt_ && draw_rt_->gw == guest_w_ && draw_rt_->gh == guest_h_ &&
        SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == kBlendDstColor &&
        SimpleState(s, kMDestBlend) == kBlendSrcColor)
        return;
    PixelKey pk = MakePixelKey(s, stages.info);
    // FOG off leaves the factor at 1, no fog for the D3D final combiner (C the fog colour). Most Wanted's takes the fog
    // alpha as an amount towards its constant (A fog alpha, B C0, C r0): at 1 the whole picture was that grey. Its A
    // becomes zero, the scene alone
    constexpr uint32_t kFinalFog = 3;
    if (!game::Opt().fog && pk.programmable && (pk.final_abcd >> 24 & 0xEF) == kFinalFog &&
        (pk.final_abcd >> 8 & 0xF) != kFinalFog)
        pk.final_abcd &= 0x00FFFFFF;
    if (last_ps_ && !std::memcmp(&pk, &last_pk_, sizeof(pk))) {
        sh.pkey = last_pkey_;
        sh.ph = last_ph_;
        sh.ps = last_ps_;
    } else {
        sh.pkey = Hash(&pk, sizeof(pk));
        EnsurePixelShader(pk, sh);
        last_pk_ = pk;
        last_pkey_ = sh.pkey, last_ph_ = sh.ph, last_ps_ = sh.ps;
    }
    if (!sh.vs || !sh.ps) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh, k);
    VkPipeline pipe = last_pipe_ && !std::memcmp(&k, &last_pso_, sizeof(k)) ? last_pipe_ : EnsurePipeline(k, sh.vs, sh.ps);
    last_pso_ = k;
    last_pipe_ = pipe;
    if (!pipe) return;
    g_perf.draws++;

    // everything uploaded before the rendering: a full ring flushes
    VkDeviceSize need = 2 * kUboRange + 1024;
    for (uint32_t i = 0; i < a.buffer_count; i++) need += a.buffers[i].bytes + 16;
    need += VkDeviceSize(a.index_count) * 4 + 16;
    if (need < kUploadSize / 4 && CurrentFrame().used + need > kUploadSize && !Submit(true, false)) return;
    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    if (game::Opt().fog) FillFogConstants(s, c);  // off: no fog mode, the shaders' fog factor stays 1
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(c.vattr, s.vattr.data(), sizeof(c.vattr));
    // the vertex constants as the last draw's: its copy in this frame's ring serves
    const Frame* cf = &CurrentFrame();
    Alloc vc;
    bool same_vsc = g_vsc_counted ? last_vsc_changes_ == g_vsc_changes
                                  : !std::memcmp(s.vsc.data(), last_vsc_.data(), sizeof(s.vsc));
    if (last_vc_frame_ == cf && last_vc_epoch_ == upload_epoch_ && same_vsc) {
        vc = last_vc_;
    } else {
        vc = Constants(s.vsc.data(), sizeof(s.vsc));
        if (!vc.cpu) return;
        if (g_vsc_counted) last_vsc_changes_ = g_vsc_changes;
        else std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
        last_vc_ = vc;
        last_vc_frame_ = &CurrentFrame();
        last_vc_epoch_ = upload_epoch_;
    }
    Alloc dc = Constants(&c, sizeof(c));
    if (!dc.cpu) return;
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc v = Upload(std::max<uint32_t>(b.bytes, 4), 16);
        if (!v.cpu) return;
        if (b.src && b.bytes) std::memcpy(v.cpu, b.src, b.bytes);
        vbufs[i] = v.buf;
        voffs[i] = v.off;
    }
    Alloc ib{};
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        ib = Upload(ibytes, 4);
        if (!ib.cpu) return;
        std::memcpy(ib.cpu, a.index_src, ibytes);
    }
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    VkSampler samplers[4];
    for (int i = 0; i < 4; i++) samplers[i] = StageSampler(s, i, stages, clamp);
    VkDescriptorSet set = Set(stages.view, samplers);

    // the rendering
    BeginRender(draw_rt_ ? &draw_rt_->img : nullptr, draw_ds_ ? &draw_ds_->img : nullptr);
    if (draw_rt_) draw_rt_->drawn = true, draw_rt_->writes++;
    if (draw_ds_) draw_ds_->drawn = true;
    vkCmdBindPipeline(Cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    BindSet(set, vc.off, dc.off);
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    float sc = W / size.w;
    // a frame's rows (FrameRegion): the surface's picture goes on them, clipped to them
    bool sub = draw_rt_ && sub_h;
    float oy = sub ? rows * sc : 0.0f;
    SetViewport(0, oy, W, sub ? sub_h * sc : H);
    int32_t x0 = int32_t(s.viewport.x * sc), y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc), y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    if (sub) {
        y0 = std::max(y0, 0) + int32_t(oy);
        y1 = std::min(y1 + int32_t(oy), int32_t(oy + sub_h * sc));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(Cb(), bf);
    vkCmdSetStencilReference(Cb(), VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
    if (a.buffer_count) vkCmdBindVertexBuffers(Cb(), 0, a.buffer_count, vbufs, voffs);
    if (a.index_count) {
        vkCmdBindIndexBuffer(Cb(), ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(Cb(), a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(Cb(), a.draw_count, 1, 0, 0);
    }
}

VkRect2D ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp(int32_t(x0 * sc), 0, int32_t(t.w)), tp = std::clamp(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp(int32_t(x1 * sc), 0, int32_t(t.w)), b = std::clamp(int32_t(y1 * sc), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}};
}

void VulkanBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                                uint32_t color, float z, uint32_t stencil, uint32_t rows, uint32_t sub_h) {
    std::vector<VkClearRect> rs;
    // a frame's rows (FrameRegion): the surface's rects moved onto them, clipped to them
    auto add = [&](int x0, int y0, int x1, int y1) {
        if (sub_h) {
            y0 = std::max(y0, 0) + int(rows);
            y1 = std::min(y1, int(sub_h)) + int(rows);
        }
        VkRect2D r = ScaledRect(t, x0, y0, x1, y1);
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    t.drawn = true, t.writes++;
    if (rs.empty()) return;
    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((flags & kClearStencil) && t.img.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (!ca.aspectMask) return;
        ca.clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (!sub_h && LatchCpuWrite(t)) UploadTarget(t, s.rt);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(color, ca.clearValue.color.float32);
    }
    // each rendering loads and stores its attachments whole, which on a tiled GPU costs about as much as drawing over
    // them: a clear goes into the rendering under way when that has the image, or, when it is all of it, is left for
    // the next rendering on it to load as cleared (the load ops of dynamic rendering or of a render pass)
    bool inside = rendering_ && (&t.img == rend_color_ || &t.img == rend_depth_);
    for (const VkClearRect& r : rs)
        inside = inside && r.rect.offset.x + r.rect.extent.width <= rend_w_ && r.rect.offset.y + r.rect.extent.height <= rend_h_;
    if (!inside) {
        const VkRect2D& r = rs[0].rect;
        if (rs.size() == 1 && !r.offset.x && !r.offset.y && r.extent.width == t.w && r.extent.height == t.h) {
            if (!t.depth) t.img.clear = ca.clearValue;
            if (ca.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) t.img.clear.depthStencil.depth = ca.clearValue.depthStencil.depth;
            if (ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) t.img.clear.depthStencil.stencil = ca.clearValue.depthStencil.stencil;
            t.img.pending |= ca.aspectMask;
            return;
        }
        if (t.depth) BeginRender(nullptr, &t.img);
        else BeginRender(&t.img, nullptr);
    }
    vkCmdClearAttachments(Cb(), 1, &ca, uint32_t(rs.size()), rs.data());
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    if (gpu_failed_) return;
    uint32_t rows = 0, sub_h = 0;
    Target* rt = (flags & kClearTarget) ? EnsureColor(s.rt, rows, sub_h) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (rt) ClearTarget(*rt, s, count, rects, flags, color, z, stencil, rows, sub_h);
    if (ds) ClearTarget(*ds, s, count, rects, flags, color, z, stencil);
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    if (gpu_failed_) return;
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;
    float dst_scale = float(dst_t->w) / dst_t->gw;
    bool listed = count && src_rects;
    // of different scales (an effect sized target, TargetScale): drawn over, in guest coordinates
    float src_scale = float(src_t->w) / src_t->gw;
    if (std::fabs(src_scale - dst_scale) > 0.01f) {
        for (uint32_t i = 0, n = listed ? count : 1; i < n; i++) {
            int32_t l = 0, t = 0, r = int32_t(src_t->gw), b = int32_t(src_t->gh), dx = 0, dy = 0;
            if (listed) {
                GuestRect g = ReadGuestRect(src_rects, i);
                l = g.left, t = g.top, r = g.right, b = g.bottom, dx = l, dy = t;
                if (dst_points) ReadGuestPoint(dst_points, i, dx, dy);
            }
            if (r <= l || b <= t) continue;
            float uv[4] = {float(l) / src_t->gw, float(t) / src_t->gh, float(r - l) / src_t->gw, float(b - t) / src_t->gh};
            VkRect2D to = ScaledRect(*dst_t, dx, dy, dx + r - l, dy + b - t);
            if (!to.extent.width || !to.extent.height) continue;
            uint32_t taps = uint32_t(std::max(1.0f, std::ceil(src_scale / dst_scale - 1e-3f)));
            Blit(dst_t->img, to, src_t->img, uv, {.alpha = true, .taps = taps});
        }
        dst_t->drawn = true, dst_t->writes++;
        return;
    }
    Transition(src_t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst_t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t n = listed ? count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t->w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px, py;
                ReadGuestPoint(dst_points, i, px, py);
                dx = ScaleUnsigned(px, dst_scale);
                dy = ScaleUnsigned(py, dst_scale);
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        regions.push_back({{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(l), int32_t(t), 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           {int32_t(dx), int32_t(dy), 0},
                           {r - l, b - t, 1}});
    }
    if (!regions.empty())
        vkCmdCopyImage(Cb(), src_t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t->img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t->drawn = true, dst_t->writes++;
}

// ---- read-backs

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    if (gpu_failed_) return;
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    Image down;
    Image* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Retire(down);  // kept until its recorded use has safely finished
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (gpu_failed_) return;
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    if (gpu_failed_) return false;
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed(), now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        if (finished && recent && (best < 0 || l.fence[i] > l.fence[best])) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        const uint8_t* p = l.rb[best].mem.map;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    if (gpu_failed_) return;
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        for (Buffer& b : l.rb) Retire(b);
        Retire(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = CopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot].buf && !CreateBuffer(l.rb[slot], VkDeviceSize(t.gw) * t.gh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return;
    EndRender();
    VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t.gw, t.gh, 1}};
    vkCmdCopyImageToBuffer(Cb(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot].buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    l.fence[slot] = value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

// ---- present

// the swap chain made again when Android gave a new window, the size or vsync changed; the surface too when the
// window is new, lost, or could not be made last time. false: nothing to show on, the frame is dropped
bool VulkanBackend::AcquireImage(uint32_t& index) {
    if (!host::Foreground()) return false;
    if (!surface_ || surface_lost_ || host::SurfaceEpoch() != surface_epoch_) {
        if (surface_) LOGI("vulkan: new surface (%s)", surface_lost_ ? "lost" : "new window");
        DestroySwapchain();
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (!CreateSurface()) return false;
        swap_stale_ = true;
    }
    int w, h;
    host::WindowPixels(w, h);
    if (!swap_ || swap_stale_ || uint32_t(w) != swap_extent_.width || uint32_t(h) != swap_extent_.height ||
        app::Get().vsync != vsync_) {
        if (!CreateSwapchain()) return false;
    }
    // a bounded wait: a window Android is taking away may never hand an image back
    constexpr uint64_t kAcquireTimeout = 500'000'000;
    for (int tries = 0; tries < 2; tries++) {
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, kAcquireTimeout, CurrentFrame().acquire, VK_NULL_HANDLE, &index);
        // suboptimal is left alone: Android says so every frame on a turned screen, where only the compositor's
        // rotation differs, and making the chain again each frame freed images the GPU was still drawing (Mali)
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return true;
        if (r == VK_TIMEOUT || r == VK_NOT_READY) return false;
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            surface_lost_ = true;
            return false;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR || !CreateSwapchain()) return false;
    }
    return false;
}

void VulkanBackend::DrawFrame(Target* t, uint32_t index) {
    EndRender();
    VkCommandBuffer cb = Cb();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = swap_images_[index];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    // the frame letterboxed to the game's aspect over black
    float aspect = game::Aspect();
    float w = float(swap_extent_.width), h = float(swap_extent_.height);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    float c[16] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, 1, 1};
    // the screen filter and anti-aliasing, off unless the player picks them: the plain blit then
    uint32_t filter = game::Opt().screen_filter, aa = game::Opt().anti_aliasing;
    bool post = t && (filter || aa);
    if (post) {
        c[8] = 1.0f / float(std::max(t->w, 1u));
        c[9] = 1.0f / float(std::max(t->h, 1u));
        c[10] = float(t->gh ? t->gh : guest_h_);
        c[12] = float(filter);
        c[13] = aa == game::kAaFxaa ? 1.0f : 0.0f;
        // supersampled: the texels each screen pixel covers, averaged
        if (aa == game::kAaSsaa) c[6] = std::clamp(std::round(float(t->h) / std::max(vh, 1.0f)), 1.0f, 4.0f);
    }
    Alloc cbuf = Constants(c, sizeof(c));
    if (!cbuf.cpu) return;
    VkPipeline pipe = t ? FullScreenPipeline(post ? kPresentHlsl : kBlitHlsl, "psmain", swap_fmt_, VK_FORMAT_UNDEFINED, false)
                        : VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pipe) {
        Transition(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkImageView views[4] = {t->img.view, lut_.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
        const VkSampler samplers[4] = {linear_, point_, point_, point_};
        set = Set(views, samplers);
    }
    VkRenderingAttachmentInfoKHR ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    ca.imageView = swap_views_[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfoKHR ri{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
    ri.renderArea = {{0, 0}, swap_extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (dynamic_) {
        begin_rendering_(cb, &ri);
    } else {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = Pass(swap_fmt_, VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT);
        bi.framebuffer = Framebuffer(swap_fmt_, VK_FORMAT_UNDEFINED, swap_views_[index], VK_NULL_HANDLE,
                                     swap_extent_.width, swap_extent_.height);
        bi.renderArea = ri.renderArea;
        bi.clearValueCount = 1;
        bi.pClearValues = &ca.clearValue;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    }
    if (pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        BindSet(set, cbuf.off, cbuf.off);
        SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
        SetScissor(0, 0, int32_t(w), int32_t(h));
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    RenderUi();
    if (dynamic_)
        end_rendering_(cb);
    else
        vkCmdEndRenderPass(cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

void VulkanBackend::RenderUi() {
    if (!ui_) return;
    std::lock_guard lock(app::UiLock());
    if (ui::ApplyGameFont()) ImGui_ImplVulkan_CreateFontsTexture();  // waits for the queue before it swaps the texture
    ImGui_ImplVulkan_NewFrame();
    ui::NewFrame(float(swap_extent_.width), float(swap_extent_.height));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    touch::Draw();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), Cb());
}

void VulkanBackend::UploadGammaRamp() {
    if (gpu_failed_) return;
    ramp_dirty_ = false;
    Alloc a = Upload(256 * 4, 16);
    if (!a.cpu) return;
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {256, 1, 1}};
    vkCmdCopyBufferToImage(Cb(), a.buf, lut_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// textures kept by count and by bytes: without BC (PowerVR, Mali) DXT is decoded to BGRA, 4 to 8 times its size, and a
// thousand of those ran a phone out of memory within minutes (#17)
void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;  // a phone has less memory than a PC: fewer kept
    constexpr uint64_t kIdleFrames = 600;
    constexpr VkDeviceSize kTextureBytes = 384ull << 20;
    if (presents_ % kSweepEvery != 0) return;
    VkDeviceSize bytes = 0;
    for (const auto& [key, t] : textures_) bytes += t.img.mem.size;
    if (textures_.size() <= kTexturesKept && bytes <= kTextureBytes) return;
    std::vector<std::pair<uint64_t, uint64_t>> lru;  // last frame drawn, key
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            bytes -= it->second.img.mem.size;
            Retire(it->second.img);
            it = textures_.erase(it);
        } else {
            lru.push_back({it->second.frame, it->first});
            ++it;
        }
    }
    if (bytes <= kTextureBytes) return;
    // still over: the least recently drawn go, but none drawn in the last second; those are made again when drawn
    std::sort(lru.begin(), lru.end());
    for (const auto& [frame, key] : lru) {
        if (bytes <= kTextureBytes || frame + kSweepEvery >= presents_) break;
        auto it = textures_.find(key);
        bytes -= it->second.img.mem.size;
        Retire(it->second.img);
        textures_.erase(it);
    }
}

void VulkanBackend::ApplySettings() {
    if (gpu_failed_) return;
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    if (!Submit(true, false)) return;
    for (auto& [key, t] : targets_) {
        Retire(t.img);
        Retire(t.fb);
    }
    targets_.clear();
    for (auto& [data, c] : cubes_) Retire(c.img);
    cubes_.clear();
    for (auto& [data, im] : regions_) Retire(im);
    regions_.clear();
    full_scale_.clear();
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    LOGI("vulkan: render scale %.3f", scale_);
}

void VulkanBackend::SavePipelineCache() {
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, cache_, &size, nullptr) != VK_SUCCESS || size == cache_saved_) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(dev_, cache_, &size, blob.data()) != VK_SUCCESS) return;
    std::filesystem::path p = app::ExeDir() / "shadercache" / "pipelines.bin", tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(blob.data(), std::streamsize(size));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    cache_saved_ = size;
}

void VulkanBackend::Present(const Surface& front) {
    if (gpu_failed_) return;
    Target* t = EnsureTarget(front, false);
    if (t && std::find(std::begin(presented_), std::end(presented_), t->data) == std::end(presented_)) {
        std::move_backward(std::begin(presented_), std::end(presented_) - 1, std::end(presented_));
        presented_[0] = t->data;
    }
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    if (gpu_failed_) return;
    uint32_t index = 0;
    auto at = std::chrono::steady_clock::now();
    bool shown = AcquireImage(index);
    g_perf.acquire_ns += SinceNs(at);
    if (shown) {
        present_index_ = index;
        DrawFrame(t, index);
    }
    if (gpu_failed_) return;
    bool timed = pass_log_ && pass_times_ && pass_stamps_;
    if (timed) vkCmdWriteTimestamp(Cb(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_times_, pass_stamps_++);
    if (!Submit(false, shown)) return;
    if (timeline_ok_) QueuePoolReset(CurrentFrame());
    present_value_[presents_ % (kFrames + 1)].store(value_);
    if (pass_log_) {
        // a stall every ten seconds, for the diagnostic only
        std::string out;
        std::vector<uint64_t> t(pass_stamps_);
        if (!WaitValue(value_)) return;
        if (timed && vkGetQueryPoolResults(dev_, pass_times_, 0, pass_stamps_, t.size() * 8, t.data(), 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            uint64_t mask = time_bits_ >= 64 ? ~0ull : (1ull << time_bits_) - 1;
            for (size_t i = 0; i < pass_list_.size() && i + 1 < t.size(); i++) {
                char ms[32];
                std::snprintf(ms, sizeof(ms), " %.2f", double((t[i + 1] - t[i]) & mask) * props_.limits.timestampPeriod / 1e6);
                out += " [" + pass_list_[i] + ms + "]";
            }
        }
        LOGR("perf: frame %llu renderings (GPU ms to the next):%s", (unsigned long long)presents_, out.c_str());
    }
    if (shown) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swap_done_[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &index;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) swap_stale_ = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    }
    pass_list_.clear();
    pass_stamps_ = 0;
    pass_log_ = diag::Perf() && time_bits_ && presents_ % 600 == 599;
    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    Frame& next = CurrentFrame();
    auto wt = std::chrono::steady_clock::now();
    if (!ResetFrame(next)) return;
    if (timeline_ok_) {
        AwaitPoolReset(next);
        if (gpu_failed_) return;
    } else {
        VkResult reset = vkResetCommandPool(dev_, next.pool, 0);
        if (reset != VK_SUCCESS) {
            FailGpu("vkResetCommandPool", reset, next.value);
            return;
        }
    }
    g_perf.frame_wait_ns += SinceNs(wt);
    Begin();
    if (gpu_failed_) return;
    if (pass_log_) {
        if (!pass_times_) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kPassStamps;
            if (vkCreateQueryPool(dev_, &qi, nullptr, &pass_times_) != VK_SUCCESS) pass_times_ = VK_NULL_HANDLE;
        }
        if (pass_times_) vkCmdResetQueryPool(Cb(), pass_times_, 0, kPassStamps);
    }
    ReleaseOldTextures();
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
        if (time_bits_) g_gpu_load = float(std::min(100.0, gpu_ns_ / (el * 1e7)));
        gpu_ns_ = 0;
    }
    if (presents_ % 600 == 0) SavePipelineCache();
    if (!preload_started_ && game::Opt().shader_preload) StartPreload();
    ApplySettings();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void VulkanBackend::WaitGpuFrame(uint64_t n) {
    if (!gpu_failed_) WaitValue(present_value_[n % (kFrames + 1)].load());
}

VulkanBackend* g_vulkan = nullptr;

}  // namespace

std::unique_ptr<Backend> CreateBackend() {
    auto backend = std::make_unique<VulkanBackend>();
    g_vulkan = backend.get();
    return backend;
}

void WaitGpuFrame(uint64_t n) {
    if (g_vulkan) g_vulkan->WaitGpuFrame(n);
}

// DXT1, 3 or 5 blocks as BGRA texels, for src/game/prompts.cpp's icons
void DecodeBlocks(Host host, const std::vector<uint8_t>& in, uint32_t row_pitch, uint32_t w, uint32_t h,
                  std::vector<uint8_t>& out) {
    DecodeDxt(host, in, row_pitch, w, h, 1, out);
}

}  // namespace gpu

namespace host {
float VulkanGpuLoad() { return gpu::g_gpu_load.load(std::memory_order_relaxed); }
}  // namespace host
