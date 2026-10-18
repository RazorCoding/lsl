#pragma once
// CPU reference for the AI method: bloc97's Anime4K v4 Restore CNN (S), MIT.
//
// lsl's `cnn` method is an FSR1 EASU upscale followed by this network.  The
// network is a *restoration* CNN, not a from-scratch super-resolution model:
// it runs at a fixed resolution and refines whatever it is handed, which is
// exactly what makes it fit an arbitrary-scale present-time upscaler (EASU
// decides the geometry, the CNN decides the detail).  See README, "AI".
//
// The weights come from the vendored GLSL via tools/anime4k_weights.py, so the
// reference and the shader cannot disagree about them; every
// `lsl-upscale --modes cnn` run checks the two against each other.

#include "image_io.hpp"

namespace lsl {

// Refine `src` in place (same extent).  src is expected to be an already
// upscaled image; the CNN neither resamples nor changes the size.
Image anime4kRestoreCPU(const Image& src);

}  // namespace lsl
