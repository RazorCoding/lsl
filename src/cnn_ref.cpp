// Anime4K v4 Restore CNN (S), MIT (bloc97).  See src/cnn_ref.hpp.
//
// This is a direct transcription of the vendored GLSL in
// third_party/anime4k/Anime4K_Restore_CNN_S.glsl, with two differences that
// are forced by where lsl runs it rather than by taste:
//
//   * `texOff` is a clamped integer fetch, not a texture sample.  Anime4K's
//     texOff offsets are whole texels, so this is what those shaders mean at
//     integer positions; lsl has no sampler to blur them with.
//   * colour arrives as the EASU output, unorm in [0,1].  The convolution
//     arithmetic is identical.
//
// shaders/cnn.comp computes the same thing one layer per dispatch, with the
// same weight table, so the two are compared pixel for pixel by lsl-upscale.

#include "cnn_ref.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "anime4k_restore_s.h"

namespace lsl {
namespace {

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

const float* layer_w(int layer, int tap, int which) {
    const LslAnime4kLayer& L = lsl_anime4k_layers[layer];
    // A split layer stores its nine go_0 matrices and then its nine go_1
    // matrices, which is the order they appear in the GLSL.
    const int off = L.woff + which * LSL_ANIME4K_GO1_OFFSET + tap * 16;
    return &lsl_anime4k_weights[off];
}

// Reads the packed source image as normalised RGBA.
inline void sampleImage(const Image& img, int x, int y, float* v) {
    x = clampi(x, 0, int(img.w) - 1);
    y = clampi(y, 0, int(img.h) - 1);
    const uint8_t* p = img.rgba.data() + (size_t(y) * img.w + x) * 4;
    v[0] = p[0] / 255.0f;
    v[1] = p[1] / 255.0f;
    v[2] = p[2] / 255.0f;
    v[3] = p[3] / 255.0f;
}

// GLSL's mat4(...) is column major, and (M * v) is sum_col v[col] * column.
inline void applyMat(const float* m, const float* v, float* out) {
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];
    out[3] = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];
}

// One layer.  `in` is ignored when `read_base`; the base image is also the
// per-pixel skip connection when the layer has add_base.
void convLayer(const Image& base, const float* in, float* out, int layer,
               bool read_base) {
    const int W = int(base.w), H = int(base.h);
    const LslAnime4kLayer& L = lsl_anime4k_layers[layer];

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float acc[4] = {lsl_anime4k_weights[L.boff + 0],
                            lsl_anime4k_weights[L.boff + 1],
                            lsl_anime4k_weights[L.boff + 2],
                            lsl_anime4k_weights[L.boff + 3]};

            for (int t = 0; t < 9; ++t) {
                const int sx = clampi(x + lsl_anime4k_offsets[t][0], 0, W - 1);
                const int sy = clampi(y + lsl_anime4k_offsets[t][1], 0, H - 1);

                float v[4];
                if (read_base) {
                    sampleImage(base, sx, sy, v);
                } else {
                    const float* p = in + (size_t(sy) * W + sx) * 4;
                    v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; v[3] = p[3];
                }

                float tv[4];
                const float pos[4] = {std::max(v[0], 0.0f), std::max(v[1], 0.0f),
                                      std::max(v[2], 0.0f), std::max(v[3], 0.0f)};
                applyMat(layer_w(layer, t, 0), pos, tv);
                for (int c = 0; c < 4; ++c) acc[c] += tv[c];

                if (L.split) {
                    const float neg[4] = {std::max(-v[0], 0.0f),
                                          std::max(-v[1], 0.0f),
                                          std::max(-v[2], 0.0f),
                                          std::max(-v[3], 0.0f)};
                    applyMat(layer_w(layer, t, 1), neg, tv);
                    for (int c = 0; c < 4; ++c) acc[c] += tv[c];
                }
            }

            if (L.add_base) {
                float v[4];
                sampleImage(base, x, y, v);
                for (int c = 0; c < 4; ++c) acc[c] += v[c];
            }

            float* o = out + (size_t(y) * W + x) * 4;
            o[0] = acc[0]; o[1] = acc[1]; o[2] = acc[2]; o[3] = acc[3];
        }
    }
}

}  // namespace

Image anime4kRestoreCPU(const Image& src) {
    if (src.w == 0 || src.h == 0)
        throw std::runtime_error("anime4kRestoreCPU: empty source");

    const size_t N = size_t(src.w) * size_t(src.h);
    std::vector<float> a(N * 4), b(N * 4);

    // Layer 0 reads the packed image; 1 and 2 ping-pong; 3 adds the base back.
    convLayer(src, nullptr, a.data(), 0, true);
    convLayer(src, a.data(), b.data(), 1, false);
    convLayer(src, b.data(), a.data(), 2, false);
    convLayer(src, a.data(), b.data(), 3, false);

    Image out = src;
    for (size_t i = 0; i < N; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = std::max(0.0f, std::min(1.0f, b[i * 4 + c]));
            out.rgba[i * 4 + c] = uint8_t(v * 255.0f + 0.5f);
        }
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

}  // namespace lsl
