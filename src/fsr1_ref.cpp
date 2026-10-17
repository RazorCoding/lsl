// CPU reference for the FSR1 (FidelityFX Super Resolution 1) EASU upscaler.
//
// The filter itself is NOT re-implemented here. third_party/ffx/ffx_fsr1.h is
// AMD's original source (Copyright (c) 2021 Advanced Micro Devices, Inc.; MIT,
// see third_party/ffx/license.txt) and is compiled verbatim by both
// shaders/scale.comp and this file. All this translation unit supplies is the
// small GLSL-flavoured vocabulary that header expects - the AF/AU vector
// types, the A* scalar helpers, and the textureGather callbacks - so the CPU
// and GPU paths cannot drift apart by construction.
//
// The arithmetic matches the shader deliberately, bit tricks included. The GPU's
// APrxLoRcpF1/APrxLoRsqF1 are integer manipulations of the float's bits rather
// than 1/x, and EASU feeds their results straight into its tap weights, so
// substituting exact IEEE reciprocals here (which looked harmless) cost 28 dB of
// CPU-vs-GPU agreement at 2.5x. See "Bugs found and fixed during bring-up" in
// README.md.

#include "reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// GLSL-flavoured vocabulary for the FSR1 header. Kept in the global namespace
// because ffx_fsr1.h is included at global scope and uses unqualified names.
// ---------------------------------------------------------------------------

typedef float    AF1;
typedef uint32_t AU1;
typedef bool     AP1;

