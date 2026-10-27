// CPU reference renderer. Surfaces, textures, vertex programs, register combiners and fixed function stages are
// evaluated per the XDK's definitions, decoded the way gpu/shaders.cpp decodes them for HLSL.
// PROBES="x,y x,y": log each colour written at those pixels of linear PROBE_WIDTH (640) wide targets, the first
// probe with its triangle's vertices; TRACE="x,y": log the combiner registers of fragments there.
#include "reference_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "core/log.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace reference {

using namespace gpu;

namespace {

struct Vec {
    float c[4] = {};
    float& operator[](int i) { return c[i]; }
    float operator[](int i) const { return c[i]; }
};
Vec V(float x, float y, float z, float w) { return {{x, y, z, w}}; }
float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
// D3DCOLOR to rgba
Vec Unpack(uint32_t argb) {
    return V(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f);
}
Vec Rgb565(uint16_t c) { return V(((c >> 11) & 31) / 31.f, ((c >> 5) & 63) / 63.f, (c & 31) / 31.f, 1); }
bool Depth16Format(uint32_t f) { return f == 0x2C || f == 0x2D || f == 0x30 || f == 0x31; }

// swizzled texel order: x and y bits interleaved from bit 0, x first, while either axis has bits left
uint32_t SwizzleOffset(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t xbits = 0, ybits = 0;
    while ((1u << xbits) < w) xbits++;
    while ((1u << ybits) < h) ybits++;
    uint32_t out = 0, at = 0;
    for (uint32_t i = 0; i < xbits || i < ybits; i++) {
        if (i < xbits) out |= ((x >> i) & 1) << at++;
        if (i < ybits) out |= ((y >> i) & 1) << at++;
    }
    return out;
}

// a surface in guest memory

struct Plane {
    bool ok = false, swizzled = false;
    uint32_t data = 0, format = 0, w = 0, h = 0, pitch = 0, bytes = 4;

    uint8_t* At(uint32_t x, uint32_t y) const {
        uint32_t off = swizzled ? SwizzleOffset(x, y, w, h) * bytes : y * pitch + x * bytes;
        return static_cast<uint8_t*>(kernel::HostPtr(data)) + off;
    }
    uint32_t End() const { return data + pitch * h; }
    bool Depth16() const { return Depth16Format(format); }

    Vec Load(uint32_t x, uint32_t y) const {
        const uint8_t* p = At(x, y);
        if (bytes != 4) {
            uint16_t c;
            std::memcpy(&c, p, 2);
            return Rgb565(c);
        }
        uint32_t c;
        std::memcpy(&c, p, 4);
        Vec v = Unpack(c);
        if (format == 0x07 || format == 0x1E) v[3] = 1;  // X8R8G8B8
        return v;
    }
    // mask: 1 blue, 2 green, 4 red, 8 alpha
    void Store(uint32_t x, uint32_t y, Vec c, uint32_t mask) const {
        uint8_t* p = At(x, y);
        auto q = [](float v, float top) { return std::lround(Sat(v) * top); };
        if (bytes != 4) {
            uint16_t v = uint16_t(q(c[0], 31)) << 11 | uint16_t(q(c[1], 63)) << 5 | uint16_t(q(c[2], 31));
            std::memcpy(p, &v, 2);
            return;
        }
        const int channel[4] = {2, 1, 0, 3};  // bytes b g r a
        for (int i = 0; i < 4; i++)
            if (mask >> i & 1) p[i] = uint8_t(q(c[channel[i]], 255));
    }
    void LoadDepth(uint32_t x, uint32_t y, uint32_t& z, uint32_t& stencil) const {
        const uint8_t* p = At(x, y);
        if (Depth16()) {
            uint16_t v;
            std::memcpy(&v, p, 2);
            z = v;
            stencil = 0;
            return;
        }
        uint32_t v;
        std::memcpy(&v, p, 4);
        z = v >> 8;
        stencil = v & 0xFF;
    }
    void StoreDepth(uint32_t x, uint32_t y, uint32_t z, uint32_t stencil) const {
        uint8_t* p = At(x, y);
        if (Depth16()) {
            uint16_t v = uint16_t(z);
            std::memcpy(p, &v, 2);
            return;
        }
        uint32_t v = z << 8 | (stencil & 0xFF);
        std::memcpy(p, &v, 4);
    }
};

Plane OpenPlane(const Surface& s) {
    Plane p;
    TextureDesc d;
    if (!s.header || !Describe(s.header, d)) return p;
    p.ok = true;
    p.data = d.data;
    p.format = d.format;
    p.w = d.width;
    p.h = d.height;
    p.swizzled = !d.linear;
    p.bytes = Format(d.format).bits / 8;
    p.pitch = d.linear ? d.pitch : d.width * p.bytes;
    return p;
}

// textures, level 0 decoded to floats

struct Image {
    uint32_t w = 1, h = 1, faces = 1;
    bool depth16 = false;
    std::vector<Vec> texels[6];
    uint32_t lo = 0, hi = 0;  // guest bytes it was read from
};
std::map<uint64_t, std::shared_ptr<Image>> g_images;

// DXT1/3/5 blocks
void DecodeBlocks(const TextureDesc& d, Host host, const std::vector<uint8_t>& in, std::vector<Vec>& out) {
    uint32_t bw = std::max(1u, (d.width + 3) / 4), bh = std::max(1u, (d.height + 3) / 4);
    uint32_t block = host == Host::BC1 ? 8 : 16;
    out.assign(size_t(d.width) * d.height, Vec{});
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = &in[(size_t(by) * bw + bx) * block];
            const uint8_t* cb = host == Host::BC1 ? b : b + 8;
            uint16_t e0 = uint16_t(cb[0] | cb[1] << 8), e1 = uint16_t(cb[2] | cb[3] << 8);
            uint32_t sel = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
            Vec pal[4] = {Rgb565(e0), Rgb565(e1)};
            if (host != Host::BC1 || e0 > e1) {
                for (int i = 0; i < 3; i++) {
                    pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3;
                    pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3;
                }
                pal[2][3] = pal[3][3] = 1;
            } else {
                for (int i = 0; i < 3; i++) pal[2][i] = (pal[0][i] + pal[1][i]) / 2;
                pal[2][3] = 1;
                pal[3] = V(0, 0, 0, 0);
            }
            float alpha[16];
            if (host == Host::BC2) {
                for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> ((i & 1) * 4)) & 15) / 15.f;
            } else if (host == Host::BC3) {
                float ramp[8] = {b[0] / 255.f, b[1] / 255.f};
                if (b[0] > b[1]) {
                    for (int i = 1; i < 7; i++) ramp[i + 1] = ((7 - i) * ramp[0] + i * ramp[1]) / 7;
                } else {
                    for (int i = 1; i < 5; i++) ramp[i + 1] = ((5 - i) * ramp[0] + i * ramp[1]) / 5;
                    ramp[6] = 0;
                    ramp[7] = 1;
                }
                uint64_t idx = 0;
                for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = ramp[(idx >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= d.width || y >= d.height) continue;
                Vec c = pal[(sel >> (2 * i)) & 3];
                if (host != Host::BC1) c[3] = alpha[i];
                out[size_t(y) * d.width + x] = c;
            }
        }
}

