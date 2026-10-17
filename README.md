# lsl — Lossless Scaling-style upscaler for Linux

A from-scratch Vulkan compute upscaler for games running under Steam/Proton on
Wayland (CachyOS/Arch + Hyprland). No code, binaries, or models are taken from
Lossless Scaling or from `lsfg-vk`.

**Current state: Phase 1 only — a headless scaler core and a per-GPU
benchmark.** There is no layer, no game injection, and no frame generation yet.
Nothing here touches a running game.

---

## What is actually implemented

| Piece | State |
| --- | --- |
| `shaders/scale.comp` | nearest, bilinear, bicubic (Catmull-Rom), Lanczos-2, FSR1 EASU |
| `shaders/rcas.comp` | FSR1's RCAS sharpening pass — a second dispatch over an EASU scratch buffer |
| `src/reference.{hpp,cpp}` | CPU implementations of the four hand-written filters, area box downsampler, PSNR/SSIM, edge energy |
| `src/fsr1_ref.cpp` | CPU FSR1 EASU *and* RCAS — both compiled from AMD's own header, not re-derived |
| `third_party/ffx/` | AMD's MIT `ffx_a.h` + `ffx_fsr1.h`, vendored unmodified |
| `src/lsl_vk.{hpp,cpp}` | instance/device/compute-queue, device-local + staging buffers, pipelines, GPU-timestamp timing |
| `src/image_io.{hpp,cpp}` | PPM/PGM in, PPM out (no codec dependencies) |
| `src/probe.cpp` | `lsl-probe`: Vulkan device inventory + env traps |
| `src/upscale.cpp` | `lsl-upscale`: fidelity *and* quality benchmark |
| `tests/test_reference.cpp` | 79 CPU-reference checks |
| Validation layers | installable per-user without root; wired into `ctest` |
| FSR1 `balanced` tier | EASU upscaling **and** RCAS sharpening, both landed |
| Frame capture / layer injection | **not implemented** |
| Motion estimation / frame generation | **not implemented** |
| GUI, profiles, packaging | **not implemented** |

---

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

CTest runs 79 CPU reference checks plus a `validation` test that re-runs the
scaler under the Khronos validation layers. That test **skips itself** unless
the layers are installed:

```sh
tools/setup-validation-layers.sh     # per-user, no root, ~15 MB
```

Requires: a C++20 compiler, CMake ≥ 3.20, Ninja, `glslc`, Vulkan headers+loader,
Python 3. `libplacebo` is deliberately *not* a dependency.

## Run

```sh
./build/lsl-probe                    # what does this process actually see?

# fidelity, self-contained: synthetic 1600x1000 -> 2560x1600, writes the CSV
./build/lsl-upscale --make-test 1600x1000 --dst 2560x1600 \
                    --bench 300 --csv results/scalers.csv --out out.ppm

./build/lsl-upscale --in shot.ppm --dst 2560x1600 --modes fsr1,fsr1rcas

# quality needs a real image; the table below uses exactly this conversion
ffmpeg -i ~/Pictures/wallhaven-jeeq3q.png -vf scale=1600:1000:flags=lanczos \
       -pix_fmt rgb24 /tmp/photo.ppm
./build/lsl-upscale --in /tmp/photo.ppm --render 800x500 --bench 100  # all six modes

./build/lsl-upscale --in /tmp/photo.ppm --render 800x500 \
                    --modes fsr1rcas --sharpness 1   # RCAS strength in stops
```

`--device N` forces a physical device index; otherwise a discrete GPU is
preferred. `--sharpness N` (RCAS stops) is only read by `fsr1rcas`.

### Two different questions

The tool answers two different things, and conflating them is the easiest way to
draw a wrong conclusion:

```sh
# 1. fidelity: does the GPU agree with the CPU port of the same filter?
./build/lsl-upscale --make-test 640x400 --dst 1600x1000

# 2. quality: downsample a real image, upscale it back, compare to the original
./build/lsl-upscale --in photo.ppm --render 800x500
```

