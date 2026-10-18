#include "reference.hpp"
#include "nis_ref.hpp"
#include "cnn_ref.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace lsl {
namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float sinc(float x) {
    x = std::fabs(x);
    return (x < 1.0e-5f) ? 1.0f : std::sin(kPi * x) / (kPi * x);
}

inline float lanczos2Weight(float x) {
    x = std::fabs(x);
    if (x >= 2.0f) return 0.0f;
    return sinc(x * 0.5f) * sinc(x);
}

// Lanczos-3: the same windowed sinc as lanczos2 with the lobe widened from 2
// to 3. The kernel is no longer strictly positive and rings a little more, but
// keeps more detail; it is the widest of the pure separably-filtered methods
// here. Mirrors lanczos3Weight() in shaders/scale.comp.
inline float lanczos3Weight(float x) {
    x = std::fabs(x);
    if (x >= 3.0f) return 0.0f;
    return sinc(x * (1.0f / 3.0f)) * sinc(x);
}

inline float cubicWeight(float x) {
    const float B = 0.0f;
    const float C = 0.5f;
    x = std::fabs(x);
    const float x2 = x * x;
    const float x3 = x2 * x;
    if (x <= 1.0f)
        return ((12.0f - 9.0f * B - 6.0f * C) * x3 +
                (-18.0f + 12.0f * B + 6.0f * C) * x2 +
                (6.0f - 2.0f * B)) / 6.0f;
    if (x < 2.0f)
        return ((-B - 6.0f * C) * x3 +
                (6.0f * B + 30.0f * C) * x2 +
                (-12.0f * B - 48.0f * C) * x +
                (8.0f * B + 24.0f * C)) / 6.0f;
    return 0.0f;
}

struct V3 { float r, g, b; };

// Mirrors axisMap() in shaders/scale.comp exactly, including the 32-bit
// integer intermediate, so the CPU and GPU agree bit-for-bit even when a
// destination pixel lands exactly on a texel boundary.
struct Axis {
    int   base;
    float frac;
    bool  roundUp;
};

inline Axis axisMap(uint32_t d, uint32_t srcSize, uint32_t dstSize) {
    const int den = 2 * int(dstSize);
    const uint32_t prod = (2u * d + 1u) * srcSize;
    const int n = int(prod) - int(dstSize);

    Axis a{};
    a.base = (n >= 0) ? (n / den) : -(((-n) + den - 1) / den);
    const int rem = n - a.base * den;
    a.frac    = float(rem) / float(den);
    a.roundUp = (2 * rem) >= den;
    return a;
}

inline V3 fetchRef(const uint8_t* p, uint32_t w, uint32_t h, int x, int y) {
    x = x < 0 ? 0 : (x >= int(w) ? int(w) - 1 : x);
    y = y < 0 ? 0 : (y >= int(h) ? int(h) - 1 : y);
    const uint8_t* q = p + (size_t(y) * w + size_t(x)) * 4;
    return V3{float(q[0]), float(q[1]), float(q[2])};
}