std::shared_ptr<Image> FindImage(uint32_t header) {
    TextureDesc d;
    if (!header || !Describe(header, d)) return nullptr;
    uint64_t key = uint64_t(d.data) << 32 ^ uint64_t(d.format) << 24 ^ uint64_t(d.width) << 12 ^ d.height ^ (d.cube ? 1ull << 63 : 0);
    if (auto it = g_images.find(key); it != g_images.end()) return it->second;
    auto img = std::make_shared<Image>();
    const FormatInfo& fi = Format(d.format);
    img->w = d.width;
    img->h = d.height;
    img->faces = d.cube ? 6 : 1;
    img->depth16 = Depth16Format(d.format);
    img->lo = d.data;
    img->hi = d.data + (d.cube ? FaceSize(d) * 6 : TotalSize(d));
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    for (uint32_t f = 0; f < img->faces; f++) {
        std::vector<uint8_t> raw;
        uint32_t pitch;
        DecodeLevel(d, 0, f, guest, nullptr, raw, pitch);
        std::vector<Vec>& out = img->texels[f];
        if (fi.compressed) {
            DecodeBlocks(d, fi.host, raw, out);
            continue;
        }
        out.resize(size_t(d.width) * d.height);
        if (fi.host == Host::R32F) {
            for (size_t i = 0; i < out.size(); i++) {
                float v;
                std::memcpy(&v, &raw[i * 4], 4);
                out[i] = V(v, 0, 0, 0);
            }
            continue;
        }
        for (uint32_t y = 0; y < d.height; y++)
            for (uint32_t x = 0; x < d.width; x++) {
                const uint8_t* p = &raw[size_t(y) * pitch + x * 4];
                out[size_t(y) * d.width + x] = V(p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f);
            }
    }
    g_images[key] = img;
    return img;
}

// guest bytes written: the images read from them are stale
void Written(uint32_t lo, uint32_t hi) {
    for (auto it = g_images.begin(); it != g_images.end();) {
        if (it->second->lo < hi && lo < it->second->hi) it = g_images.erase(it);
        else ++it;
    }
}

// a texture stage's image and D3DTSS address and filter state
struct Unit {
    std::shared_ptr<Image> img;
    uint32_t address_u = 1, address_v = 1, mag = 2, min = 2;
    Vec border;

    // D3DTADDRESS: 1 wrap, 2 mirror, 4 border, else clamp
    static int Address(int i, int n, uint32_t mode, bool& outside) {
        if (mode == 1) {
            i %= n;
            return i < 0 ? i + n : i;
        }
        if (mode == 2) {
            int period = 2 * n;
            i %= period;
            if (i < 0) i += period;
            return i < n ? i : period - 1 - i;
        }
        if (mode == 4 && (i < 0 || i >= n)) outside = true;
        return std::clamp(i, 0, n - 1);
    }
    Vec Texel(int face, int x, int y) const {
        bool outside = false;
        x = Address(x, int(img->w), address_u, outside);
        y = Address(y, int(img->h), address_v, outside);
        if (outside) return border;
        return img->texels[face][size_t(y) * img->w + x];
    }
    // point when both filters are, else bilinear at texel centres
    Vec Sample(int face, float u, float v) const {
        if (!img) return V(0, 0, 0, 1);
        float sx = u * img->w - 0.5f, sy = v * img->h - 0.5f;
        if (mag < 2 && min < 2) return Texel(face, int(std::floor(u * img->w)), int(std::floor(v * img->h)));
        int x = int(std::floor(sx)), y = int(std::floor(sy));
        float fx = sx - x, fy = sy - y;
        Vec t00 = Texel(face, x, y), t10 = Texel(face, x + 1, y), t01 = Texel(face, x, y + 1), t11 = Texel(face, x + 1, y + 1);
        Vec r;
        for (int i = 0; i < 4; i++) r[i] = (t00[i] * (1 - fx) + t10[i] * fx) * (1 - fy) + (t01[i] * (1 - fx) + t11[i] * fx) * fy;
        return r;
    }
    // a cube along a direction; a plain texture takes x y as its coordinates
    Vec SampleDir(float x, float y, float z) const {
        if (!img) return V(0, 0, 0, 1);
        if (img->faces < 6) return Sample(0, x, y);
        float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z), major, s, t;
        int face;
        if (ax >= ay && ax >= az) {
            face = x >= 0 ? 0 : 1;
            major = ax;
            s = x >= 0 ? -z : z;
            t = -y;
        } else if (ay >= az) {
            face = y >= 0 ? 2 : 3;
            major = ay;
            s = x;
            t = y >= 0 ? z : -z;
        } else {
            face = z >= 0 ? 4 : 5;
            major = az;
            s = z >= 0 ? x : -x;
            t = -y;
        }
        if (major == 0) major = 1;
        Unit edge = *this;
        edge.address_u = edge.address_v = 3;
        return edge.Sample(face, (s / major + 1) * 0.5f, (t / major + 1) * 0.5f);
    }
};

// vertex programs

// the microcode fields, as shaders.cpp names them
struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

// magnitude clamped between 1 / 2^64 and 2^64, sign kept
float MagClamp(float v) {
    float m = std::clamp(std::fabs(v), 5.42101e-20f, 1.84467e+19f);
    return v < 0 ? -m : m;
}
float MacMul(float a, float b) { return (a == 0 || b == 0) ? 0 : a * b; }
Vec Splat(float v) { return V(v, v, v, v); }

// 1 mov, 2 mul, 3 add, 4 mad, 5 dp3, 6 dph, 7 dp4, 8 dst, 9 min, 10 max, 11 slt, 12 sge, 13 arl
Vec Mac(uint32_t op, const Vec& a, const Vec& b, const Vec& c) {
    Vec r;
    switch (op) {
        case 1:
        case 13: return a;
        case 2: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]); return r;
        case 3: for (int i = 0; i < 4; i++) r[i] = a[i] + c[i]; return r;
        case 4: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]) + c[i]; return r;
        case 5: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        case 6: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]);
        case 7: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        case 8: return V(1, a[1] * b[1], a[2], b[3]);
        case 9: for (int i = 0; i < 4; i++) r[i] = std::min(a[i], b[i]); return r;
        case 10: for (int i = 0; i < 4; i++) r[i] = std::max(a[i], b[i]); return r;
        case 11: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.f : 0.f; return r;
        case 12: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.f : 0.f; return r;
        default: return r;
    }
}

// 1 mov, 2 rcp, 3 rcc, 4 rsq, 5 exp, 6 log, 7 lit
Vec Ilu(uint32_t op, const Vec& c) {
    float v = c[0];
    switch (op) {
        case 1: return c;
        case 2: return Splat(1.0f / v);
        case 3: return Splat(MagClamp(1.0f / v));
        case 4: return Splat(v == 0 ? INFINITY : 1.0f / std::sqrt(std::fabs(v)));
        case 5: {
            float whole = std::floor(v);
            return V(std::exp2(whole), v - whole, std::exp2(v), 1);
        }
        case 6: {
            float mag = std::fabs(v);
            if (mag == 0) return V(-INFINITY, 1, -INFINITY, 1);
            float ex = std::floor(std::log2(mag));
            return V(ex, mag / std::exp2(ex), std::log2(mag), 1);
        }
        case 7: {
            const float cap = 127.99609375f;
            float power = std::clamp(c[3], -cap, cap), diffuse = std::max(c[0], 0.f), spec = std::max(c[1], 0.f);
            return V(1, diffuse, diffuse > 0 ? std::exp2(power * std::log2(spec)) : 0, 1);
        }
        default: return Vec{};
    }
}

struct VertexProgram {
    const State& s;
    Vec in[16];
    Vec temp[12], out[16];  // out 0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7 oB0, 8 oB1, 9 to 12 oT0 to oT3
    int a0 = 0;

    // temp 12 is oPos
    Vec& Reg(uint32_t r) { return r == 12 ? out[0] : temp[r]; }