// GLSL vectors expose named components and swizzles at once. The header reads
// con.xy/con.zw (EASU) and con.x (RCAS), so AU4 has to offer both views of the
// same storage; the anonymous struct is the same documented GCC/Clang extension
// that AF2/AF3/AF4 below already rely on. AU2 must stay a plain aggregate:
// GCC refuses class-typed members with constructors inside an anonymous
// aggregate, and the header never constructs a uvec2 from components itself.
struct AU2 { uint32_t x, y; };
struct AU4 {
    union {
        struct { uint32_t x, y, z, w; };
        struct { AU2 xy, zw; };
    };
    AU4() = default;
    uint32_t& operator[](int i) {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    uint32_t operator[](int i) const {
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
};

// GLSL's ivec2. RCAS walks a 3x3 neighbourhood with signed offsets, so its
// coordinates have to be able to go negative at the image border.
struct ASU2 {
    int32_t x = 0, y = 0;
    ASU2() = default;
    ASU2(int32_t a, int32_t b) : x(a), y(b) {}
    explicit ASU2(AU2 u) : x(int32_t(u.x)), y(int32_t(u.y)) {}
};
inline ASU2 operator+(ASU2 a, ASU2 b) { return ASU2{a.x + b.x, a.y + b.y}; }

// The AU2 constructor mirrors GLSL's numeric vec2(uvec2) conversion, which is
// what the header relies on (AF2(ip) with ip a uvec2). The anonymous union adds
// GLSL's colour aliases (.rgb/.rgba); it is a documented GCC/Clang extension,
// and it is how the two member namings stay the same storage.
struct AF2 {
    union { struct { float x, y; }; struct { float r, g; }; };
    AF2() = default;
    AF2(float a, float b) : x(a), y(b) {}
    explicit AF2(AU2 u) : x(float(u.x)), y(float(u.y)) {}
};
struct AF3 {
    union { struct { float x, y, z; }; struct { float r, g, b; }; };
    AF3() = default;
    AF3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct AF4 {
    union {
        struct { float x, y, z, w; };
        struct { float r, g, b, a; };
        // RCAS pulls the colour straight out of a load: FsrRcasLoadF(...).rgb.
        // A named member rather than one buried in an anonymous struct, because
        // GCC rejects constructor-bearing types inside anonymous aggregates.
        AF3 rgb;
    };
    AF4() = default;
    AF4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

static inline float    bitsToFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
static inline uint32_t floatToBits(float f)    { uint32_t u; std::memcpy(&u, &f, 4); return u; }

#define AF1_(a)     ((AF1)(a))
#define AU1_(a)     ((AU1)(a))
#define AF2_(a)     AF2{float(a), float(a)}
#define AF3_(a)     AF3{float(a), float(a), float(a)}
#define AF4_(a)     AF4{float(a), float(a), float(a), float(a)}
// NB: the parameter must not be named after a vector field, or the preprocessor
// rewrites the field access too - AF2_AU2(v) with (v).x would become (a).a.
#define AU1_AF1(v)  (floatToBits(AF1(v)))
#define AF1_AU1(v)  (bitsToFloat(AU1(v)))
#define AF2_AU2(v)  (AF2{bitsToFloat((v).x), bitsToFloat((v).y)})
#define inout       &
#define outAU4      AU4&
#define A_STATIC    static

// Scalar helpers. Names and semantics follow ffx_a.h. The using-declarations
// matter: without them the unqualified abs()/min()/max() in the header bind to
// the integer overloads from <cstdlib> instead of the float ones.
using std::abs;
using std::floor;
using std::max;
using std::min;
using std::sqrt;

inline AF1 ASatF1(AF1 x)          { return std::min(std::max(x, 0.0f), 1.0f); }
inline AF1 ARcpF1(AF1 x)          { return 1.0f / x; }

// The three "approximate" helpers are not approximations of convenience: the
// shader gets them from the GPU COMMON section of ffx_a.h, where they are
// integer bit manipulations on the float's exponent/mantissa, and EASU feeds
// their results straight into its tap weights and edge-length terms. Using an
// exact reciprocal here instead cost ~28 dB of CPU-vs-GPU agreement at 2.5x,
// because a ~1e-3 relative error in the weights is visible in the output. They
// are therefore reproduced bit-for-bit from that section, which also makes the
// reference deterministic across drivers for free.
inline AF1 APrxLoRcpF1(AF1 a)  { return AF1_AU1(0x7ef07bbbu - AU1_AF1(a)); }
inline AF1 APrxMedRcpF1(AF1 a) {
    const AF1 b = AF1_AU1(0x7ef19fffu - AU1_AF1(a));
    return b * (-b * a + AF1_(2.0));
}
inline AF1 APrxLoRsqF1(AF1 a)  { return AF1_AU1(0x5f347d74u - (AU1_AF1(a) >> AU1_(1))); }
inline AF1 AMin3F1(AF1 a, AF1 b, AF1 c) { return std::min(a, std::min(b, c)); }
inline AF1 AMax3F1(AF1 a, AF1 b, AF1 c) { return std::max(a, std::max(b, c)); }
inline AF1 AExp2F1(AF1 x) { return std::exp2(x); }

// Component-wise forms used only by the TEPD/RCAS extras. AGtZeroF3 is written
// as an explicit sign test; ffx_a.h saturates x*inf, which is the same answer
// for every finite input and well defined at zero.
inline AF3 ASatF3(AF3 x) { return AF3{ASatF1(x.x), ASatF1(x.y), ASatF1(x.z)}; }
inline AF3 APrxMedRcpF3(AF3 x) {
    return AF3{APrxMedRcpF1(x.x), APrxMedRcpF1(x.y), APrxMedRcpF1(x.z)};
}
inline AF3 AGtZeroF3(AF3 x) {
    return AF3{x.x > 0.0f ? 1.0f : 0.0f,
               x.y > 0.0f ? 1.0f : 0.0f,
               x.z > 0.0f ? 1.0f : 0.0f};
}

// The remaining pieces of the A-language are only reached by the FSR1 extras
// the reference does not call (RCAS denoise, the SRTM tone mapper, TEPD
// dither). They still have to parse, so they are provided faithfully rather
// than stubbed.
#define varAF2(name) AF1 name[2]
#define initAF2(a, b) {float(a), float(b)}
// Falls out of FsrRcasCon's setup of con[1]. Only the 16-bit RCAS path reads
// that, so the value is inert here, but the component order matches GLSL's
// packHalf2x16 (v.x low, v.y high) rather than being left transposed.
inline AU1 AU1_AH2_AF2(const AF1* v) {
    auto half = [](float f) -> uint32_t {
        const uint32_t u = floatToBits(f);
        const uint32_t sign = (u >> 16) & 0x8000u;
        const int32_t  exp  = int32_t((u >> 23) & 0xffu) - 127 + 15;
        if (exp <= 0)  return sign;                          // flush subnormal
        if (exp >= 31) return sign | 0x7c00u;                // saturate
        return sign | (uint32_t(exp) << 10) | ((u >> 13) & 0x3ffu);
    };
    return half(v[0]) | (half(v[1]) << 16);
}

// Vector helpers that GLSL provides as built-ins.
inline AF2 floor(AF2 a) { return AF2{std::floor(a.x), std::floor(a.y)}; }
inline AF3 min(AF3 a, AF3 b) { return AF3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline AF3 max(AF3 a, AF3 b) { return AF3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline AF3 AMin3F3(AF3 a, AF3 b, AF3 c) { return min(min(a, b), c); }
inline AF3 AMax3F3(AF3 a, AF3 b, AF3 c) { return max(max(a, b), c); }
inline AF2 sqrt(AF2 a) { return AF2{std::sqrt(a.x), std::sqrt(a.y)}; }
inline AF3 sqrt(AF3 a) { return AF3{std::sqrt(a.x), std::sqrt(a.y), std::sqrt(a.z)}; }
inline AF3 floor(AF3 a) { return AF3{std::floor(a.x), std::floor(a.y), std::floor(a.z)}; }
inline AF1 AFractF1(AF1 x) { return x - std::floor(x); }

// Component-wise operators.
inline AF2 operator+(AF2 a, AF2 b) { return AF2{a.x + b.x, a.y + b.y}; }
inline AF2 operator-(AF2 a, AF2 b) { return AF2{a.x - b.x, a.y - b.y}; }
inline AF2 operator*(AF2 a, AF2 b) { return AF2{a.x * b.x, a.y * b.y}; }
inline AF2 operator*(AF2 a, float s) { return AF2{a.x * s, a.y * s}; }
inline AF2 operator*(float s, AF2 a) { return AF2{a.x * s, a.y * s}; }
inline AF2& operator+=(AF2& a, AF2 b) { a.x += b.x; a.y += b.y; return a; }
inline AF2& operator-=(AF2& a, AF2 b) { a.x -= b.x; a.y -= b.y; return a; }
inline AF2& operator*=(AF2& a, AF2 b) { a.x *= b.x; a.y *= b.y; return a; }

inline AF3 operator+(AF3 a, AF3 b) { return AF3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline AF3 operator-(AF3 a, AF3 b) { return AF3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline AF3 operator*(AF3 a, AF3 b) { return AF3{a.x * b.x, a.y * b.y, a.z * b.z}; }
inline AF3 operator*(AF3 a, float s) { return AF3{a.x * s, a.y * s, a.z * s}; }
inline AF3 operator*(float s, AF3 a) { return AF3{a.x * s, a.y * s, a.z * s}; }
inline AF3& operator+=(AF3& a, AF3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline AF3& operator*=(AF3& a, AF3 b) { a.x *= b.x; a.y *= b.y; a.z *= b.z; return a; }

inline AF4 operator+(AF4 a, AF4 b) { return AF4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline AF4 operator*(AF4 a, AF4 b) { return AF4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }

// 8-bit output quantisation, mirroring pack() in shaders/scale.comp and
// shaders/rcas.comp: clamp to [0,1], scale by 255, round half away from zero.
inline uint8_t quant255(float v) {
    const float r = std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f);
    return uint8_t(r <= 0.0f ? 0 : (r >= 255.0f ? 255 : r));
}

// ---------------------------------------------------------------------------
// textureGather emulation.
//
// EASU reads its twelve taps through textureGather. Phase 1 keeps pixels in a
// plain RGBA8 array rather than a sampled image, so the same 2x2 gather is
// emulated over that array. Vulkan defines the component order as
//
//     .x = (i0, j0+1)   .y = (i0+1, j0+1)   .z = (i0+1, j0)   .w = (i0, j0)
//
// with i0 = floor(u*W - 0.5), j0 = floor(v*H - 0.5), and clamping at the
// edges (the shader samples CLAMP_TO_EDGE). A wrong order still yields a
// plausible image, so tests/test_reference.cpp pins it down with a 1:1
// passthrough test rather than relying on the eye.
// ---------------------------------------------------------------------------

namespace {
const uint8_t* g_srcPx = nullptr;
int            g_srcW  = 0;
int            g_srcH  = 0;
}  // namespace

static inline float tap01(int x, int y, int c) {
    x = std::min(std::max(x, 0), g_srcW - 1);
    y = std::min(std::max(y, 0), g_srcH - 1);
    return float(g_srcPx[(size_t(y) * g_srcW + x) * 4 + c]) * (1.0f / 255.0f);
}

static inline AF4 gather01(AF2 q, int c) {
    const int i0 = int(std::floor(q.x * float(g_srcW) - 0.5f));
    const int j0 = int(std::floor(q.y * float(g_srcH) - 0.5f));
    return AF4{tap01(i0,     j0 + 1, c),
               tap01(i0 + 1, j0 + 1, c),
               tap01(i0 + 1, j0,     c),
               tap01(i0,     j0,     c)};
}

AF4 FsrEasuRF(AF2 p) { return gather01(p, 0); }
AF4 FsrEasuGF(AF2 p) { return gather01(p, 1); }
AF4 FsrEasuBF(AF2 p) { return gather01(p, 2); }

// RCAS's 3x3 cross reads, clamped the same way the shader clamps its sampler.
AF4 FsrRcasLoadF(ASU2 q) {
    const int x = q.x < 0 ? 0 : (q.x >= g_srcW ? g_srcW - 1 : q.x);
    const int y = q.y < 0 ? 0 : (q.y >= g_srcH ? g_srcH - 1 : q.y);
    const uint8_t* p = g_srcPx + (size_t(y) * size_t(g_srcW) + size_t(x)) * 4;
    return AF4{float(p[0]) * (1.0f / 255.0f),
               float(p[1]) * (1.0f / 255.0f),
               float(p[2]) * (1.0f / 255.0f),
               1.0f};
}

// RCAS's optional colour-space hook; the whole pipeline works in the encoded
// space, so there is nothing to convert.
void FsrRcasInputF(AF1& r, AF1& g, AF1& b) { (void)r; (void)g; (void)b; }

#define A_GPU 1
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
// AMD's header marks its unused constant helpers `static`; only the FsrEasu*/
// FsrRcas* entry points are called here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "ffx_fsr1_cpp.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------

namespace lsl {

Image upscaleFSR1CPU(const Image& src, uint32_t dstW, uint32_t dstH) {
    Image out;
    out.w = dstW;
    out.h = dstH;
    out.rgba.resize(size_t(dstW) * dstH * 4);

    g_srcPx = src.rgba.data();
    g_srcW  = int(src.w);
    g_srcH  = int(src.h);

    // Same constant generation as the shader.
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3,
               float(src.w), float(src.h),
               float(src.w), float(src.h),
               float(dstW),   float(dstH));

    for (uint32_t dy = 0; dy < dstH; ++dy) {
        for (uint32_t dx = 0; dx < dstW; ++dx) {
            AF3 c;
            FsrEasuF(c, AU2{dx, dy}, con0, con1, con2, con3);

            uint8_t* q = out.rgba.data() + (size_t(dy) * dstW + dx) * 4;
            q[0] = quant255(c.x);
            q[1] = quant255(c.y);
            q[2] = quant255(c.z);
            q[3] = 255;
        }
    }

    g_srcPx = nullptr;
    return out;
}

Image rcasCPU(const Image& src, float sharpness) {
    Image out;
    out.w = src.w;
    out.h = src.h;
    out.rgba.resize(src.rgba.size());

    g_srcPx = src.rgba.data();
    g_srcW  = int(src.w);
    g_srcH  = int(src.h);

    // Same constant generation as shaders/rcas.comp.
    AU4 con;
    FsrRcasCon(con, sharpness);

    for (uint32_t y = 0; y < src.h; ++y) {
        for (uint32_t x = 0; x < src.w; ++x) {
            AF1 r, g, b;
            FsrRcasF(r, g, b, AU2{x, y}, con);

            uint8_t* q = out.rgba.data() + (size_t(y) * src.w + x) * 4;
            q[0] = quant255(r);
            q[1] = quant255(g);
            q[2] = quant255(b);
            q[3] = 255;
        }
    }

    g_srcPx = nullptr;
    return out;
}

}  // namespace lsl
