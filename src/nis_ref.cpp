// NIS (NVIDIA Image Scaling) sharpening, ported from NVIDIA's MIT-licensed
// NIS_Scaler.h.  See src/nis_ref.hpp for which part of NIS this is and why.
//
// The constants are NVSharpenUpdateConfig()'s, evaluated at the sharpness
// slider's default of 0.5.  The profile is fixed in lsl: the `nis` method does
// not take the RCAS-stops `sharpness` knob, because that knob's meaning (0 is
// strongest, larger is softer) runs opposite to NIS's slider and folding both
// into one number would be a trap.  shaders/nis.comp carries the same constants
// as literals; every `lsl-upscale --modes nis` run compares the two.

#include "nis_ref.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace lsl {
namespace {

// NVSharpenUpdateConfig(sharpness=0.5), SDR path.
constexpr float kEps                = 1.0f / 255.0f;
constexpr float kDetectRatio        = 2.0f * 1127.0f / 1024.0f;
constexpr float kDetectThres        = 64.0f / 1024.0f;
constexpr float kMinContrastRatio   = 2.0f;
constexpr float kRatioNorm          = 1.0f / 8.0f;  // 1/(kMaxContrastRatio-kMinContrastRatio)
constexpr float kContrastBoost      = 1.0f;
constexpr float kSharpStartY        = 0.45f;
constexpr float kSharpScaleY        = 1.0f / 0.45f;  // 1/(kSharpEndY-kSharpStartY)
constexpr float kSharpStrengthMin   = 0.4f;
constexpr float kSharpStrengthScale = 1.2f;  // kSharpStrengthMax-kSharpStrengthMin
constexpr float kSharpLimitMin      = 0.14f;
constexpr float kSharpLimitScale    = 0.36f;  // kSharpLimitMax-kSharpLimitMin

inline float sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline uint8_t unorm8(float v) {
    const float x = sat(v) * 255.0f + 0.5f;
    return uint8_t(x);
}

// CalcLTIFast: a contrast-adaptive attenuator that keeps the USM from
// over-shooting at already-hard edges ("reduce ringing" in NVIDIA's comment).
float calcLTI(const float y[5]) {
    const float a_min = std::min(std::min(y[0], y[1]), y[2]);
    const float a_max = std::max(std::max(y[0], y[1]), y[2]);
    const float b_min = std::min(std::min(y[2], y[3]), y[4]);
    const float b_max = std::max(std::max(y[2], y[3]), y[4]);
    const float a_cont = a_max - a_min;
    const float b_cont = b_max - b_min;
    const float cont_ratio =
        std::max(a_cont, b_cont) / (std::min(a_cont, b_cont) + kEps);
    return (1.0f - sat((cont_ratio - kMinContrastRatio) * kRatioNorm)) *
           kContrastBoost;
}

// EvalUSM: NVIDIA's 3-tap profile (-0.6001, 1.2002, -0.6001), scaled by the
// luma-dependent strength and clamped to the luma-dependent limit.
float evalUSM(const float pxl[5], float strength, float limit) {
    float y_usm = -0.6001f * pxl[1] + 1.2002f * pxl[2] - 0.6001f * pxl[3];
    y_usm *= strength;
    y_usm = std::min(limit, std::max(-limit, y_usm));
    y_usm *= calcLTI(pxl);
    return y_usm;
}

// GetDirUSM: the same profile along 0/90/45/135 degrees.
void dirUSM(const float p[5][5], float r[4]) {
    const float scaleY = 1.0f - sat((p[2][2] - kSharpStartY) * kSharpScaleY);
    const float strength = scaleY * kSharpStrengthScale + kSharpStrengthMin;
    const float limit = (scaleY * kSharpLimitScale + kSharpLimitMin) * p[2][2];

    float d0[5], d90[5], d45[5], d135[5];
    for (int i = 0; i < 5; ++i) d0[i] = p[i][2];
    for (int i = 0; i < 5; ++i) d90[i] = p[2][i];

    d45[0] = p[1][1];
    d45[1] = lerp(p[2][1], p[1][2], 0.5f);
    d45[2] = p[2][2];
    d45[3] = lerp(p[3][2], p[2][3], 0.5f);
    d45[4] = p[3][3];

    d135[0] = p[3][1];
    d135[1] = lerp(p[3][2], p[2][1], 0.5f);
    d135[2] = p[2][2];
    d135[3] = lerp(p[2][3], p[1][2], 0.5f);
    d135[4] = p[1][3];

    r[0] = evalUSM(d0, strength, limit);
    r[1] = evalUSM(d90, strength, limit);
    r[2] = evalUSM(d45, strength, limit);
    r[3] = evalUSM(d135, strength, limit);
}

// GetEdgeMap at (i,j)=(1,1): a weight per direction saying which way the local
// gradient actually runs.  The weights sum to one; over flat ground they are
// all zero, which is what makes the pass leave flat areas alone.
void edgeMap(const float p[5][5], int i, int j, float w[4]) {
    const float g_0 = std::fabs(p[i + 0][j + 0] + p[i + 0][j + 1] + p[i + 0][j + 2] -
                                p[i + 2][j + 0] - p[i + 2][j + 1] - p[i + 2][j + 2]);
    const float g_45 = std::fabs(p[i + 1][j + 0] + p[i + 0][j + 0] + p[i + 0][j + 1] -
                                 p[i + 2][j + 1] - p[i + 2][j + 2] - p[i + 1][j + 2]);
    const float g_90 = std::fabs(p[i + 0][j + 0] + p[i + 1][j + 0] + p[i + 2][j + 0] -
                                 p[i + 0][j + 2] - p[i + 1][j + 2] - p[i + 2][j + 2]);
    const float g_135 = std::fabs(p[i + 1][j + 0] + p[i + 2][j + 0] + p[i + 2][j + 1] -
                                  p[i + 0][j + 1] - p[i + 0][j + 2] - p[i + 1][j + 2]);

    const float g_0_90_max = std::max(g_0, g_90);
    const float g_0_90_min = std::min(g_0, g_90);
    const float g_45_135_max = std::max(g_45, g_135);
    const float g_45_135_min = std::min(g_45, g_135);

    w[0] = w[1] = w[2] = w[3] = 0.0f;
    if (g_0_90_max + g_45_135_max == 0.0f) return;

    const float e_0_90 =
        std::min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0f);
    const float e_45_135 = 1.0f - e_0_90;

