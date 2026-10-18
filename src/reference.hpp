#pragma once
// CPU reference implementations of the same scalers, used to validate the GPU
// path and to produce PSNR/SSIM figures. Coordinate convention and kernels are
// kept byte-for-byte equivalent to the matching shader.

#include "image_io.hpp"
#include "method.hpp"  // ScaleMode and the method name table

#include <cstdint>

namespace lsl {

// The mode ids (MODE_NEAREST .. MODE_COUNT) live in method.hpp, together with
// the names everything else uses to talk about them.

// RCAS strength in stops: 0 is the strongest lobe AMD's RCAS will apply, and
// each whole number halves it. This matches the default AMD's own sample uses
// (FsrRcasCon(con, 0.0)).
constexpr float kDefaultSharpness = 0.0f;

// `sharpness` is consulted by MODE_FSR1RCAS (RCAS strength, in stops); the
// other modes ignore it.
Image upscaleCPU(const Image& src, uint32_t dstW, uint32_t dstH, uint32_t mode,
                 float sharpness = kDefaultSharpness);

// FSR1's EASU filter, compiled from AMD's own source. Lives in fsr1_ref.cpp
// because it needs a different header arrangement from the hand-written
// scalers in reference.cpp. Produces the unsharpened EASU output.
Image upscaleFSR1CPU(const Image& src, uint32_t dstW, uint32_t dstH);

// FSR1's RCAS sharpening pass, also compiled from AMD's source. In-place in
// spirit: takes an image and returns one of the same size, sharpened.
Image rcasCPU(const Image& src, float sharpness);

// Area-average (box) downsample, used to build a low-resolution proxy of a real
// image so that an upscaler can be scored against the *original* rather than
// against a CPU copy of itself.
//
// Box averaging is the right choice here precisely because it is boring: it
// preserves the average of each covered region, so it does not pre-bias the
// comparison towards whichever filter the upscaler happens to use. (Bilinear
// downsampling, for instance, would discard half the source taps and flatter
// the smoother filters.)
Image downscaleBox(const Image& src, uint32_t dstW, uint32_t dstH);

// PSNR over RGB (alpha ignored). Returns 999.0 for identical images.
double psnrRGB(const Image& a, const Image& b);

// Global SSIM over RGB (mean/variance/covariance of the whole image per
// channel). Global rather than windowed: cheap, adequate for regression gates.
double ssimRGB(const Image& a, const Image& b);

// Mean absolute 3x3 Laplacian over RGB, a crude edge-energy proxy: higher means
// more local contrast. Reported because PSNR and SSIM both reward softness, so
// neither can tell that a sharpening pass did anything except cost fidelity.
// It is NOT a quality score - noise and ringing raise it too - and it is only
// comparable between images of the same content and size.
double lapAbsRGB(const Image& img);

}  // namespace lsl