* **Without `--render`** the PSNR is against `reference.cpp` — a CPU
  implementation of the *same* kernel. A high score means the GPU port is
  correct. It says nothing about whether the picture looks good. This is the
  regression gate used by `ctest`.
* **With `--render`** the input is treated as ground truth, a proxy for the
  low-resolution render is made by area-averaging it down to `WxH`, it is
  scaled back up, and the score is against the original. That is the only one
  of the two that can compare *different* filters.

Pixel-difference metrics still do not measure sharpness: they reward whatever
minimises the squared error, and a slightly soft image often scores better than
a crisp one with a little ringing. Read the columns as "how much detail was
reconstructed", not "how good it looks".

A one-line example of each:

```
mode      dispatch_avg     psnr_db  ssim          # fidelity (vs CPU reference)
nearest      0.5244 ms      999.00  1.0000
bilinear     0.5647 ms       89.73  1.0000
cubic        1.2107 ms      103.11  1.0000
lanczos2     1.7562 ms       93.05  1.0000
fsr1         1.6142 ms       84.10  1.0000
fsr1rcas     1.9633 ms       57.26  1.0000

mode      dispatch_avg   psnr_vs_gt  ssim  edge    # quality (vs original)
nearest      0.2234 ms       34.53  0.9817  3.60
bilinear     0.3013 ms       35.04  0.9833  1.38
cubic        0.7795 ms       36.33  0.9878  1.79
lanczos2     1.1282 ms       36.37  0.9879  1.78
fsr1         0.8713 ms       35.87  0.9864  1.81
fsr1rcas     0.9322 ms       35.36  0.9852  4.23
```

Note how the *ordering* is stable but the numbers are nothing alike — 999 dB
versus 34.5 dB for the same `nearest` dispatch. That is why the distinction
matters.

### Host-specific trap

Hyprland on this machine sets `VK_LOADER_DRIVERS_DISABLE=nvidia_icd.json`
globally (`~/.config/hypr/hyprland.lua:26`), which hides the GTX 1650 from every
process. Steam works around it with `env -u VK_LOADER_DRIVERS_DISABLE
prime-run`. To benchmark the dGPU the same way:

```sh
env -u VK_LOADER_DRIVERS_DISABLE \
    VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
    ./build/lsl-upscale ...
```

`lsl-probe` prints a warning whenever it detects this variable.

---

## Measured results

