#pragma once
// CPU reference for the NIS (NVIDIA Image Scaling) sharpening pass.
//
// lsl's `nis` method is FSR1's EASU upscale followed by NIS's *sharpening*
// stage -- a 1:1 directional unsharp mask with an edge map.  That is the half
// of NIS that the SDK's own `NVSharpen` entry point implements, and the half
// games enable when they offer "NIS" without changing resolution.  The NIS
// *scaler* is not here: it resamples through a texture sampler, which the
// layer's storage-buffer pipeline deliberately has no equivalent for (see the
// README's "Methods" section).
//
// The arithmetic is transcribed from NVIDIAGameWorks/NVIDIAImageScaling
// (NIS_Scaler.h, NVSharpen + GetDirUSM + GetEdgeMap + CalcLTIFast + EvalUSM,
// MIT) at the neutral slider position, and mirrored line for line by
// shaders/nis.comp.  The layer-vs-benchmark gate in tools/upscale-check.sh is
// what keeps the two in step.

#include "image_io.hpp"

namespace lsl {

// 1:1 sharpening.  Returns an image the same size as `src`.
Image nisSharpenCPU(const Image& src);

}  // namespace lsl
