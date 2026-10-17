// Unit tests for the CPU reference path and metrics. These gate the GPU
// numbers: if the reference is wrong, every PSNR column in the benchmark is
// meaningless.

#include "image_io.hpp"
#include "reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

void checkNear(double actual, double expected, double tol, const std::string& what) {
    check(std::fabs(actual - expected) <= tol,
          what + " (got " + std::to_string(actual) + ", want " +
              std::to_string(expected) + " +/- " + std::to_string(tol) + ")");
}

// solid-colour image
lsl::Image solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    lsl::Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

uint8_t at(const lsl::Image& img, uint32_t x, uint32_t y, int c) {
    return img.rgba[(size_t(y) * img.w + x) * 4 + c];
}

}  // namespace

int main() {
    using namespace lsl;

    // --- metrics behave on identical / different images -------------------
    {
        const Image a = makeTestPattern(64, 48);
        const Image b = a;
        checkNear(psnrRGB(a, b), 999.0, 1e-9, "PSNR of identical images");
        checkNear(ssimRGB(a, b), 1.0, 1e-6, "SSIM of identical images");

        const Image c = solid(64, 48, 0, 0, 0);
        const double p = psnrRGB(a, c);
        check(p < 40.0, "PSNR against a black image is finite and low, got " +
                            std::to_string(p));
    }

    // --- 1x1 source must tile to one flat colour ---------------------------
    // Every pure scaler reproduces the single texel exactly everywhere. The
    // full FSR1 chain lands within one level instead of exactly, because RCAS
    // divides by AMD's "medium precision" reciprocal (APrxMedRcpF1), which is
    // deliberately ~0.3% off so that it does not shift tonality - enough to
    // round white down to 254.
    {
        const Image one = solid(1, 1, 10, 200, 30);
        for (uint32_t mode = 0; mode < MODE_COUNT; ++mode) {
            const Image out = upscaleCPU(one, 17, 13, mode);
            // fsr1rcas may be off by one level; everything else must be exact.
            const int tol = (mode == MODE_FSR1RCAS) ? 1 : 0;
            bool flat = true;
            for (uint32_t y = 0; y < out.h && flat; ++y)
                for (uint32_t x = 0; x < out.w && flat; ++x)
                    flat = std::abs(int(at(out, x, y, 0)) - 10) <= tol &&
                           std::abs(int(at(out, x, y, 1)) - 200) <= tol &&
                           std::abs(int(at(out, x, y, 2)) - 30) <= tol;
            check(flat, std::string("1x1 ") + modeName(mode) +
                            (tol ? " tiles to flat colour (within one level)"
                                 : " tiles to flat colour"));
        }
    }

    // --- identity scale: every scaler must reproduce the source ------------
    {
        const Image src = makeTestPattern(96, 64);
        for (uint32_t mode = 0; mode < MODE_COUNT; ++mode) {
            const Image out = upscaleCPU(src, src.w, src.h, mode);
            const double p = psnrRGB(src, out);
            // nearest and bilinear are exact at 1:1; cubic/lanczos are exact
            // mathematically but pay for float accumulation. fsr1 and fsr1rcas
            // are sharpening filters (EASU, then EASU+RCAS), so at 1:1 they only
            // have to stay close - the tests below carry their real evidence.
            const double gate = (mode == MODE_CUBIC || mode == MODE_LANCZOS2) ? 49.0
                              : (mode == MODE_FSR1 || mode == MODE_FSR1RCAS) ? 18.0
                              : 90.0;
            check(p >= gate, std::string("identity scale ") + modeName(mode) +
                                 " PSNR " + std::to_string(p));
        }
    }

    // --- FSR1 preserves flat colour exactly --------------------------------
    // In a flat region every EASU tap is the same texel and the weights
    // normalise, so a flat image must survive untouched at any scale -
    // including through the deringing clamp, which can only re-clamp to the
    // same value. This catches a mis-normalised kernel or a gather that reads
    // outside the image.
    {
        const Image src = solid(37, 23, 91, 137, 213);
        const uint32_t dims[][2] = {{37, 23}, {74, 46}, {19, 11}};
        for (const auto& d : dims) {
            const Image out = upscaleCPU(src, d[0], d[1], MODE_FSR1);
            bool flat = true;
            for (uint32_t y = 0; y < out.h && flat; ++y)
                for (uint32_t x = 0; x < out.w && flat; ++x)
                    flat = at(out, x, y, 0) == 91 && at(out, x, y, 1) == 137 &&
                           at(out, x, y, 2) == 213;
            check(flat, "fsr1 preserves flat colour at " + std::to_string(d[0]) +
                            "x" + std::to_string(d[1]));
        }
    }

    // --- FSR1 sharpens a step edge without moving it -----------------------
    // A black/white vertical edge has to stay where it is; only its contrast
    // may change. Were the four components of the emulated textureGather
    // ordered wrongly, EASU would pull texels from the wrong side of the edge
    // and the transition column would move.
    {
        Image src = solid(16, 4, 0, 0, 0);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 8; x < 16; ++x) {
                src.rgba[(size_t(y) * 16 + x) * 4 + 0] = 255;
                src.rgba[(size_t(y) * 16 + x) * 4 + 1] = 255;
                src.rgba[(size_t(y) * 16 + x) * 4 + 2] = 255;
            }
        const Image out = upscaleCPU(src, 16, 4, MODE_FSR1);
        uint32_t crossing = 16;  // first interior column reaching mid grey
        for (uint32_t x = 0; x < 16; ++x)
            if (at(out, x, 2, 0) >= 128) { crossing = x; break; }
        check(crossing >= 7 && crossing <= 9,
              "fsr1 keeps the step edge in place (crossing column " +
                  std::to_string(crossing) + ", want 7..9)");
        check(at(out, 0, 2, 0) == 0 && at(out, 15, 2, 0) == 255,
              "fsr1 respects black and white away from the edge");
    }

    // --- nearest: explicit quadrant mapping on a 2x upscale ----------------
    {
        Image src;
        src.w = 2;
        src.h = 2;
        src.rgba = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   255, 255, 0, 255,
        };
        const Image out = upscaleCPU(src, 4, 4, MODE_NEAREST);
        check(at(out, 0, 0, 0) == 255 && at(out, 0, 0, 1) == 0, "nearest top-left = red");
        check(at(out, 3, 0, 1) == 255, "nearest top-right = green");
        check(at(out, 0, 3, 2) == 255, "nearest bottom-left = blue");
        check(at(out, 3, 3, 1) == 255 && at(out, 3, 3, 0) == 255, "nearest bottom-right = yellow");
        check(at(out, 1, 1, 0) == 255 && at(out, 1, 1, 1) == 0, "nearest holds within quadrant");
    }

    // --- bilinear: midpoint of two known texels ----------------------------
    {
        Image src;
        src.w = 2;
        src.h = 1;
        src.rgba = {0, 0, 0, 255,  200, 0, 0, 255};
        // 4x upscale: output pixel 1 sits between texel 0 and texel 1
        const Image out = upscaleCPU(src, 4, 1, MODE_BILINEAR);
        const int v = at(out, 1, 0, 0);
        check(v > 0 && v < 200, "bilinear interpolates between texels, got " +
                                    std::to_string(v));
        check(at(out, 0, 0, 0) == 0, "bilinear left edge stays 0");
        check(at(out, 3, 0, 0) == 200, "bilinear right edge stays 200");
    }

    // --- downscale must work and not blow up -------------------------------
    {
        const Image src = makeTestPattern(64, 64);
        for (uint32_t mode = 0; mode < MODE_COUNT; ++mode) {
            const Image out = upscaleCPU(src, 32, 32, mode);
            bool finite = true;
            for (size_t i = 0; i < size_t(out.w) * out.h * 4 && finite; ++i)
                finite = out.rgba[i] <= 255;  // uint8_t, so always true
            check(finite && out.w == 32 && out.h == 32,
                  std::string("downscale ") + modeName(mode));
        }
    }

    // --- non-integer scale (the real workload) -----------------------------
    {
        const Image src = makeTestPattern(160, 100);
        for (uint32_t mode = 0; mode < MODE_COUNT; ++mode) {
            const Image out = upscaleCPU(src, 256, 160, mode);
            check(out.w == 256 && out.h == 160,
                  std::string("1.6x ") + modeName(mode) + " dimensions");
        }
    }

    // --- nearest must not drift by one texel at exact boundaries -----------
    // Regression: 640 -> 1600 (2.5x) used to mis-map whole columns, because
    // floor((d + 0.5) * src / dst) evaluated in float lands on the wrong side
    // of the boundary when the result is an exact integer. The expected index
    // here is computed as an exact rational: floor((2d+1) * src / (2*dst)).
    {
        Image src;
        src.w = 640;
        src.h = 1;
        src.rgba.resize(size_t(640) * 4);
        for (uint32_t x = 0; x < 640; ++x) {
            src.rgba[x * 4 + 0] = uint8_t(x % 251);  // prime period: no ties
            src.rgba[x * 4 + 1] = 0;
            src.rgba[x * 4 + 2] = 0;
            src.rgba[x * 4 + 3] = 255;
        }
        const Image out = upscaleCPU(src, 1600, 1, MODE_NEAREST);
        for (uint32_t d : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u,
                           799u, 800u, 801u, 1598u, 1599u}) {
            const uint32_t want = uint32_t((2ull * d + 1ull) * 640ull / (2ull * 1600ull));
            check(at(out, d, 0, 0) == uint8_t(want % 251),
                  "nearest 2.5x exact mapping at dst x=" + std::to_string(d));
        }
    }

    // --- downscaleBox: a solid image stays that colour at any ratio --------
    {
        const Image src = solid(37, 23, 91, 12, 200);
        const Image out = downscaleBox(src, 9, 5);
        bool ok = out.w == 9 && out.h == 5;
        for (size_t i = 0; i < size_t(out.w) * out.h && ok; ++i)
            ok = out.rgba[i * 4 + 0] == 91 && out.rgba[i * 4 + 1] == 12 &&
                 out.rgba[i * 4 + 2] == 200 && out.rgba[i * 4 + 3] == 255;
        check(ok, "box downscale of a solid image");
    }

    // --- downscaleBox: identity is lossless --------------------------------
    {
        const Image src = makeTestPattern(48, 32);
        check(psnrRGB(src, downscaleBox(src, 48, 32)) == 999.0,
              "box downscale at 1:1 is lossless");
    }

    // --- downscaleBox: exact 2x2 averages at an integer factor -------------
    {
        Image src;
        src.w = 4;
        src.h = 4;
        src.rgba.assign(size_t(4) * 4 * 4, 255);
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                src.rgba[(size_t(y) * 4 + x) * 4 + 0] = uint8_t(y * 4 + x);
        const Image out = downscaleBox(src, 2, 2);
        // 2x2 blocks {0,1,4,5}=2.5 {2,3,6,7}=4.5 {8,9,12,13}=10.5
        // {10,11,14,15}=12.5, each then rounded half-up.
        check(at(out, 0, 0, 0) == 3, "box 2x2 average (0,1,4,5)");
        check(at(out, 1, 0, 0) == 5, "box 2x2 average (2,3,6,7)");
        check(at(out, 0, 1, 0) == 11, "box 2x2 average (8,9,12,13)");
        check(at(out, 1, 1, 0) == 13, "box 2x2 average (10,11,14,15)");
    }

    // --- downscaleBox: the mean survives an awkward ratio ------------------
    // This is what makes the harness honest: if the downsampler darkened or
    // brightened the image, every quality score would be measured against the
    // wrong ground truth.
    {
        const Image src = makeTestPattern(101, 67);
        const Image out = downscaleBox(src, 23, 17);
        double sumS = 0.0, sumO = 0.0;
        for (size_t i = 0; i < size_t(src.w) * src.h; ++i) sumS += src.rgba[i * 4];
        for (size_t i = 0; i < size_t(out.w) * out.h; ++i) sumO += out.rgba[i * 4];
        checkNear(sumO / double(size_t(out.w) * out.h),
                  sumS / double(size_t(src.w) * src.h), 1.5,
                  "box downscale preserves the mean");
    }

    // --- downscaleBox to 1x1 is the whole-image mean -----------------------
    {
        const Image src = makeTestPattern(37, 29);
        const Image out = downscaleBox(src, 1, 1);
        double sum = 0.0;
        const size_t n = size_t(src.w) * src.h;
        for (size_t i = 0; i < n; ++i) sum += src.rgba[i * 4];
        checkNear(double(out.rgba[0]), sum / double(n), 0.5,
                  "box downscale to 1x1 = image mean");
    }

    // --- RCAS: what the sharpener does, and deliberately does not ----------
    // RCAS clamps how far a pixel may be pushed away from its neighbourhood,
    // so it is contrast-adaptive rather than a plain unsharp mask: it does
    // nothing at a full black/white step (contrast there is already maximal)
    // and nothing at all in a flat region. What it sharpens is mid-contrast
    // detail. All three behaviours are pinned down here, because "sharper" is
    // exactly the property PSNR and SSIM cannot see.
    {
        // flat colour and both extremes survive, to within the one level the
        // medium-precision reciprocal can cost (see the 1x1 note above).
        const Image flat = solid(24, 16, 91, 137, 213);
        const Image out = rcasCPU(flat, 0.0f);
        bool same = out.w == 24 && out.h == 16;
        for (uint32_t y = 0; y < out.h && same; ++y)
            for (uint32_t x = 0; x < out.w && same; ++x)
                same = std::abs(int(at(out, x, y, 0)) - 91) <= 1 &&
                       std::abs(int(at(out, x, y, 1)) - 137) <= 1 &&
                       std::abs(int(at(out, x, y, 2)) - 213) <= 1;
        check(same, "rcas leaves flat colour alone (within one level)");

        const Image black = rcasCPU(solid(24, 16, 0, 0, 0), 0.0f);
        check(at(black, 12, 8, 0) == 0 && at(black, 12, 8, 1) == 0 &&
                  at(black, 12, 8, 2) == 0,
              "rcas keeps a black image black");
        const Image white = rcasCPU(solid(24, 16, 255, 255, 255), 0.0f);
        check(at(white, 12, 8, 0) >= 254 && at(white, 12, 8, 1) >= 254 &&
                  at(white, 12, 8, 2) >= 254,
              "rcas keeps white at or next to white");
    }

    // mid-contrast detail is where RCAS acts, and `sharpness` in stops can
    // only reduce that action (0 is AMD's strongest lobe; each whole stop
    // halves it). A flat image must stay at exactly zero edge energy at every
    // setting: a sharpener that invents structure in an empty region is
    // broken, not aggressive.
    {
        const Image flat = solid(32, 32, 128, 128, 128);
        for (float s : {0.0f, 1.0f, 2.0f, 4.0f})
            checkNear(lapAbsRGB(rcasCPU(flat, s)), 0.0, 1e-9,
                      "rcas edge energy of a flat image at " +
                          std::to_string(s) + " stops");

        Image checker = solid(32, 32, 0, 0, 0);
        for (uint32_t y = 0; y < 32; ++y)
            for (uint32_t x = 0; x < 32; ++x) {
                const uint8_t v = uint8_t(((x / 2 + y / 2) & 1) ? 110 : 130);
                for (int c = 0; c < 3; ++c)
                    checker.rgba[(size_t(y) * 32 + x) * 4 + c] = v;
            }
        const double in = lapAbsRGB(checker);
        const double e0 = lapAbsRGB(rcasCPU(checker, 0.0f));
        const double e1 = lapAbsRGB(rcasCPU(checker, 1.0f));
        const double e2 = lapAbsRGB(rcasCPU(checker, 2.0f));
        check(e0 > in * 2.0, "rcas sharpens mid-contrast detail (edge " +
                                 std::to_string(in) + " -> " +
                                 std::to_string(e0) + ")");
        check(e1 < e0 && e2 < e1, "rcas sharpness in stops only reduces sharpening");
    }

    // a step edge must stay where it is: a mis-ordered FsrRcasLoadF would read
    // the wrong neighbour and shift the transition by a column.
    {
        Image step = solid(32, 8, 0, 0, 0);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 16; x < 32; ++x)
                for (int c = 0; c < 3; ++c)
                    step.rgba[(size_t(y) * 32 + x) * 4 + c] = 255;
        const Image out = rcasCPU(step, 0.0f);
        bool held = true;
        for (uint32_t y = 0; y < 8 && held; ++y)
            // the first white column may read 254 for the same reason white
            // does, so compare against 254 rather than exact 255.
            held = at(out, 15, y, 0) == 0 && at(out, 16, y, 0) >= 254 &&
                   at(out, 0, y, 0) == 0 && at(out, 31, y, 0) >= 254;
        check(held, "rcas does not move a step edge");
    }

    // --- the full chain is exactly EASU followed by RCAS -------------------
    // The GPU runs fsr1rcas as two dispatches through a scratch buffer (see
    // src/upscale.cpp). If the scratch size, the barrier between the passes or
    // the descriptor wiring were wrong, the GPU would not reproduce this exact
    // composition.
    {
        const Image src = makeTestPattern(80, 50);
        const Image out = upscaleCPU(src, 128, 80, MODE_FSR1RCAS, 0.0f);
        const Image composed = rcasCPU(upscaleFSR1CPU(src, 128, 80), 0.0f);
        check(psnrRGB(out, composed) == 999.0, "fsr1rcas == rcas(fsr1) exactly");
        check(out.w == 128 && out.h == 80, "fsr1rcas output dimensions");
    }

    // --- mode names --------------------------------------------------------
    check(std::string(modeName(MODE_NEAREST)) == "nearest", "modeName(nearest)");
    check(std::string(modeName(MODE_LANCZOS2)) == "lanczos2", "modeName(lanczos2)");
    check(std::string(modeName(MODE_FSR1RCAS)) == "fsr1rcas", "modeName(fsr1rcas)");

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