1600×1000 → 2560×1600 (the panel's native mode). The source is the synthetic
`--make-test` pattern, so every number here is reproducible from the command
above. Dispatch time from GPU timestamps, 300 iterations, device-local storage.
`avg` / `p99` in ms; `edge` is mean |Laplacian| of the output on the 0–255 scale.

**NVIDIA GeForce GTX 1650 (discrete, 4 GB), driver 2580775424**

| mode | avg | p99 | PSNR vs CPU ref | SSIM | edge |
| --- | --- | --- | --- | --- | --- |
| nearest | 0.519 | 0.567 | 999 (bit-exact) | 1.0000 | 26.24 |
| bilinear | 0.593 | 0.627 | 89.73 dB | 1.0000 | 13.47 |
| cubic (Catmull-Rom) | 1.200 | 1.204 | 103.11 dB | 1.0000 | 16.00 |
| lanczos2 | 1.755 | 1.756 | 93.05 dB | 1.0000 | 15.78 |
| fsr1 (EASU) | 1.611 | 1.613 | 84.10 dB | 1.0000 | 17.49 |
| fsr1rcas (EASU→RCAS) | 1.963 | 1.965 | 57.26 dB | 0.999996 | 18.34 |

**AMD Radeon Graphics (RADV RENOIR, iGPU)**

| mode | avg | p99 | PSNR vs CPU ref | SSIM | edge |
| --- | --- | --- | --- | --- | --- |
| nearest | 1.004 | 1.491 | 999 (bit-exact) | 1.0000 | 26.24 |
| bilinear | 1.046 | 1.390 | 89.73 dB | 1.0000 | 13.47 |
| cubic (Catmull-Rom) | 2.794 | 3.667 | 999 (bit-exact) | 1.0000 | 16.00 |
| lanczos2 | 4.617 | 6.001 | 94.03 dB | 1.0000 | 15.78 |
| fsr1 (EASU) | 4.061 | 5.684 | 84.10 dB | 1.0000 | 17.49 |
| fsr1rcas (EASU→RCAS) | 5.371 | 6.030 | 57.26 dB | 0.999996 | 18.34 |

PSNR 999 is the "identical" sentinel. Every other figure is the GPU and the CPU
reference performing the *same* arithmetic in a different order, so it measures
remaining floating-point slack rather than filter quality: 100+ dB is a handful
of one-level differences among millions of samples, and the 89.7 dB bilinear
figure is simply the 8-bit rounding boundary landing on the other side for a few
samples. FSR1 sits at 84 dB because it is the only mode whose reference is a
literal recompile of the same source rather than a parallel port.

`fsr1rcas` is lower (57 dB) and that is expected, not a defect: RCAS is a second
pass *on top of* EASU, so every one-level disagreement EASU leaves behind is
locally amplified by the sharpening term before it is quantised a second time.
57 dB is still ~12 dB above the gate, i.e. only a small fraction of samples
differ by one level. On this synthetic pattern (hard black/white edges) `edge`
barely moves, 17.49 → 18.34 — see the *Quality* table below, where RCAS works on
natural mid-contrast detail instead.

Read the timings as: **Lanczos-2 over a 1600×1000 → 2560×1600 upscale costs
~1.76 ms on the dGPU**, against an 8.33 ms budget at 120 Hz. FSR1 is a little
cheaper still (1.61 ms) for a better-looking result, and the full EASU→RCAS chain
adds ~0.35 ms (1.96 ms) — still under a quarter of the 120 Hz budget. The spatial
tiers are affordable. The iGPU is ~1.5–3× slower and bandwidth-bound (`nearest`
and `bilinear` cost about the same there; it has no PCIe hop but shares DRAM).

### Quality (the number that actually ranks the filters)

1600×1000 source (`~/Pictures/wallhaven-jeeq3q.png` — the exact conversion is in
*Run*) → area-box downsampled to 800×500 → upscaled back to 1600×1000 and
compared against the original. Both GPUs produce **identical** PSNR/SSIM/`edge`
here, which is a useful cross-driver correctness signal; only the timings differ.
The ground truth's own edge energy is **3.69** on the 0–255 scale.

| mode | avg, GTX 1650 | avg, RADV Renoir | PSNR vs original | SSIM | edge |
| --- | --- | --- | --- | --- | --- |
| nearest | 0.223 | 0.311 | 34.53 dB | 0.9817 | 3.60 |
| bilinear | 0.312 | 0.365 | 35.04 dB | 0.9833 | 1.38 |
| cubic (Catmull-Rom) | 0.790 | 0.977 | 36.33 dB | 0.9878 | 1.79 |
| lanczos2 | 1.084 | 1.536 | 36.37 dB | 0.9879 | 1.78 |
| fsr1 (EASU) | 0.823 | 1.151 | 35.87 dB | 0.9864 | 1.81 |
| fsr1rcas (EASU→RCAS) | 0.851 | 1.511 | 35.36 dB | 0.9852 | 4.23 |

This is where RCAS earns its place, and where PSNR stops being useful on its own.
Every smooth reconstruction kernel undershoots the ground truth's edge energy
badly — bilinear 1.38, cubic 1.79, Lanczos-2 1.78, EASU alone 1.81 — because over
a 2× upscale they can only *interpolate*; none of them can put back detail the
downsampler removed. `nearest` sits closest (3.60) but at the cost of visible
stair-stepping, and it has the worst PSNR of the six.

`fsr1rcas` at AMD's default strength (sharpness 0) pushes edge energy to 4.23,
past the ground truth, and pays for it in PSNR (35.36, the lowest). RCAS is a
sharpener, not a reconstruction kernel: it cannot recover detail either, it only
stops the result from looking soft. The setting is therefore a trade-off, and the
control behaves exactly as documented — each whole *stop* halves the lobe:

| sharpness (stops) | PSNR vs original | SSIM | edge |
| --- | --- | --- | --- |
| 0 (AMD default) | 35.36 dB | 0.9852 | 4.23 |
| 0.5 | 36.01 dB | 0.9870 | 2.74 |
| 1 | 36.02 dB | 0.9870 | 2.21 |
| 2 | 35.95 dB | 0.9867 | 1.89 |
| 4 | 35.87 dB | 0.9864 | 1.82 |
| (no RCAS — plain `fsr1`) | 35.87 dB | 0.9864 | 1.81 |

At 4 stops RCAS is effectively a no-op — PSNR and SSIM match plain `fsr1` to the
last digit, and edge energy to within 0.01 — an independent check that the
sharpening term really does vanish. On *this* image the
PSNR optimum is around 0.5–1 stop, which lands edge energy at 2.2–2.7, between
EASU's 1.81 and the ground truth's 3.69. The raw sweep is recorded in
`results/rcas-sharpness.txt`.

Two caveats on the numbers:

* **Run-to-run variance is large.** Across sessions at identical settings,
  `nearest` on the GTX 1650 has measured 0.223 ms, 0.365 ms, 0.511 ms and
  0.531 ms, and `cubic` 0.790 ms and 2.015 ms — the spread between runs is
  comparable to the difference between some filters. The iGPU's `p99` also sits
  ~10 % above its mean. Treat these as ballpark figures, and only compare modes
  measured within a single run.
* These are **dispatch-only** timings. Upload, download, and presentation are
  excluded, and in the real pipeline the frame must also be captured first.

### What PSNR here does *and does not* mean

It compares the GPU shader against the CPU reference implementation of the
**same** filter. It is a correctness gate — high values mean the shader computes
what the reference computes — **not** an image-quality ranking between filters.
The *Quality* table above is the one that ranks them.

## Correctness gates

* `nearest` must be bit-exact against the CPU reference (PSNR 999 sentinel).
* bilinear/cubic/Lanczos-2/FSR1 must stay above 45 dB vs the CPU reference.
  `fsr1rcas` is held to the same 45 dB gate; it measures 57 dB at 1600×1000, the
  extra spread coming from RCAS amplifying EASU's residual one-level differences.
* `lsl-upscale` exits non-zero if any mode fails, so this is CI-usable.
* Verified bit-exact for `nearest` at 2×, 2.5×, 1.6×, 1.33×, 8×, 3.33×,
  1:1, and several awkward ratios, plus downscaling.
* Same sweep for FSR1: 60–85 dB CPU-vs-GPU at 2×, 2.5×, 1.6×, 1.33×, 8×,
  3.33× and 0.5×, and **bit-exact at 1:1** — which independently confirms the
  gather path really does collapse to the identity mapping at unity scale.
* `fsr1rcas` across the same ratios: 51–68 dB CPU-vs-GPU (both passes in one
  gate), and also **bit-exact at 1:1** on the synthetic pattern. It is only the
  *engineered* ±1 behaviour of the reciprocal that keeps `lsl-tests` at the 18 dB
  1:1 gate rather than 90 dB.
* FSR1 is held to a *different* 1:1 rule: EASU is a sharpener with a negative
  lobe and a deringing clamp, so it deliberately does not reproduce its input
  unchanged. `fsr1rcas` inherits that and adds a quirk of its own: RCAS divides
  by AMD's "medium precision" reciprocal (`APrxMedRcpF1`), which is deliberately
  ~0.3 % low so it never shifts overall tonality — enough to move a flat patch by
  one level (255 → 254). Both modes therefore use an 18 dB 1:1 gate in
  `tests/test_reference.cpp`, not the 90 dB the exact scalers get. The invariants
  that *do* hold are pinned anyway: flat colour survives to within one level at
  any scale, a step edge keeps its position, and a flat region stays at exactly
  zero edge energy at every sharpness setting.
* Zero core-validation errors on both GPUs, for all six modes (see *Validation
  layers* below).

---

## FSR1, and why its reference is built differently

The `balanced` tier is AMD's FidelityFX Super Resolution 1: EASU upscaling
followed by RCAS sharpening. Two things about it are deliberate:

* **The filter is not re-derived.** `third_party/ffx/ffx_fsr1.h` is AMD's
  original MIT-licensed source, vendored unmodified (`third_party/ffx/license.txt`
  is kept alongside it). `shaders/scale.comp`, `shaders/rcas.comp` and
  `src/fsr1_ref.cpp` all compile that *same* file, so the CPU reference and the
  shaders cannot drift apart the way two hand-written ports can. What
  `fsr1_ref.cpp` supplies is only the small GLSL-flavoured vocabulary the header
  expects: the `AF*`/`AU*` vector types (including GLSL's `.rgb` aliases and the
  numeric `vec2(uvec2)` conversion), the `A*F1` scalar helpers, and the
  `textureGather`/buffer-load callbacks.
* **The scaling constants are computed on both sides by the same code.** The
  header's `FsrEasuCon()` (and, for the sharpening pass, `FsrRcasCon()`) runs
  inside the shader from push-constant sizes and inside the reference, so there
  is no place for a hand-copied constant to be wrong in only one of them.

One thing cannot travel verbatim: the header writes in/out parameters the GLSL
way (`inout AF3 aC`, `out AF3 pix`), and C++ has no macro spelling for that,
because the `&` has to follow the type. `tools/ffx_cpp_shim.py` therefore emits a
build-tree copy with exactly those qualifiers rewritten to C++ references, and
fails the build if the set it rewrites ever becomes empty or incomplete. The
vendored original is never touched, and it is the copy the shader includes.

The `balanced` tier runs **EASU followed by RCAS.** `shaders/rcas.comp` is a
second compute dispatch: EASU writes the upscaled image into a scratch buffer, a
`VkMemoryBarrier2` orders the two passes, and RCAS reads that scratch and writes
the final output. Both passes are inside the same GPU-timestamp pair, so the
`fsr1rcas` timings include both. RCAS compiles the same vendored header (its
constants come from `FsrRcasCon()`), so its CPU reference is not a parallel port
either. The one behaviour worth knowing about is that RCAS is *contrast-adaptive*:
it clamps how far a pixel may be pushed away from its neighbourhood, so it does
nothing at a full black/white step and nothing at all in a flat region. That is
why the sharpening shows up clearly on natural detail (edge energy 1.81 → 4.23 in
the *Quality* table) but barely moves the synthetic pattern used for the fidelity
run (17.49 → 18.34) — see the notes under *Correctness gates*.

---

## Bugs found and fixed during bring-up

Worth recording, because most of them produced *plausible-looking wrong numbers*
rather than errors:

1. **Pipeline layout had no descriptor set layout.** `createLayout()` never set
   `setLayoutCount`/`pSetLayouts`, so `vkCmdBindDescriptorSets(firstSet=0)` was
   out of range. NVIDIA executed the dispatch and quietly wrote nothing
   (output all black, timestamps still advancing); RADV segfaulted inside
   `vkCreateComputePipelines`. The fix is pairing `createSetLayout()` before
   `createLayout(pcr, setLayout)`.
2. **`VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` while re-submitting the same
   buffer 300 times.** Undefined behaviour; removed.
3. **Host-visible source/destination buffers.** The first working benchmark
   reported a flat ~3.3 ms for *every* mode on the GTX 1650 — it was measuring
   the PCIe hop, not the scaler. The timed dispatch now uses `DEVICE_LOCAL`
   storage with a one-shot staging upload and download.
4. **`nearest` was not bit-exact at non-integer ratios.** `floor((d + 0.5) *
   src / dst)` in float flips by one texel whenever the result is an exact
   integer and the GPU and CPU round differently — 640×400 → 1600×1000 came out
   at 29 dB instead of exact. The base texel index and fraction are now computed
   in pure integer arithmetic (`axisMap()`), identically in the shader and the
   CPU reference. This also stabilised the interpolation modes, which had the
   same discontinuity.
5. Catmull-Rom was mislabelled `B = C = 0.5`; true Catmull-Rom is `B = 0,
   C = 0.5`. Corrected in both the shader and the reference.
6. **`synchronization2` was used without ever being enabled.** All barriers,
   submissions and timestamps use the sync2 API (`vkCmdPipelineBarrier2`,
   `vkQueueSubmit2`, `vkCmdWriteTimestamp2`), which is invalid unless
   `VkPhysicalDeviceSynchronization2Features::synchronization2` is enabled at
   device creation — and it was not. Both RADV and the NVIDIA driver accept the
   calls anyway, so it worked by luck on this machine and would have been
   undefined behaviour elsewhere. Found only once the validation layers were
   running; fixed in `Context::Context`, which now queries support, enables the
   feature (adding `VK_KHR_synchronization2` when the device is below Vulkan
   1.3), and fails loudly if the device cannot provide it.
7. **The FSR1 CPU reference was computing different arithmetic from the GPU.**
   The most instructive one. `third_party/ffx/ffx_a.h` defines the shader's
   `APrxLoRcpF1`/`APrxLoRsqF1` as integer bit tricks (`0x7ef07ebb - bits(x)`,
   not `1/x`), and EASU feeds those results straight into its tap weights and
   edge-length terms. The reference used exact IEEE reciprocals, so the two
   implementations looked like the same filter while disagreeing by ~1e-3 in
   the weights — which cost **28 dB** of CPU-vs-GPU agreement at 2.5× (21.6 dB
   instead of ~55 dB). The tell was that both GPUs reported *byte-identical*
   21.58 dB: bit manipulation is exact integer maths, so it cannot vary between
   vendors, which meant the odd one out was the CPU. The helpers are now
   reproduced bit-for-bit from that section.
8. **FSR1's 8-bit quantiser truncated instead of rounding.** The shader's
   `pack()` clamps to [0,1], scales by 255 and calls `round()`; the new CPU path
   copied the hand-written scalers' shape but dropped the rounding, so every
   fractional value landed half a level low and fidelity capped out at ~55 dB.
   Both paths now round half away from zero, which lifted FSR1 from ~55 dB to
   over 80 dB.
9. **A preprocessor macro shadowed a vector field name.** The shim's
   `AF2_AU2(x)` expanded `(x).x`, and because the parameter was itself named `x`
   the preprocessor rewrote the field access too, producing `(a).a`. The
   compiler caught this one rather than a wrong number, but it is the kind of
   shorthand worth avoiding in a translation shim; the parameter is now `v`.
10. **A push constant silently never reached the shader.** `vkCmdPushConstants()`
    takes a *shader stage* in `stageFlags`, but `VK_PIPELINE_BIND_POINT_COMPUTE`
    happens to equal 1 — which is also `VK_SHADER_STAGE_VERTEX_BIT`. Passing the
    bind point compiled cleanly and looked harmless, yet the constant was dropped:
    `p.dstW` read back as 0, the shader's bounds guard returned immediately, and
    every mode wrote a black image (PSNR ~10.9, `edge` 0.00). Core validation
    flagged it (`VUID-vkCmdPushConstants-offset-01795`/`01796`); without the
    layers the only symptom was a plausible-looking black frame.
11. **The default mode list was hand-written and silently dropped a mode.**
    Adding `fsr1rcas` bumped `MODE_COUNT` to 6, but the CLI's list of modes to
    run when `--modes` is omitted was a separate literal `{…, MODE_FSR1}`. So
    every run *without* `--modes` — including the fidelity command this README
    shows — quietly ran five of six modes and printed "all modes passed", a
    clean-looking success that was missing the one mode the RCAS work went into.
    The explicit `--modes` runs used to build the tables did not hit it, which is
    why it survived bring-up. `allModes()` now derives the list from
    `MODE_COUNT`, so a new mode cannot be added without appearing.

Note on process: bugs 3, 4 and 7 were only caught because the numbers looked
*wrong* (a flat cost across four different filters; a 29 dB result where 999 was
expected; 21.58 dB where 55 was plausible). Bug 7 in particular is why the
per-mode CPU-vs-GPU gate exists at all: FSR1's quality score (35.87 dB, between
bilinear at 35.04 and cubic at 36.33) looked completely healthy while the
reference underneath it was quietly measuring a different function. Each fix was
verified by re-measuring, not assumed. Bugs 6 and 10 are the opposite case — the
code was *wrong but working*, and nothing short of a validation layer would have
revealed it.