    // operand A 0, B 1, C 2
    Vec Operand(const Tok& k, int which) {
        uint32_t neg, sw[4], reg, mux;
        if (which == 0) {
            neg = k.f(1, 8, 1);
            sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
            reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
        } else if (which == 1) {
            neg = k.f(2, 25, 1);
            sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
            reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
        } else {
            neg = k.f(2, 10, 1);
            sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
            reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
            uint32_t ilu = k.f(1, 25, 3);
            if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
        }
        Vec base;
        if (mux == 1) {
            base = reg == 12 ? out[0] : temp[reg > 12 ? 0 : reg];
        } else if (mux == 2) {
            base = in[k.f(1, 9, 4)];
        } else {
            int cr = int(k.f(1, 13, 8));
            if (k.f(3, 1, 1)) cr = std::clamp(cr + a0, 0, 191);
            const auto& c = s.vsc[size_t(cr)];
            base = V(c[0], c[1], c[2], c[3]);
        }
        Vec r;
        for (int i = 0; i < 4; i++) r[i] = neg ? -base[int(sw[i])] : base[int(sw[i])];
        return r;
    }

    static void Masked(Vec& dst, const Vec& src, uint32_t xyzw) {
        for (int i = 0; i < 4; i++)
            if (xyzw >> (3 - i) & 1) dst[i] = src[i];
    }

    void Run(const std::vector<uint32_t>& prog) {
        out[3] = out[7] = out[9] = out[10] = out[11] = out[12] = V(0, 0, 0, 1);
        out[6] = V(1, 1, 1, 1);
        for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
            Tok k{&prog[n]};
            uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
            if (mac || ilu) {
                Vec a = Operand(k, 0), b = Operand(k, 1), c = Operand(k, 2);
                Vec mr = mac ? Mac(mac, a, b, c) : Vec{}, ir = ilu ? Ilu(ilu, c) : Vec{};
                uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
                bool use_ilu = k.f(3, 2, 1) == 1;
                // the output register is written before the temporaries
                if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13)))
                    Masked(out[k.f(3, 3, 8) & 0xF], use_ilu ? ir : mr, omask);
                if (ilu) {
                    uint32_t dst = mac ? 1 : r;
                    if (dst <= 12) Masked(Reg(dst), ir, k.f(3, 16, 4));
                }
                if (mac == 13) {
                    a0 = int(std::floor(mr[0] + 0.001f));
                } else if (mac) {
                    uint32_t m = ilu && r == 1 ? 0 : k.f(3, 24, 4);
                    if (r <= 12) Masked(Reg(r), mr, m);
                }
            }
            if (k.f(3, 0, 1)) break;
        }
    }
};

float FogAmount(const State& s, float d) {
    if (!s.rs[kRsFogEnable]) return 1;
    float start = AsFloat(s.rs[kRsFogStart]), end = AsFloat(s.rs[kRsFogEnd]), density = AsFloat(s.rs[kRsFogDensity]);
    switch (s.rs[kRsFogTableMode]) {
        case 1: return std::exp(-d * density);
        case 2: return std::exp(-(d * density) * (d * density));
        case 3: return end == start ? 1 : (end - d) / (end - start);
        default: return 1;
    }
}

// per fragment shading

enum Attr { kD0, kD1, kB0, kB1, kFog, kT0, kAttrs = kT0 + 4 };

struct Fragment {
    Vec v0, v1, fog;  // fog: colour, amount in w
    Vec tc[4];
};

// what every fragment of a draw shares
struct Material {
    PixelKey key;
    TexInfo info[4];
    Unit unit[4];
    Vec pc[18];  // combiner c0 c1 per stage, then the final combiner's
    Vec tfactor;
    const State* s = nullptr;
};

struct Trace {
    bool on = false;  // the fragment at the TRACE pixel
};
Trace g_trace;