    const bool c_0_90 = (g_0_90_max > (g_0_90_min * kDetectRatio)) &&
                        (g_0_90_max > kDetectThres) &&
                        (g_0_90_max > g_45_135_min);
    const bool c_45_135 = (g_45_135_max > (g_45_135_min * kDetectRatio)) &&
                          (g_45_135_max > kDetectThres) &&
                          (g_45_135_max > g_0_90_min);
    const bool c_g_0_90 = (g_0_90_max == g_0);
    const bool c_g_45_135 = (g_45_135_max == g_45);

    const float f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0f;
    const float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0f;

    w[0] = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0f;
    w[1] = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0f;
    w[2] = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0f;
    w[3] = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0f;
}

}  // namespace

Image nisSharpenCPU(const Image& src) {
    if (src.w == 0 || src.h == 0)
        throw std::runtime_error("nisSharpenCPU: empty source");

    const int W = int(src.w), H = int(src.h);

    // The pass works on luma; NVSharpen builds its tile from getY() of the
    // sampled colour and later adds the result back to the sampled RGB.
    std::vector<float> lum(size_t(W) * size_t(H));
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = src.rgba.data() + (size_t(y) * W + x) * 4;
            lum[size_t(y) * W + x] =
                (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
        }
    }

    Image out = src;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float p[5][5];
            for (int i = 0; i < 5; ++i) {
                const int sy = std::min(std::max(y + i - 2, 0), H - 1);
                for (int j = 0; j < 5; ++j) {
                    const int sx = std::min(std::max(x + j - 2, 0), W - 1);
                    p[i][j] = lum[size_t(sy) * W + sx];
                }
            }

            float d[4];
            dirUSM(p, d);
            float w[4];
            edgeMap(p, 1, 1, w);
            const float usmY =
                d[0] * w[0] + d[1] * w[1] + d[2] * w[2] + d[3] * w[3];

            const uint8_t* q = src.rgba.data() + (size_t(y) * W + x) * 4;
            uint8_t* o = out.rgba.data() + (size_t(y) * W + x) * 4;
            o[0] = unorm8(q[0] / 255.0f + usmY);
            o[1] = unorm8(q[1] / 255.0f + usmY);
            o[2] = unorm8(q[2] / 255.0f + usmY);
            o[3] = q[3];
        }
    }
    return out;
}

}  // namespace lsl