### Validation layers

Core validation is installed for this user only, without root, and wired into
CTest (`tools/setup-validation-layers.sh`, `tools/validation-check.sh`). It
reports zero errors on both GPUs for all six modes, and on this increment it
earned its keep by catching bug 10 above.

Getting it to run at all took three separate traps, all recorded in the scripts:

* **`pacman` needs root**, so the official Arch package is downloaded and
  unpacked under `~/.local/opt` with a manifest in `~/.local/share`. Removal is
  `rm -rf` of those two directories.
* **A relative `library_path` in the manifest does not work.** The loader does
  *not* resolve it against the manifest's own directory; it searches
  `LD_LIBRARY_PATH` and the system paths, so the packaged relative path fails
  with `Requested layer "VK_LAYER_KHRONOS_validation" failed to load.` The
  setup script writes an absolute path. (The same mistake is why the
  system-installed `lsfg-vk` layer never loads on this host.)
* **A failed layer load is completely silent by default.** The program runs
  normally, validates nothing, and reports success — so early "clean" runs here
  were meaningless. `validation-check.sh` therefore forces `VK_LOADER_DEBUG=error`
  and, more importantly, compiles and runs
  `tools/validation-selftest.c`, which commits two deliberate spec violations
  and requires them to be reported before the real run is trusted. Without that
  the check would pass whether or not validation happened.