// unorm to byte b, less lo under 128 or hi from 128, over d
float SByte(float u, float lo, float hi, float d) {
    float b = u * 255;
    return (b - (b >= 128 ? hi : lo)) / d;
}
// a dot stage's operand: the source stage's rgb, raw or read back as signed bytes
Vec DotInput(uint32_t map, const Vec& t) {
    float lo, hi, d;
    switch (map) {
        case 1: lo = 128, hi = 128, d = 127; break;
        case 2: lo = -0.5f, hi = 255.5f, d = 127.5f; break;
        case 3: lo = 0, hi = 256, d = 127; break;
        default: return t;
    }
    return V(SByte(t[0], lo, hi, d), SByte(t[1], lo, hi, d), SByte(t[2], lo, hi, d), 0);
}
float Dot3(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Combiners {
    const Material& m;
    const Fragment& f;
    Vec t[4], v0, v1, r0, r1;
    Vec e, fprod;  // final combiner E and F
    int stage = 0;
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;

    // a texture stage with rect coordinates takes texels
    void Norm(int i, float& u, float& v) const {
        if (m.info[i].rect && m.unit[i].img) {
            u /= m.unit[i].img->w;
            v /= m.unit[i].img->h;
        }
    }
    // the depth texel against ref by D3DRS_SHADOWFUNC, point sampled
    Vec Shadow(int i, float u, float v, float ref, bool compare) const {
        const Unit& u0 = m.unit[i];
        float top = u0.img && u0.img->depth16 ? 65535.f : 16777215.f;
        float texel = u0.Sample(0, u, v)[0] * top, z = compare ? std::clamp(ref, 0.f, top) : 0;
        bool pass;
        switch (m.s->rs[kRsShadowFunc]) {  // GL compare enums, as in Passes
            case 0x200: pass = false; break;
            case 0x201: pass = texel < z; break;
            case 0x202: pass = texel == z; break;
            case 0x203: pass = texel <= z; break;
            case 0x204: pass = texel > z; break;
            case 0x205: pass = texel != z; break;
            case 0x206: pass = texel >= z; break;
            default: pass = true; break;
        }
        return Splat(pass ? 1.f : 0.f);
    }

    // texture stage i per its mode; false kills the fragment
    bool Fetch(int i, float dp[4]) {
        const PixelKey& k = m.key;
        const Unit& u0 = m.unit[i];
        const TexInfo& info = m.info[i];
        uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
        uint32_t src = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xF : i == 3 ? (k.input_texture >> 20) & 0xF : 0;
        uint32_t map = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xF;
        const Vec& q = f.tc[i];
        auto dot = [&] { dp[i] = Dot3(q, DotInput(map, t[src])); };
        switch (mode) {
            case 0: t[i] = V(0, 0, 0, 1); break;
            case 1:
            case 2: {
                float w = q[3] == 0 ? 1 : q[3], u = q[0] / w, v = q[1] / w;
                Norm(i, u, v);
                if (info.shadow) t[i] = mode == 1 ? Shadow(i, u, v, 0, false) : Shadow(i, u, v, q[2] / w, true);
                else if (mode == 1 && info.kind == kTexCube) t[i] = u0.SampleDir(u, v, 1);
                else t[i] = u0.Sample(0, u, v);
                break;
            }
            case 3: t[i] = info.kind == kTexCube ? u0.SampleDir(q[0], q[1], q[2]) : u0.Sample(0, q[0], q[1]); break;
            case 4: t[i] = q; break;
            case 5:  // clip plane: a set compare_mode bit kills at >= 0, a clear one below 0
                for (int j = 0; j < 4; j++) {
                    bool ge = (k.compare_mode >> (i * 4 + j)) & 1;
                    if (ge ? q[j] >= 0 : q[j] < 0) return false;
                }
                t[i] = Vec{};
                break;
            case 6:
            case 7: {  // bump env map: signed du dv through the 2x2 matrix, 7 scales by luminance
                const auto& ts = m.s->tss[size_t(i)];
                Vec in = t[src];
                float du = SByte(in[2], 0, 256, 127), dv = SByte(in[1], 0, 256, 127);
                float u = q[0] + (AsFloat(ts[kTssBumpEnvMat00]) * du + AsFloat(ts[kTssBumpEnvMat10]) * dv);
                float v = q[1] + (AsFloat(ts[kTssBumpEnvMat01]) * du + AsFloat(ts[kTssBumpEnvMat11]) * dv);
                Norm(i, u, v);
                t[i] = u0.Sample(0, u, v);
                if (mode == 7) {
                    float lum = in[0] * AsFloat(ts[kTssBumpEnvLScale]) + AsFloat(ts[kTssBumpEnvLOffset]);
                    for (int j = 0; j < 4; j++) t[i][j] *= lum;
                }
                break;
            }
            case 9: {
                dot();
                float u = dp[i - 1], v = dp[i];
                Norm(i, u, v);
                t[i] = u0.Sample(0, u, v);
                break;
            }
            case 0xA:
            case 0x11: dot(); t[i] = Vec{}; break;
            case 0xB: {  // reflect diffuse at stage 2: stage 3's product early for the normal
                dot();
                float next = Dot3(f.tc[3], DotInput((k.dot_mapping >> 8) & 0xF, t[(k.input_texture >> 20) & 0xF]));
                t[i] = u0.SampleDir(dp[i - 1], dp[i], next);
                break;
            }
            case 0xC: {  // reflect specular at stage 3: eye from the three w
                dot();
                Vec n = V(dp[i - 2], dp[i - 1], dp[i], 0), eye = V(f.tc[i - 2][3], f.tc[i - 1][3], f.tc[i][3], 0);
                float nn = Dot3(n, n), ne = Dot3(n, eye);
                Vec r;
                for (int j = 0; j < 3; j++) r[j] = 2 * n[j] * ne / nn - eye[j];
                t[i] = u0.SampleDir(r[0], r[1], r[2]);
                break;
            }
            case 0xD: dot(); t[i] = u0.Sample(0, dp[i - 2], dp[i - 1]); break;
            case 0xE: dot(); t[i] = u0.SampleDir(dp[i - 2], dp[i - 1], dp[i]); break;
            case 0xF: t[i] = u0.Sample(0, t[src][3], t[src][0]); break;
            case 0x10: t[i] = u0.Sample(0, t[src][1], t[src][2]); break;
            default: t[i] = Vec{}; break;
        }
        return !(mode && mode != 4 && mode != 5 && (k.alphakill >> i & 1) && t[i][3] == 0);
    }

    Vec Var(uint32_t reg) const {
        uint32_t flags = m.key.count >> 8;
        switch (reg) {
            case 1: return m.pc[stage == 8 ? 16 : ((flags & 0x10) ? stage * 2 : 0)];
            case 2: return m.pc[stage == 8 ? 17 : ((flags & 0x100) ? stage * 2 + 1 : 1)];
            case 3: return f.fog;
            case 4: return v0;
            case 5: return v1;
            case 8: return t[0];
            case 9: return t[1];
            case 10: return t[2];
            case 11: return t[3];
            case 12: return r0;
            case 13: return r1;
            case 14: {  // v1 + r0, each maybe inverted, maybe clamped
                Vec r;
                for (int i = 0; i < 3; i++) {
                    float a = inv_v1 ? 1 - v1[i] : v1[i], b = inv_r0 ? 1 - r0[i] : r0[i];
                    r[i] = a + b;
                    if (clamp_sum) r[i] = Sat(r[i]);
                }
                return r;
            }
            case 15: return V(e[0] * fprod[0], e[1] * fprod[1], e[2] * fprod[2], 0);
            default: return Vec{};
        }
    }

    // an input byte: register, channel (alpha or the colour/blue), input mapping
    void Input(uint32_t v, bool alpha, float r[3]) const {
        Vec x = Var(v & 0xF);
        bool a = v & 0x10;
        for (int i = 0; i < 3; i++) {
            float c = a ? x[3] : alpha ? x[2] : x[i];
            // unsigned, unsigned invert, expand, expand negate, half bias, half bias negate, signed, signed negate
            switch (v & 0xE0) {
                case 0x00: c = std::max(c, 0.f); break;
                case 0x20: c = 1 - Sat(c); break;
                case 0x40: c = 2 * std::max(c, 0.f) - 1; break;
                case 0x60: c = -2 * std::max(c, 0.f) + 1; break;
                case 0x80: c = std::max(c, 0.f) - 0.5f; break;
                case 0xA0: c = -std::max(c, 0.f) + 0.5f; break;
                case 0xC0: break;
                default: c = -c; break;
            }
            r[i] = c;
        }
    }

    // bias, shift left 1, bias then shift left 1, shift left 2, shift right 1
    static float OutMap(float v, uint32_t map) {
        switch (map) {
            case 0x08: return v - 0.5f;
            case 0x10: return v * 2;
            case 0x18: return (v - 0.5f) * 2;
            case 0x20: return v * 4;
            case 0x30: return v / 2;
            default: return v;
        }
    }

    Vec* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return &v0;
            case 5: return &v1;
            case 8: return &t[0];
            case 9: return &t[1];
            case 10: return &t[2];
            case 11: return &t[3];
            case 12: return &r0;
            case 13: return &r1;
            default: return nullptr;
        }
    }

    struct Write {
        Vec* dst;
        int comp;
        float v;
    };
    // one half of a general combiner stage; rgb and alpha both read before either writes
    void Half(uint32_t inputs, uint32_t output, bool alpha, std::vector<Write>& w) {
        float A[3], B[3], C[3], D[3];
        Input(inputs >> 24, alpha, A);
        Input((inputs >> 16) & 0xFF, alpha, B);
        Input((inputs >> 8) & 0xFF, alpha, C);
        Input(inputs & 0xFF, alpha, D);
        uint32_t fl = output >> 12, map = fl & 0x38;
        float ab_dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2], cd_dot = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
        bool pick_cd = (m.key.count >> 8 & 1) ? r0[3] >= 0.5f : (uint32_t(r0[3] * 255) & 1) == 1;
        float ab[3], cd[3], ms[3];
        for (int i = 0; i < (alpha ? 1 : 3); i++) {
            float x = (fl & 2) && !alpha ? ab_dot : A[i] * B[i];
            float y = (fl & 1) && !alpha ? cd_dot : C[i] * D[i];
            float sum = (fl & 4) ? (pick_cd ? y : x) : x + y;
            ab[i] = std::clamp(OutMap(x, map), -1.f, 1.f);
            cd[i] = std::clamp(OutMap(y, map), -1.f, 1.f);
            ms[i] = std::clamp(OutMap(sum, map), -1.f, 1.f);
        }
        auto put = [&](uint32_t reg, const float* v, bool blue_to_alpha) {
            Vec* d = Dest(reg);
            if (!d) return;
            if (alpha) {
                w.push_back({d, 3, v[0]});
                return;
            }
            for (int i = 0; i < 3; i++) w.push_back({d, i, v[i]});
            if (blue_to_alpha) w.push_back({d, 3, v[2]});
        };
        put((output >> 4) & 0xF, ab, fl & 0x80);
        put(output & 0xF, cd, fl & 0x40);
        put((output >> 8) & 0xF, ms, false);
    }

    bool Shade(Vec& out) {
        float dp[4] = {};
        for (int i = 0; i < 4; i++)
            if (!Fetch(i, dp)) return false;
        const PixelKey& k = m.key;
        v0 = f.v0;
        v1 = f.v1;
        r0 = V(0, 0, 0, (k.tex_modes & 0x1F) ? t[0][3] : 1);
        r1 = Vec{};
        uint32_t n = std::min<uint32_t>(k.count & 0xFF, 8);
        std::vector<Write> w;
        for (uint32_t st = 0; st < n; st++) {
            stage = int(st);
            w.clear();
            Half(k.rgb_in[st], k.rgb_out[st], false, w);
            Half(k.a_in[st], k.a_out[st], true, w);
            for (const Write& x : w) (*x.dst)[x.comp] = x.v;
            if (g_trace.on)
                LOGI("  dbg st%u r0 %.3f %.3f %.3f %.3f r1 %.3f %.3f %.3f %.3f", st, r0[0], r0[1], r0[2], r0[3], r1[0], r1[1], r1[2], r1[3]);
        }
        if (g_trace.on) {
            LOGI("  dbg T0 %.3f %.3f %.3f %.3f T1 %.3f %.3f %.3f %.3f T2 %.3f %.3f %.3f %.3f T3 %.3f %.3f %.3f %.3f", t[0][0], t[0][1], t[0][2],
                 t[0][3], t[1][0], t[1][1], t[1][2], t[1][3], t[2][0], t[2][1], t[2][2], t[2][3], t[3][0], t[3][1], t[3][2], t[3][3]);
            LOGI("  dbg v0 %.3f %.3f %.3f %.3f v1 %.3f %.3f %.3f %.3f r0 %.3f %.3f %.3f %.3f r1 %.3f %.3f %.3f %.3f", v0[0], v0[1], v0[2],
                 v0[3], v1[0], v1[1], v1[2], v1[3], r0[0], r0[1], r0[2], r0[3], r1[0], r1[1], r1[2], r1[3]);
            for (int i = 0; i < 8; i++) {
                const Vec &a = m.pc[i * 2], &b = m.pc[i * 2 + 1];
                LOGI("  dbg c%d %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", i, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
            }
            const Vec* q = f.tc;
            LOGI("  dbg pT1 %.3f %.3f %.3f %.3f pT2 %.3f %.3f %.3f %.3f pT3 %.3f %.3f %.3f %.3f", q[1][0], q[1][1], q[1][2], q[1][3], q[2][0],
                 q[2][1], q[2][2], q[2][3], q[3][0], q[3][1], q[3][2], q[3][3]);
        }
        if (!k.final_abcd && !k.final_efg) {
            for (int i = 0; i < 3; i++) out[i] = f.fog[i] * (1 - f.fog[3]) + r0[i] * f.fog[3];
            out[3] = r0[3];
            return true;
        }
        // final combiner: D + lerp(C, B, A) in rgb, G in alpha
        stage = 8;
        uint32_t fl = k.final_efg & 0xFF;
        clamp_sum = fl & 0x80;
        inv_v1 = fl & 0x40;
        inv_r0 = fl & 0x20;
        float E[3], F[3], A[3], B[3], C[3], D[3], G[3];
        Input(k.final_efg >> 24, false, E);
        Input((k.final_efg >> 16) & 0xFF, false, F);
        e = V(E[0], E[1], E[2], 0);
        fprod = V(F[0], F[1], F[2], 0);
        Input(k.final_abcd >> 24, false, A);
        Input((k.final_abcd >> 16) & 0xFF, false, B);
        Input((k.final_abcd >> 8) & 0xFF, false, C);
        Input(k.final_abcd & 0xFF, false, D);
        Input((k.final_efg >> 8) & 0xFF, true, G);
        for (int i = 0; i < 3; i++) out[i] = D[i] + C[i] * (1 - A[i]) + B[i] * A[i];
        out[3] = G[0];
        return true;
    }
};