inline V3 lerp3(V3 a, V3 b, float t) {
    return V3{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

inline uint8_t clamp255(float v) {
    const float r = std::lround(v);
    if (r <= 0.0f)   return 0;
    if (r >= 255.0f) return 255;
    return uint8_t(r);
}

}  // namespace

// modeName()/modeLabel() live in method.hpp, next to the table they read.

Image upscaleCPU(const Image& src, uint32_t dstW, uint32_t dstH, uint32_t mode,
                 float sharpness) {
    if (src.empty()) throw std::runtime_error("upscaleCPU: empty source");
    if (dstW == 0 || dstH == 0) throw std::runtime_error("upscaleCPU: zero destination");

    // FSR1 derives its own sample positions from the invocation index, so it
    // does not go through axisMap(). The full FSR1 chain is EASU followed by
    // RCAS sharpening at the destination resolution.
    if (mode == MODE_FSR1)     return upscaleFSR1CPU(src, dstW, dstH);
    if (mode == MODE_FSR1RCAS) return rcasCPU(upscaleFSR1CPU(src, dstW, dstH), sharpness);

    // The other two refiners sit on an EASU upscale the same way fsr1rcas does:
    // EASU resamples, then a 1:1 pass runs at the destination resolution. The
    // `sharpness` knob is not consulted -- the NIS USM profile and the Anime4K
    // weights are fixed.
    if (mode == MODE_NIS) return nisSharpenCPU(upscaleFSR1CPU(src, dstW, dstH));
    if (mode == MODE_CNN) return anime4kRestoreCPU(upscaleFSR1CPU(src, dstW, dstH));

    Image out;
    out.w = dstW;
    out.h = dstH;
    out.rgba.resize(size_t(dstW) * dstH * 4);

    const uint8_t* sp = src.rgba.data();
    const uint32_t sw = src.w, sh = src.h;

    for (uint32_t dy = 0; dy < dstH; ++dy) {
        for (uint32_t dx = 0; dx < dstW; ++dx) {
            const Axis ax = axisMap(dx, sw, dstW);
            const Axis ay = axisMap(dy, sh, dstH);
            const int   bx = ax.base, by = ay.base;
            const float fx = ax.frac, fy = ay.frac;

            V3 v{0.0f, 0.0f, 0.0f};

            switch (mode) {
                case MODE_NEAREST:
                    v = fetchRef(sp, sw, sh, bx + (ax.roundUp ? 1 : 0),
                                           by + (ay.roundUp ? 1 : 0));
                    break;

                case MODE_BILINEAR: {
                    V3 a = lerp3(fetchRef(sp, sw, sh, bx,     by),
                                 fetchRef(sp, sw, sh, bx + 1, by),     fx);
                    V3 b = lerp3(fetchRef(sp, sw, sh, bx,     by + 1),
                                 fetchRef(sp, sw, sh, bx + 1, by + 1), fx);
                    v = lerp3(a, b, fy);
                    break;
                }

                case MODE_CUBIC: {
                    float r = 0, g = 0, b = 0;
                    for (int j = -1; j <= 2; ++j) {
                        const float wy = cubicWeight(fy - float(j));
                        for (int k = -1; k <= 2; ++k) {
                            const float w = wy * cubicWeight(fx - float(k));
                            V3 c = fetchRef(sp, sw, sh, bx + k, by + j);
                            r += c.r * w; g += c.g * w; b += c.b * w;
                        }
                    }
                    v = V3{r, g, b};
                    break;
                }

                case MODE_LANCZOS2: {
                    float r = 0, g = 0, b = 0, wsum = 0;
                    for (int j = -1; j <= 2; ++j) {
                        const float wy = lanczos2Weight(fy - float(j));
                        for (int k = -1; k <= 2; ++k) {
                            const float w = wy * lanczos2Weight(fx - float(k));
                            V3 c = fetchRef(sp, sw, sh, bx + k, by + j);
                            r += c.r * w; g += c.g * w; b += c.b * w;
                            wsum += w;
                        }
                    }
                    if (std::fabs(wsum) > 1.0e-6f) { r /= wsum; g /= wsum; b /= wsum; }
                    v = V3{r, g, b};
                    break;
                }

                case MODE_LANCZOS3: {
                    float r = 0, g = 0, b = 0, wsum = 0;
                    for (int j = -2; j <= 3; ++j) {
                        const float wy = lanczos3Weight(fy - float(j));
                        for (int k = -2; k <= 3; ++k) {
                            const float w = wy * lanczos3Weight(fx - float(k));
                            V3 c = fetchRef(sp, sw, sh, bx + k, by + j);
                            r += c.r * w; g += c.g * w; b += c.b * w;
                            wsum += w;
                        }
                    }
                    if (std::fabs(wsum) > 1.0e-6f) { r /= wsum; g /= wsum; b /= wsum; }
                    v = V3{r, g, b};
                    break;
                }

                default:
                    throw std::runtime_error("upscaleCPU: unknown mode");
            }

            uint8_t* q = out.rgba.data() + (size_t(dy) * dstW + dx) * 4;
            q[0] = clamp255(v.r);
            q[1] = clamp255(v.g);
            q[2] = clamp255(v.b);
            q[3] = 255;
        }
    }
    return out;
}

Image downscaleBox(const Image& src, uint32_t dstW, uint32_t dstH) {
    if (dstW == 0 || dstH == 0)
        throw std::runtime_error("downscaleBox: zero destination extent");
    if (src.w == 0 || src.h == 0)
        throw std::runtime_error("downscaleBox: empty source");

    Image out;
    out.w = dstW;
    out.h = dstH;
    out.rgba.assign(size_t(dstW) * dstH * 4, 0);

    const double sx = double(src.w) / double(dstW);
    const double sy = double(src.h) / double(dstH);

    for (uint32_t y = 0; y < dstH; ++y) {
        const double y0 = double(y) * sy;
        const double y1 = y0 + sy;
        const uint32_t iy0 = uint32_t(y0);
        const uint32_t iy1 = std::min<uint32_t>(src.h, uint32_t(std::ceil(y1)));
        for (uint32_t x = 0; x < dstW; ++x) {
            const double x0 = double(x) * sx;
            const double x1 = x0 + sx;
            const uint32_t ix0 = uint32_t(x0);
            const uint32_t ix1 = std::min<uint32_t>(src.w, uint32_t(std::ceil(x1)));

            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            double wsum = 0.0;
            for (uint32_t iy = iy0; iy < iy1; ++iy) {
                const double wy =
                    std::min(y1, double(iy + 1)) - std::max(y0, double(iy));
                if (wy <= 0.0) continue;
                const uint8_t* row = &src.rgba[size_t(iy) * src.w * 4];
                for (uint32_t ix = ix0; ix < ix1; ++ix) {
                    const double wx =
                        std::min(x1, double(ix + 1)) - std::max(x0, double(ix));
                    if (wx <= 0.0) continue;
                    const double w = wx * wy;
                    const uint8_t* p = &row[size_t(ix) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += w * double(p[c]);
                    wsum += w;
                }
            }

            uint8_t* q = &out.rgba[(size_t(y) * dstW + x) * 4];
            for (int c = 0; c < 4; ++c) {
                const double v = acc[c] / wsum + 0.5;
                q[c] = uint8_t(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v));
            }
        }
    }
    return out;
}

double psnrRGB(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h)
        throw std::runtime_error("psnrRGB: size mismatch");
    double se = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < size_t(a.w) * a.h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = double(a.rgba[i * 4 + c]) - double(b.rgba[i * 4 + c]);
            se += d * d;
            ++n;
        }
    }
    if (n == 0) return 0.0;
    const double mse = se / double(n);
    // Identical images must outrank merely near-identical ones, which can
    // legitimately reach ~110 dB when only a handful of samples differ by 1.
    if (mse <= 1.0e-12) return 999.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