### Synchronization validation

Core validation, above, says nothing about missing barriers. That needs
synchronization validation, and sorting it out corrected an earlier note here.

**The switch was deprecated, not missing.**
`VK_LAYER_ENABLES=…SYNCHRONIZATION_VALIDATION_EXT` is the old spelling and makes
the layer warn when it is mixed with new-style settings; the current one is
`VK_LAYER_VALIDATE_SYNC=1`. With it, syncval is demonstrably live:
`tools/sync-selftest.c` records two `vkCmdFillBuffer` on the same buffer with no
barrier, and the layer reports

```
Validation Error: [ SYNC-HAZARD-WRITE-AFTER-WRITE ] …
vkCmdFillBuffer(): WRITE_AFTER_WRITE hazard detected.
```

and reports nothing for the same program with the variable unset. The earlier
claim that synchronization validation "could not be switched on" was wrong.

**It just does not check compute hazards.** On this layer build
(`vulkan-validation-layers` 1.4.363), sync validation reports transfer hazards
only. A minimal two-dispatch hazard — write-only shader, then read-only, no
barrier, the exact shape of the EASU → RCAS scratch dependency — prints nothing,
and neither does `lsl-upscale --hazard`, which omits that barrier. This is
upstream:

* [issue 6061](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/6061)
  — "synchronization validation of compute writes currently is not supported"
  (open since 2023);