// D3D fixed function texture stages
bool ShadeStages(const Material& m, const Fragment& f, Vec& out) {
    Vec cur = f.v0, tmp;
    for (int i = 0; i < 4; i++) {
        const uint32_t* st = m.key.ff[i];  // colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
        if (st[0] <= 1) break;
        const Unit& u0 = m.unit[i];
        Vec tex;
        if (m.info[i].kind == kTexCube) {
            tex = u0.SampleDir(f.tc[i][0], f.tc[i][1], f.tc[i][2]);
        } else if (m.info[i].kind) {
            float u = f.tc[i][0], v = f.tc[i][1];
            if (m.info[i].rect && u0.img) {
                u /= u0.img->w;
                v /= u0.img->h;
            }
            tex = u0.Sample(0, u, v);
        } else {
            tex = V(1, 1, 1, 1);
        }
        if ((m.key.alphakill >> i & 1) && tex[3] == 0) return false;
        // D3DTA: source, then alpha replicate, then complement
        auto arg = [&](uint32_t a) {
            Vec x;
            switch (a & 0xF) {
                case 0: x = f.v0; break;
                case 2: x = tex; break;
                case 3: x = m.tfactor; break;
                case 4: x = f.v1; break;
                case 5: x = tmp; break;
                default: x = cur; break;
            }
            if (a & 0x20) x = Splat(x[3]);
            if (a & 0x10)
                for (int j = 0; j < 4; j++) x[j] = 1 - x[j];
            return x;
        };
        // D3DTOP on channel j
        auto op = [&](uint32_t o, const Vec& a1, const Vec& a2, const Vec& a0, int j, bool alpha) -> float {
            float x = a1[j], y = a2[j], z = a0[j];
            switch (o) {
                case 2: return x;
                case 3: return y;
                case 4: return x * y;
                case 5: return x * y * 2;
                case 6: return x * y * 4;
                case 7: return x + y;
                case 8: return x + y - 0.5f;
                case 9: return (x + y - 0.5f) * 2;
                case 10: return x - y;
                case 11: return x + y * (1 - x);
                case 12: return y + (x - y) * f.v0[3];
                case 13: return y + (x - y) * cur[3];
                case 14: return y + (x - y) * tex[3];
                case 15: return y + (x - y) * m.tfactor[3];
                case 16: return x + y * (1 - tex[3]);
                case 18: return alpha ? x : x + y * a1[3];
                case 19: return alpha ? x : x * y + a1[3];
                case 20: return alpha ? x : x + y * (1 - a1[3]);
                case 21: return alpha ? x : (1 - x) * y + a1[3];
                case 22: {
                    float d = 0;
                    for (int q = 0; q < 3; q++) d += (a1[q] - 0.5f) * 2 * (a2[q] - 0.5f) * 2;
                    return Sat(d);
                }
                case 23: return z + x * y;
                case 24: return y + (x - y) * z;
                default: return cur[alpha ? 3 : j];
            }
        };
        Vec r;
        Vec c1 = arg(st[1]), c2 = arg(st[2]), c0 = arg(st[3]);
        for (int j = 0; j < 3; j++) r[j] = Sat(op(st[0], c1, c2, c0, j, false));
        if (st[4] <= 1) {
            r[3] = cur[3];
        } else {
            Vec a1 = arg(st[5]), a2 = arg(st[6]), a0 = arg(st[7]);
            r[3] = Sat(op(st[4], a1, a2, a0, 3, true));
        }
        (st[8] == 5 ? tmp : cur) = r;
    }
    for (int j = 0; j < 3; j++) cur[j] = Sat(cur[j] + f.v1[j]);
    for (int j = 0; j < 3; j++) out[j] = f.fog[j] * (1 - f.fog[3]) + cur[j] * f.fog[3];
    out[3] = cur[3];
    return true;
}

// output merger, GL enums as the pushbuffer holds them