double ssimRGB(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h)
        throw std::runtime_error("ssimRGB: size mismatch");

    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    const size_t n = size_t(a.w) * a.h;

    double total = 0.0;
    for (int c = 0; c < 3; ++c) {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < n; ++i) {
            ma += a.rgba[i * 4 + c];
            mb += b.rgba[i * 4 + c];
        }
        ma /= double(n);
        mb /= double(n);

        double va = 0, vb = 0, cov = 0;
        for (size_t i = 0; i < n; ++i) {
            const double da = double(a.rgba[i * 4 + c]) - ma;
            const double db = double(b.rgba[i * 4 + c]) - mb;
            va += da * da;
            vb += db * db;
            cov += da * db;
        }
        va /= double(n - 1 ? n - 1 : 1);
        vb /= double(n - 1 ? n - 1 : 1);
        cov /= double(n - 1 ? n - 1 : 1);

        const double num = (2 * ma * mb + C1) * (2 * cov + C2);
        const double den = (ma * ma + mb * mb + C1) * (va + vb + C2);
        total += (den > 0.0) ? num / den : 1.0;
    }
    return total / 3.0;
}

double lapAbsRGB(const Image& img) {
    if (img.w < 3 || img.h < 3) return 0.0;

    const int w = int(img.w), h = int(img.h);
    auto at = [&](int x, int y, int c) -> double {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return double(img.rgba[(size_t(y) * w + size_t(x)) * 4 + size_t(c)]);
    };

    double acc = 0.0;
    size_t n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const double lap = 4.0 * at(x, y, c) - at(x, y - 1, c) -
                                   at(x, y + 1, c) - at(x - 1, y, c) - at(x + 1, y, c);
                acc += std::fabs(lap);
                ++n;
            }
        }
    }
    return n ? acc / double(n) : 0.0;
}

}  // namespace lsl