* [issue 6096](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/6096)
  — `writeonly` / `NonReadable` descriptor bindings are not honoured yet (open).

`tools/sync-check.sh` therefore proves syncval is live *before* running anything,
runs the scaler under it, and prints what the result does not cover. `--hazard`
(test-only) drops the EASU → RCAS barrier for the day the layer can see it.
Until then those barriers rest on construction, the CPU/GPU equivalence gates,
and review — not on the layer.

### Still not verified

* Timestamps bracket the dispatch only — they exclude upload, download, and
  presentation. End-to-end frame cost will be higher.

---

## Next steps

1. **Phase 0 gate (manual, on a real game).** Confirm what the game actually
   renders at and which GPU Proton picks:
   `DXVK_HUD=devinfo,fps %command%` in the Steam launch options.
2. ~~Add a downscale→upscale quality harness~~ — **done**, see *Two different
   questions* above. It re-ranked the filters (on the current source: nearest
   34.53 dB, bilinear 35.04, cubic 36.33, Lanczos-2 36.37).
3. ~~FSR1 EASU~~ — **done**, see *FSR1* above: 84 dB CPU-vs-GPU fidelity,
   ~0.8 ms at 800×500 → 1600×1000, and **bit-exact at 1:1** in the ratio sweep.
4. ~~FSR1 RCAS~~ — **done**. The sharpening pass runs as a second dispatch behind
   a barrier, reads the reserved `sharpness` push-constant field, and compiles
   the same vendored header. As predicted, it made PSNR fight the feature, so the
   harness grew an `edge` (edge-energy) column and `tests/test_reference.cpp`
   grew RCAS-specific checks — flat stays flat, mid-contrast detail sharpens,
   higher stops only reduce it, and the chain equals `rcas(fsr1)` exactly. RCAS
   also surfaced a real behavioural quirk: AMD's deliberately-imprecise
   reciprocal moves a flat patch by up to one level. That is now documented
   rather than "fixed", because matching AMD is the point. Remaining work in this
   tier:
   * **NIS** as an alternative `balanced` implementation to compare against.