bool Passes(uint32_t func, uint32_t a, uint32_t b) {
    switch (func) {
        case 0x200: return false;
        case 0x201: return a < b;
        case 0x202: return a == b;
        case 0x203: return a <= b;
        case 0x204: return a > b;
        case 0x205: return a != b;
        case 0x206: return a >= b;
        default: return true;
    }
}
// GL ops: zero, replace, saturating incr and decr, invert, wrapping incr and decr, else keep
uint32_t StencilResult(uint32_t op, uint32_t v, uint32_t ref) {
    switch (op) {
        case 0: return 0;
        case 0x1E01: return ref;
        case 0x1E02: return std::min(v + 1, 255u);
        case 0x1E03: return v ? v - 1 : 0;
        case 0x150A: return ~v & 255;
        case 0x8507: return (v + 1) & 255;
        case 0x8508: return (v - 1) & 255;
        default: return v;
    }
}
// GL blend factors, 0x8001 to 0x8004 take the constant colour
float BlendFactor(uint32_t f, const Vec& src, const Vec& dst, const Vec& k, int ch) {
    switch (f) {
        case 0: return 0;
        case 1: return 1;
        case 0x300: return src[ch];
        case 0x301: return 1 - src[ch];
        case 0x302: return src[3];
        case 0x303: return 1 - src[3];
        case 0x304: return dst[3];
        case 0x305: return 1 - dst[3];
        case 0x306: return dst[ch];
        case 0x307: return 1 - dst[ch];
        case 0x308: return ch == 3 ? 1 : std::min(src[3], 1 - dst[3]);
        case 0x8001: return k[ch];
        case 0x8002: return 1 - k[ch];
        case 0x8003: return k[3];
        case 0x8004: return 1 - k[3];
        default: return 1;
    }
}

// probes

struct Pixel {
    int x, y;
};
struct Probes {
    std::vector<Pixel> at;
    Pixel trace{-1, -1};
    uint32_t width = 640;
    Probes() {
        auto parse = [](const char* s, std::vector<Pixel>& out) {
            int x, y, n;
            while (s && std::sscanf(s, " %d,%d%n", &x, &y, &n) == 2) {
                out.push_back({x, y});
                s += n;
            }
        };
        parse(std::getenv("PROBES"), at);
        std::vector<Pixel> t;
        parse(std::getenv("TRACE"), t);
        if (!t.empty()) trace = t[0];
        if (const char* w = std::getenv("PROBE_WIDTH")) width = uint32_t(std::atoi(w));
    }
    bool Watched(const Plane& rt) const { return rt.ok && rt.w == width && !rt.swizzled; }
};
const Probes& Watch() {
    static const Probes p;
    return p;
}

uint32_t g_draws = 0;  // this frame's

// triangles

struct Shaded {
    Vec pos;  // screen x y, depth units z, w
    Vec attr[kAttrs];
};

// clip space vertex, attributes not yet divided
struct Clip {
    float h[4];
    Vec attr[kAttrs];
};

// screen vertex with attributes over w
struct Screen {
    float x, y, z, iw;
    Vec attr[kAttrs];
};

}  // namespace

void BeginFrame() {
    g_images.clear();
    g_draws = 0;
}

void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    Plane rt = OpenPlane(s.rt), zs = OpenPlane(s.zs);
    // the given rects, else the viewport
    auto each = [&](const Plane& p, auto&& fn) {
        std::vector<std::array<int, 4>> area;
        if (count && rects) {
            for (uint32_t i = 0; i < count; i++) {
                int32_t r[4];
                std::memcpy(r, rects + i * 16, 16);
                area.push_back({r[0], r[1], r[2], r[3]});
            }
        } else {
            int x = int(s.viewport.x), y = int(s.viewport.y);
            area.push_back({x, y, x + int(s.viewport.w), y + int(s.viewport.h)});
        }
        for (auto& r : area)
            for (int y = std::max(r[1], 0); y < std::min<int>(r[3], int(p.h)); y++)
                for (int x = std::max(r[0], 0); x < std::min<int>(r[2], int(p.w)); x++) fn(uint32_t(x), uint32_t(y));
    };
    // D3DCLEAR: 1 z, 2 stencil, 0x10 to 0x80 red, green, blue, alpha
    if ((flags & 0xF0) && rt.ok) {
        uint32_t mask = (flags & 0x10 ? 4u : 0u) | (flags & 0x20 ? 2u : 0u) | (flags & 0x40 ? 1u : 0u) | (flags & 0x80 ? 8u : 0u);
        Vec c = Unpack(color);
        each(rt, [&](uint32_t x, uint32_t y) { rt.Store(x, y, c, mask); });
        Written(rt.data, rt.End());
    }
    if ((flags & 3) && zs.ok) {
        float top = zs.Depth16() ? 65535.f : 16777215.f;
        each(zs, [&](uint32_t x, uint32_t y) {
            uint32_t oz, os;
            zs.LoadDepth(x, y, oz, os);
            zs.StoreDepth(x, y, (flags & 1) ? uint32_t(z * top) : oz, (flags & 2) ? stencil : os);
        });
        Written(zs.data, zs.End());
    }
}

void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) {
    Plane a = OpenPlane(src), b = OpenPlane(dst);
    if (!a.ok || !b.ok) return;
    uint32_t n = count && rects ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = 0, y0 = 0, x1 = int(a.w), y1 = int(a.h), dx = 0, dy = 0;
        if (count && rects) {
            x0 = int(kernel::R32(rects + i * 16));
            y0 = int(kernel::R32(rects + i * 16 + 4));
            x1 = int(kernel::R32(rects + i * 16 + 8));
            y1 = int(kernel::R32(rects + i * 16 + 12));
            dx = x0;
            dy = y0;
        }
        if (points) {
            dx = int(kernel::R32(points + i * 8));
            dy = int(kernel::R32(points + i * 8 + 4));
        }
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int tx = dx + x - x0, ty = dy + y - y0;
                if (x < 0 || y < 0 || x >= int(a.w) || y >= int(a.h) || tx < 0 || ty < 0 || tx >= int(b.w) || ty >= int(b.h)) continue;
                if (a.bytes == b.bytes) std::memcpy(b.At(uint32_t(tx), uint32_t(ty)), a.At(uint32_t(x), uint32_t(y)), a.bytes);
                else b.Store(uint32_t(tx), uint32_t(ty), a.Load(uint32_t(x), uint32_t(y)), 15);
            }
    }
    Written(b.data, b.End());
}