5. ~~Work out how to enable synchronization validation~~ — **done**, and it
   changed the answer: the setting is `VK_LAYER_VALIDATE_SYNC=1`, and this VVL
   does not report compute/shader-storage hazards (issues 6061, 6096), so Phase
   2 cannot lean on the layer to check its barriers.
6. Then move to the Phase 2 question: how a layer gets a compute pass between the
   game's render target and `vkQueuePresentKHR` without disturbing Hyprland.

## Layout

```
shaders/scale.comp        GLSL compute scaler (nearest/bilinear/cubic/lanczos2/EASU)
shaders/rcas.comp         GLSL compute sharpener — FSR1's RCAS, a second dispatch
shaders/                  compiled to SPIR-V at build time, embedded in the binary
third_party/ffx/          AMD ffx_a.h + ffx_fsr1.h (MIT), vendored unmodified
src/lsl_vk.*              Vulkan scaffolding
src/reference.*           CPU filters, box downsampler, PSNR/SSIM, edge energy
src/fsr1_ref.cpp          CPU FSR1 EASU + RCAS; compiles the vendored header via a shim
src/image_io.*            PPM/PGM I/O
src/probe.cpp             lsl-probe
src/upscale.cpp           lsl-upscale (fidelity + quality modes)
tests/test_reference.cpp  CPU reference tests
tools/embed_spv.py        SPIR-V -> C++ array
tools/ffx_cpp_shim.py     GLSL 'inout'/'out' params -> C++ references
tools/setup-validation-layers.sh   per-user validation layer install (no root)
tools/with-validation.sh          run a command with validation forced on
tools/validation-check.sh         CTest wrapper; fails if validation is not really running
tools/validation-selftest.c       deliberate violations used to prove the layer is active
tools/sync-check.sh               syncval wrapper: hazard self-test, then the real command
tools/sync-selftest.c             deliberate fill/fill hazard, proves syncval is active
results/scalers.csv       benchmark output (per-GPU)
results/rcas-sharpness.txt        RCAS sharpness sweep record
```

The full plan and phase breakdown live in `.opencode/plan/PLAN.md` and
`.opencode/plan/PHASE-1-PROTOTYPE.md`.