void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    uint32_t draw = g_draws++;
    Assembled as;
    if (!Assemble(s, prim, start, count, indices, as) || as.topology != Topology::Triangles) return;
    Plane rt = OpenPlane(s.rt);
    Plane zs = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? OpenPlane(s.zs) : Plane{};
    if (!rt.ok && !zs.ok) return;
    if (rt.ok && zs.ok && (rt.w != zs.w || rt.h != zs.h)) zs.ok = false;
    auto simple = [&](uint32_t method) { return s.simple[method >> 2]; };

    Material mat;
    mat.s = &s;
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        TexInfo& ti = mat.info[i];
        ti.kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        ti.rect = d.linear;
        if (Format(d.format).depth) ti.shadow = d.format == 0x2C || d.format == 0x30 ? 2 : 1;
        Unit& u = mat.unit[i];
        const auto& ts = s.tss[size_t(i)];
        u.img = FindImage(s.textures[size_t(i)]);
        u.address_u = ts[kTssAddressU];
        u.address_v = ts[kTssAddressV];
        u.mag = ti.shadow ? 1 : ts[kTssMagFilter];
        u.min = ti.shadow ? 1 : ts[kTssMinFilter];
        u.border = Unpack(ts[kTssBorderColor]);
    }
    mat.key = MakePixelKey(s, mat.info);
    for (int i = 0; i < 8; i++) {
        mat.pc[i * 2] = Unpack(simple(kMPsConstant0 + i * 4));
        mat.pc[i * 2 + 1] = Unpack(simple(kMPsConstant1 + i * 4));
    }
    mat.pc[16] = Unpack(simple(kMPsFinalConstant0));
    mat.pc[17] = Unpack(simple(kMPsFinalConstant1));
    mat.tfactor = Unpack(s.rs[kRsTextureFactor]);

    // vertices
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    int regs = 0, slot[16];
    for (int r = 0; r < 16; r++) slot[r] = (as.regs >> r) & 1 ? regs++ : -1;
    auto input = [&](uint32_t v, int r) { return &as.vertices[(size_t(v) * regs + slot[r]) * 4]; };
    uint32_t gw = rt.ok ? rt.w : zs.w, gh = rt.ok ? rt.h : zs.h;
    std::vector<Shaded> verts(as.vertex_count);
    for (uint32_t vi = 0; vi < as.vertex_count; vi++) {
        VertexProgram vp{s};
        for (int r = 0; r < 16; r++) {
            if (slot[r] >= 0) {
                const float* p = input(vi, r);
                vp.in[r] = V(p[0], p[1], p[2], p[3]);
            } else {
                vp.in[r] = r == 3 ? V(1, 1, 1, 1) : V(0, 0, 0, 1);
            }
        }
        Shaded& o = verts[vi];
        if (program) {
            vp.Run(s.vs->program);
            o.pos = vp.out[0];
            o.pos[3] = MagClamp(o.pos[3]);
            o.attr[kD0] = vp.out[3];
            o.attr[kD1] = vp.out[4];
            o.attr[kB0] = vp.out[7];
            o.attr[kB1] = vp.out[8];
            o.attr[kFog] = Splat(FogAmount(s, vp.out[5][0]));
            for (int t = 0; t < 4; t++) o.attr[kT0 + t] = vp.out[9 + t];
        } else {
            const Vec& q = vp.in[0];
            if (as.pretransformed) {
                o.pos = V(q[0], q[1], q[2] * s.zscale, q[3] == 0 ? 1 : 1 / q[3]);
            } else {
                // world, view, projection, then the viewport, as the D3D12 path builds it
                auto mul = [](const float* x, const float* y, float* r) {
                    for (int i = 0; i < 4; i++)
                        for (int j = 0; j < 4; j++)
                            r[i * 4 + j] = x[i * 4] * y[j] + x[i * 4 + 1] * y[4 + j] + x[i * 4 + 2] * y[8 + j] + x[i * 4 + 3] * y[12 + j];
                };
                float wv[16], wvp[16], cp[4];
                mul(s.transforms[256].data(), s.transforms[2].data(), wv);
                mul(wv, s.transforms[3].data(), wvp);
                for (int j = 0; j < 4; j++) cp[j] = q[0] * wvp[j] + q[1] * wvp[4 + j] + q[2] * wvp[8 + j] + wvp[12 + j];
                float w = MagClamp(cp[3]);
                const Viewport& v = s.viewport;
                o.pos = V(v.x + (cp[0] / w + 1) * 0.5f * v.w, v.y + (1 - cp[1] / w) * 0.5f * v.h,
                          (v.min_z + cp[2] / w * (v.max_z - v.min_z)) * s.zscale, w);
            }
            o.attr[kD0] = vp.in[3];
            o.attr[kD1] = slot[4] >= 0 ? vp.in[4] : Vec{};
            o.attr[kB0] = o.attr[kD0];
            o.attr[kB1] = o.attr[kD1];
            o.attr[kFog] = Splat(FogAmount(s, std::fabs(o.pos[2] / s.zscale)));
            for (int t = 0; t < 4; t++) o.attr[kT0 + t] = slot[9 + t] >= 0 ? vp.in[9 + t] : V(0, 0, 0, 1);
        }
        for (int a : {kD0, kD1, kB0, kB1})
            for (int j = 0; j < 4; j++) o.attr[a][j] = Sat(o.attr[a][j]);
        if (!s.rs[kRsSpecularEnable]) o.attr[kD1] = o.attr[kB1] = V(0, 0, 0, 1);
    }

    // raster and output state
    bool blend = simple(kMBlendEnable) != 0;
    uint32_t src_factor = simple(kMSrcBlend), dst_factor = simple(kMDestBlend), blend_op = simple(kMBlendOp);
    uint32_t cw = simple(kMColorWriteEnable);
    uint32_t cmask = (cw >> 16 & 1 ? 4u : 0u) | (cw >> 8 & 1 ? 2u : 0u) | (cw & 1 ? 1u : 0u) | (cw >> 24 & 1 ? 8u : 0u);
    bool ztest = zs.ok && s.rs[kRsZEnable], zwrite = ztest && simple(kMZWriteEnable);
    uint32_t zfunc = simple(kMZFunc);
    bool stencil = zs.ok && s.rs[kRsStencilEnable];
    uint32_t sfunc = simple(kMStencilFunc), sref = simple(kMStencilRef) & 0xFF, smask = simple(kMStencilMask) & 0xFF,
             swmask = simple(kMStencilWriteMask) & 0xFF;
    uint32_t op_fail = s.rs[kRsStencilFail], op_zfail = simple(kMStencilZFail), op_pass = simple(kMStencilPass);
    bool atest = simple(kMAlphaTestEnable) != 0;
    uint32_t afunc = simple(kMAlphaFunc), aref = simple(kMAlphaRef) & 0xFF;
    uint32_t cull = s.rs[kRsCullMode];
    bool flat = simple(kMShadeMode) == 0x1D00, two_sided = s.rs[kRsTwoSidedLighting] != 0;  // 0x1D00 GL_FLAT
    Vec blend_color = Unpack(simple(kMBlendColor));
    int left = int(s.viewport.x), top = int(s.viewport.y), right = left + int(s.viewport.w), bottom = top + int(s.viewport.h);
    if (s.scissor_count) {
        left = std::max(left, int(s.scissors[0]));
        top = std::max(top, int(s.scissors[1]));
        right = std::min(right, int(s.scissors[2]));
        bottom = std::min(bottom, int(s.scissors[3]));
    }
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, int(gw));
    bottom = std::min(bottom, int(gh));
    const Probes& probes = Watch();
    bool watched = probes.Watched(rt);

    const float near_w = 1e-6f;
    for (size_t ti = 0; ti + 2 < as.indices.size(); ti += 3) {
        Clip tri[3];
        for (int i = 0; i < 3; i++) {
            const Shaded& v = verts[as.indices[ti + i]];
            for (int j = 0; j < 3; j++) tri[i].h[j] = v.pos[j] * v.pos[3];
            tri[i].h[3] = v.pos[3];
            std::copy(v.attr, v.attr + kAttrs, tri[i].attr);
        }
        // flat shading takes the first vertex's colours
        const Vec* provoking = tri[0].attr;
        Vec flat_colour[4] = {provoking[kD0], provoking[kD1], provoking[kB0], provoking[kB1]};
        // clipped against w > near_w: a triangle leaves at most four corners
        Clip poly[4];
        int corners = 0;
        for (int i = 0; i < 3; i++) {
            const Clip &p = tri[i], &q = tri[(i + 1) % 3];
            bool p_in = p.h[3] > near_w, q_in = q.h[3] > near_w;
            if (p_in) poly[corners++] = p;
            if (p_in == q_in) continue;
            float t = (near_w - p.h[3]) / (q.h[3] - p.h[3]);
            Clip& m = poly[corners++];
            for (int j = 0; j < 4; j++) m.h[j] = p.h[j] + (q.h[j] - p.h[j]) * t;
            for (int a = 0; a < kAttrs; a++)
                for (int j = 0; j < 4; j++) m.attr[a][j] = p.attr[a][j] + (q.attr[a][j] - p.attr[a][j]) * t;
        }
        if (corners < 3) continue;
        Screen sv[4];
        for (int i = 0; i < corners; i++) {
            float iw = 1 / poly[i].h[3];
            sv[i].x = poly[i].h[0] * iw;
            sv[i].y = poly[i].h[1] * iw;
            sv[i].z = poly[i].h[2] * iw;
            sv[i].iw = iw;
            for (int a = 0; a < kAttrs; a++)
                for (int j = 0; j < 4; j++) sv[i].attr[a][j] = poly[i].attr[a][j] * iw;
        }
        for (int fan = 1; fan + 1 < corners; fan++) {
            const Screen &p0 = sv[0], &p1 = sv[fan], &p2 = sv[fan + 1];
            float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
            if (area == 0 || !std::isfinite(area)) continue;
            bool clockwise = area > 0;  // y down
            // GL_CW 0x900, GL_CCW 0x901
            if ((cull == 0x900 && clockwise) || (cull == 0x901 && !clockwise)) continue;
            bool front = s.rs[kRsFrontFace] == 0x901 ? !clockwise : clockwise;
            int x0 = std::max(left, int(std::floor(std::min({p0.x, p1.x, p2.x})))), x1 = std::min(right - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}))));
            int y0 = std::max(top, int(std::floor(std::min({p0.y, p1.y, p2.y})))), y1 = std::min(bottom - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}))));
            if (x0 > x1 || y0 > y1) continue;
            float inv_area = 1 / area;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    // barycentrics at the pixel centre
                    float cx = x + 0.5f, cy = y + 0.5f;
                    float b0 = ((p1.x - cx) * (p2.y - cy) - (p2.x - cx) * (p1.y - cy)) * inv_area;
                    float b1 = ((p2.x - cx) * (p0.y - cy) - (p0.x - cx) * (p2.y - cy)) * inv_area;
                    float b2 = 1 - b0 - b1;
                    if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                    float z = b0 * p0.z + b1 * p1.z + b2 * p2.z;
                    if (z < 0 || z > s.zscale) continue;
                    float iw = b0 * p0.iw + b1 * p1.iw + b2 * p2.iw;
                    Vec attr[kAttrs];
                    for (int a = 0; a < kAttrs; a++)
                        for (int j = 0; j < 4; j++) attr[a][j] = (b0 * p0.attr[a][j] + b1 * p1.attr[a][j] + b2 * p2.attr[a][j]) / iw;
                    if (flat) std::copy(flat_colour, flat_colour + 4, attr);
                    Fragment frag;
                    bool back = two_sided && !front;
                    frag.v0 = attr[back ? kB0 : kD0];
                    frag.v1 = attr[back ? kB1 : kD1];
                    frag.fog = Unpack(s.rs[kRsFogColor]);
                    frag.fog[3] = Sat(attr[kFog][0]);
                    for (int t = 0; t < 4; t++) frag.tc[t] = attr[kT0 + t];
                    g_trace.on = watched && x == probes.trace.x && y == probes.trace.y;
                    Vec col;
                    bool keep = mat.key.programmable ? Combiners{mat, frag}.Shade(col) : ShadeStages(mat, frag, col);
                    if (!keep) continue;
                    if (atest && !Passes(afunc, uint32_t(int(std::lround(Sat(col[3]) * 255))), aref)) continue;
                    uint32_t oz = 0, os = 0, zi = uint32_t(std::floor(z));
                    if (zs.ok) zs.LoadDepth(uint32_t(x), uint32_t(y), oz, os);
                    auto stencil_op = [&](uint32_t op) { return (os & ~swmask) | (StencilResult(op, os, sref) & swmask); };
                    if (stencil && !Passes(sfunc, sref & smask, os & smask)) {
                        zs.StoreDepth(uint32_t(x), uint32_t(y), oz, stencil_op(op_fail));
                        continue;
                    }
                    if (ztest && !Passes(zfunc, zi, oz)) {
                        if (stencil) zs.StoreDepth(uint32_t(x), uint32_t(y), oz, stencil_op(op_zfail));
                        continue;
                    }
                    if (zs.ok && (zwrite || stencil)) zs.StoreDepth(uint32_t(x), uint32_t(y), zwrite ? zi : oz, stencil ? stencil_op(op_pass) : os);
                    if (!rt.ok || !cmask) continue;
                    for (int j = 0; j < 4; j++) col[j] = Sat(col[j]);
                    Vec shaded = col;
                    if (blend) {
                        Vec d = rt.Load(uint32_t(x), uint32_t(y)), r;
                        for (int j = 0; j < 4; j++) {
                            float a = col[j] * BlendFactor(src_factor, col, d, blend_color, j);
                            float b = d[j] * BlendFactor(dst_factor, col, d, blend_color, j);
                            switch (blend_op) {  // GL subtract, reverse subtract, min, max, else add
                                case 0x800A: r[j] = a - b; break;
                                case 0x800B: r[j] = b - a; break;
                                case 0x8007: r[j] = std::min(col[j], d[j]); break;
                                case 0x8008: r[j] = std::max(col[j], d[j]); break;
                                default: r[j] = a + b; break;
                            }
                        }
                        col = r;
                    }
                    if (watched)
                        for (size_t pi = 0; pi < probes.at.size(); pi++) {
                            if (probes.at[pi].x != x || probes.at[pi].y != y) continue;
                            for (int c = 0; pi == 0 && c < 3; c++) {
                                uint32_t vi = as.indices[ti + c];
                                const Vec& pos = verts[vi].pos;
                                const float* v0 = slot[0] >= 0 ? input(vi, 0) : nullptr;
                                LOGI("  v%u pos %.2f %.2f %.1f w %.3f in %.3f %.3f %.3f %.3f", vi, pos[0], pos[1], pos[2], pos[3], v0 ? v0[0] : 0,
                                     v0 ? v0[1] : 0, v0 ? v0[2] : 0, v0 ? v0[3] : 0);
                            }
                            LOGI("probe %d,%d draw %u src %.3f %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f blend %u %X %X mask %X", x, y, draw, shaded[0],
                                 shaded[1], shaded[2], shaded[3], col[0], col[1], col[2], col[3], unsigned(blend), src_factor, dst_factor, cmask);
                        }
                    rt.Store(uint32_t(x), uint32_t(y), col, cmask);
                }
        }
    }
    if (rt.ok && cmask) Written(rt.data, rt.End());
    if (zs.ok) Written(zs.data, zs.End());
}

bool WriteTga(const Surface& s, const char* path) {
    Plane p = OpenPlane(s);
    if (!p.ok) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint8_t head[18] = {0, 0, 2};  // uncompressed true colour
    head[12] = uint8_t(p.w), head[13] = uint8_t(p.w >> 8), head[14] = uint8_t(p.h), head[15] = uint8_t(p.h >> 8);
    head[16] = 24;
    head[17] = 0x20;  // top row first
    std::fwrite(head, 1, 18, f);
    std::vector<uint8_t> row(size_t(p.w) * 3);
    for (uint32_t y = 0; y < p.h; y++) {
        for (uint32_t x = 0; x < p.w; x++) {
            Vec c = p.Load(x, y);
            for (int i = 0; i < 3; i++) row[x * 3 + i] = uint8_t(Sat(c[2 - i]) * 255);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace reference
