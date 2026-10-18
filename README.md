# lsl — Lossless Scaling-style upscaler for Linux

A from-scratch Vulkan compute upscaler for games running under Steam/Proton on
Wayland (CachyOS/Arch + Hyprland). No code, binaries, or models are taken from
Lossless Scaling or from `lsfg-vk`.

**Current state: Phase 1, the Phase 2 layer, the Phase 3 GUI and its Phase 4
methods; packaging landed with it.** Phase 1 is the scaler core and its benchmark
— nine upscaling methods, from nearest neighbour through FSR1 + RCAS and NIS
sharpening to Anime4K's Restore CNN. Phase 2 is a Vulkan layer that makes a
running application render at a fraction of the surface size and upscales the
frame at present time with any of them, including `auto`, which picks one per
swapchain from the render size. Phase 3 is a GTK4 window that writes those
environment variables into a game's Steam launch options; `tools/install-layer.sh`
puts the layer where it can stay. What is still not built is frame generation
(and FSR2/3-style temporal upscaling, which needs motion vectors a present-time
layer never sees); on a real game the layer is injected by hand or through the
window.

---

## What is actually implemented

| Piece | State |
| --- | --- |
| `shaders/scale.comp` | nearest, bilinear, bicubic (Catmull-Rom), Lanczos-2, Lanczos-3, FSR1 EASU |
| `shaders/rcas.comp` | FSR1's RCAS sharpening pass — a second dispatch over an EASU scratch buffer |
| `shaders/nis.comp` | NVIDIA Image Scaling's sharpening pass, as a second dispatch (reimplementation, MIT sources vendored) |
| `shaders/cnn.comp` | Anime4K v4's Restore CNN, four float convolutions per pixel |
| `src/reference.{hpp,cpp}` | CPU implementations of the hand-written filters, area box downsampler, PSNR/SSIM, edge energy |
| `src/fsr1_ref.cpp` | CPU FSR1 EASU *and* RCAS — both compiled from AMD's own header, not re-derived |
| `src/nis_ref.{hpp,cpp}` | CPU NVSharpen, matching `shaders/nis.comp` |
| `src/cnn_ref.{hpp,cpp}` | CPU Anime4K Restore CNN |
| `third_party/ffx/` | AMD's MIT `ffx_a.h` + `ffx_fsr1.h`, vendored unmodified |
| `third_party/anime4k/` | Anime4K v4's MIT GLSL, vendored unmodified; weights baked by `tools/anime4k_weights.py` |
| `src/lsl_vk.{hpp,cpp}` | instance/device/compute-queue, device-local + staging buffers, pipelines, GPU-timestamp timing |
| `src/image_io.{hpp,cpp}` | PPM/PGM in, PPM out (no codec dependencies) |
| `src/probe.cpp` | `lsl-probe`: Vulkan device inventory + env traps |
| `src/upscale.cpp` | `lsl-upscale`: fidelity *and* quality benchmark |
| `tests/test_reference.cpp` | 111 CPU-reference checks |
| `layer/lsl_layer.c` | the Vulkan layer: swapchain capture, the size lie, present interception |
| `layer/lsl_upscale.c` | the in-layer present engine — every method, plus PPM dumps |
| `layer/lsl_method.{h,c}` | the layer's C copy of the method names, checked against `src/method.hpp` by `tools/upscale-check.sh` |
| Validation layers | installable per-user without root; wired into `ctest` |
| FSR1 `balanced` tier | EASU upscaling **and** RCAS sharpening, both landed |
| Frame capture / layer injection | **done** — `LSL_LAYER_CAPTURE`, `LSL_LAYER_SCALE` |
| Present-time upscaling | **done** — render at 1/scale, upscale back to full size |
| `src/steam.{hpp,cpp}` + `src/profile.{hpp,cpp}` | Steam discovery, in-place VDF edit, launch-options merge, per-game profiles |
| `tests/test_steam.cpp` | 156 checks over the above, no display needed |
| `gui/main.cpp` | `lsl-gui`: GTK4 window — game list, profile, launch options, launch button |
| `tools/resize-check.c` | WSI harness: swapchain recreated with `oldSwapchain`, resize storms |
| `tools/resize-check.sh` | layer gate: every recreate upscaled, extent-only rebuild, post-recreate dump matches the reference, zero validation messages |
| `tools/install-layer.sh` | installs the layer + manifest into a prefix (no root), prints the launch line |
| `tools/install-check.sh` | packaging gate: installs into a throwaway prefix and proves the installed copy loads — and that a bad `library_path` does not |
| Motion estimation / frame generation | **not implemented** |
| Packaging (a distributable layer + installer) | **done** — `cmake --install` + `tools/install-layer.sh` |

---

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

CTest runs eight tests: the 111 CPU reference checks (`reference`), 156 checks
over the Steam configuration core (`steam`), a `validation` test that re-runs the
scaler under the Khronos validation layers, `layer-upscale`, which loads the
layer into `vkcube` and checks its present-time result against the Phase 1
reference (see *Verifying the layer* below), `layer-resize`, which drives the
layer through recreated swapchains (see *Resizing* below), `layer-install`, which
installs the layer into a throwaway prefix and checks the *installed* copy loads
(see *Installing the layer* below), and two GUI selftests (headless
`gui-selftest`, and `gui-window-selftest`, which builds the window and skips
itself without a display). The validation-dependent halves **skip themselves**
unless the layers are installed:

```sh
tools/setup-validation-layers.sh     # per-user, no root, ~15 MB
```

Requires: a C++20 compiler, CMake ≥ 3.20, Ninja, `glslc`, Vulkan headers+loader,
Python 3, and `vkcube` (`vulkan-tools`) for the layer test; `gtkmm-4.0` for the
GUI, which is skipped if `pkg-config` cannot find it; `libplacebo` is
deliberately *not* a dependency.

---

## Phase 2 — the layer

`build/layer/liblsl-layer.so` does the whole thing inside a running application.
It is injected through the Steam launch options; the GUI writes these, and this
is the manual form:

```sh
VK_ADD_LAYER_PATH=/home/you/Code/lsl/build/layer \
VK_INSTANCE_LAYERS=VK_LAYER_LSL_frame \
LSL_LAYER_SCALE=2 %command%
```

Unset `LSL_LAYER_SCALE` (or set it to `1`) and the layer loads but does nothing,
so the rest of the line can stay while scaling is off.

| Variable | Meaning |
| --- | --- |
| `LSL_LAYER_SCALE` | upscale factor, `2` = render at half size. Unset/`≤1` = off. Fractional values work (`1.5`, `2.5`); the app is told `trunc(real/scale)` |
| `LSL_LAYER_SHARPNESS` | RCAS strength in stops, as in `lsl-upscale` (default 0 = AMD's setting). Only `fsr1rcas` reads it |
| `LSL_LAYER_MODE` | which method to run: `nearest`, `bilinear`, `cubic`, `lanczos2`, `lanczos3`, `fsr1`, `fsr1rcas`, `nis`, `cnn`, or `auto`. Unset/empty = `fsr1rcas`; an unknown name logs a warning and uses `fsr1rcas` |
| `LSL_LAYER_DUMP` | prefix; writes `<prefix>.src.ppm`, `.out.ppm`, `.easu.ppm` for the **first** upscaled frame |
| `LSL_LAYER_LOG` | where the layer's one-line-per-event log goes (nothing is logged without it) |
| `LSL_LAYER_CAPTURE` | `1` = the Phase-1-style readback path (hash per frame), slow by design |
| `LSL_LAYER_CAPTURE_FRAMES` | stop capturing after N frames (default 12, `0` = no limit) |
| `LSL_LAYER_BREAK_LAYOUT` | test-only: leave the image in the wrong layout, to prove validation is really looking at the layer |

### Installing the layer

The launch line above points into the build tree, which is fine while developing
and breaks the moment the tree moves. `tools/install-layer.sh` copies the shared
object and a manifest that names it **absolutely** into a prefix instead —
`~/.local` by default, no root:

```sh
./tools/install-layer.sh                  # into ~/.local
./tools/install-layer.sh --prefix /opt/lsl
```

It prints the exact launch line for that prefix, and `tools/uninstall-layer.sh`
removes it again. The layout is `<prefix>/lib/lsl/layer/` holding
`liblsl-layer.so` and `VkLayer_lsl_frame.json` — the same shape as `build/layer`,
so the GUI's directory check and `LSL_LAYER_DIR` accept either.

Two things are deliberate:

* **It is not installed where the loader scans automatically.** The manifest does
  not go under `vulkan/implicit_layer.d`, because the layer is enabled per game,
  not for every Vulkan application on the machine. The launch line already sets
  `VK_ADD_LAYER_PATH`, which is all a per-game layer needs.
* **The manifest is written at install time.** `library_path` has to be absolute
  — the loader does not resolve it against the manifest's own directory, the same
  trap that stops the packaged `lsfg-vk` layer from loading (see *Validation
  layers*) — and it has to name the *installed* object, not the build one. So the
  installed manifest is generated by `cmake --install`, after `--prefix` is
  known, and `tools/install-check.sh` fails if the path is relative, points at
  nothing, or the installed copy does not actually load.

### How the lie works

`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` is the only place an application
learns what size its surface is. The layer reports a smaller `currentExtent`
(with a matching `minImageExtent`, since the spec requires
`min ≤ current ≤ max`) and leaves `maxImageExtent` alone. The application then
builds a smaller swapchain and renders into it — but the swapchain that is
*created* is still the surface's real size, because the lie is only in what is
handed back to the app, not in what goes down to the driver.

The app's `WxH` image therefore lands in the top-left `WxH` corner of the real
`NxM` image. At present time the layer:

1. copies that corner into a device-local buffer,
2. runs the configured method's first pass from `WxH` to `NxM` into a scratch
   plane (EASU, Lanczos-3, nearest, …),
3. runs the method's refinement pass, if it has one (RCAS, NIS sharpening, or
   the CNN), into the destination plane,
4. copies the full image back over the presented image,
5. barriers the image to `PRESENT_SRC_KHR` and lets the present through.

There is no CPU wait on the present path: one queue, in-order submits, no fence
read. The only stall anywhere is the readback for `LSL_LAYER_DUMP`.

`maxImageExtent` is left alone on purpose: an app that ignores `currentExtent`
and asks for the full size just gets no upscaling rather than a broken
swapchain. Leaving the app's `oldSwapchain` path alone means a resize simply
re-lies and rebuilds.

**Native Wayland has no fixed `currentExtent`** (the driver reports
`UINT32_MAX`), so there is nothing to shrink on the first query. There the layer
lets the first swapchain through at full size, learns the surface size from it,
and lies from the second one on — so the first frame is full size and later ones
are not. XWayland (and X11 generally) names the size outright and is upscaled
from the first frame.

### Methods

`LSL_LAYER_MODE` names what every pass above actually is:

| Method | What it runs |
| --- | --- |
| `nearest` | nearest neighbour |
| `bilinear` | bilinear |
| `cubic` | bicubic (Catmull-Rom) |
| `lanczos2` | Lanczos-2, a 4-tap window |
| `lanczos3` | Lanczos-3, a wider 6-tap window |
| `fsr1` | FSR1's EASU alone |
| `fsr1rcas` | EASU, then FSR1's RCAS sharpening — the historical default |
| `nis` | EASU, then NVIDIA Image Scaling's sharpening |
| `cnn` | EASU, then Anime4K v4's Restore CNN, four float convolutions per pixel |

`nis` and `cnn` are reimplementations written for a storage-buffer pipeline, not
ports: NVIDIA's `NVScaler`/`NVSharpen` and Anime4K's shaders assume texture
sampling, so the arithmetic was re-derived and both sides — the layer's
`shaders/nis.comp`/`cnn.comp` and the CPU reference in `src/nis_ref.cpp` /
`src/cnn_ref.cpp` — are checked against each other byte for byte. The MIT sources
they are named after are vendored unmodified under `third_party/`.

`auto` is a policy, not a method: the layer resolves it once per swapchain, from
geometry alone (the frame it would have to inspect has already been copied by the
time it gets there), and `lsl-upscale` rejects it because a policy has no CPU
reference to compare against. The rule comes from measured cost and quality:

| method | cost at 1600×1000 → 2560×1600 | PSNR vs GT | edge (GT 3.69) |
| --- | --- | --- | --- |
| `fsr1rcas` | 1.97 ms | 35.36 dB | 4.23 |
| `nis` | 2.87 ms | 36.05 dB | 3.01 |
| `lanczos3` | 3.39 ms | 36.85 dB | 1.94 |
| `cnn` | 20.0 ms (≈5 ns/px) | 36.15 dB | 2.33 |

There is no free lunch to hand out, so `cnn` is the one that has to be rationed,
and by output size rather than by ratio:

* destination ≤ 1280×720 → `cnn`, where it costs about 5 ms and the app is
  rendering small enough to afford it;
* otherwise a scale ratio ≥ 1.5 → `nis`, a real downscale where sharpening
  recovers what is missing, and its milder sharpening is a better match than
  RCAS's maximum lobe (which overshoots the ground truth's edge energy);
* otherwise → `fsr1rcas`, the historical default.

The choice is per swapchain because a per-frame one would flicker as the picture
changed, and the layer's build log line says which method it settled on
(`… mode nis (auto)`).

### Formats

Only formats the shader family can read and write are used:
`R8G8B8A8_*`/`B8G8R8A8_*` (`_UNORM`/`_SRGB`, what XWayland usually gives) and
`R16G16B16A16_SFLOAT`, which pulls a second SPIR-V variant of every shader
compiled with `-DLSL_IO_16F`. The 16-bit variant is what Wayland's compositor
normally hands out. Anything else logs which format it saw and leaves the
application at full size. The shaders are compiled with
`--target-env=vulkan1.0`, because the layer loads into applications that may not
advertise anything newer.

### Verifying the layer

`tools/upscale-check.sh` turns "the picture looks sharper" into a number. It
runs `vkcube` with the layer loaded, and the layer dumps the source and the
output of the first upscaled frame. The dumped source is fed through the Phase 1
command-line upscaler — the same shaders, compiled from the same source — and
the two images are compared. It does this **once per method**: the list of names
comes from `lsl-upscale --list-modes` (that is, from `src/method.hpp`'s table),
so a method added there is exercised through the layer without anyone editing
the script, and the layer is required to have built the method it was asked for:

```
$ ./tools/upscale-check.sh build
self-test: one capture pass with a deliberate wrong layout, which validation must report
self-test: ok -- the validation layer sees the layer's commands
running: vkcube --wsi xcb --c 5, scale 2, mode nearest, validation layers
nearest: 635x385 -> 1271x771 as nearest, format 44, 8-bit pixels
comparing nearest against lsl-upscale --modes nearest:
  differing bytes  : 0/2939823
  max channel delta: 0
  PSNR             : inf
…
running: vkcube --wsi xcb --c 5, scale 2, mode auto, validation layers
auto: 635x385 -> 1271x771 as nis, format 44, 8-bit pixels
comparing auto against lsl-upscale --modes nis:
  differing bytes  : 0/2939823
  max channel delta: 0
  PSNR             : inf
running: vkcube --wsi xcb --c 5, scale 2, the default (no LSL_LAYER_MODE), validation layers
default: 635x385 -> 1271x771 as fsr1rcas, format 44, 8-bit pixels
…
validation: the layer's commands were validated, nothing reported
PASS: present-time upscale matches lsl-upscale for 11 method(s)
```

(The `vkcube` window size — and so which method `auto` picks, `cnn` here or
`nis` below the 1280×720 budget — depends on the compositor; the sizes above are
from one XWayland run.)

* 8-bit swapchains must match **byte for byte**. A BGRA swapchain reaches the
  shader with R and B exchanged relative to the dump's human-readable order, so
  the comparison is done in the shader's channel order.
* **Every method is checked, and the layer must agree on the name.** The layer
  keeps its own C copy of the vocabulary (`layer/lsl_method.{h,c}`, which cannot
  include the C++ `src/method.hpp`), and this is what keeps the two from
  drifting: a name the layer parses differently, or maps to the wrong filter,
  fails the byte match.
* **`auto` is checked too.** A policy has no CPU reference, so the method it
  resolved to is read back out of the layer's own build line (`… mode nis
  (auto)`) and that method's reference is used. The one run with `LSL_LAYER_MODE`
  unset exists to pin the historical default at `fsr1rcas`.
* 16-bit swapchains are gated on PSNR instead (55.2 dB on Wayland), because the
  dump is 8-bit and the reference therefore works from quantised input.
* When the validation layers are installed they are loaded *underneath* the
  injected layer for the same run, so the commands the layer records are
  validated too, and the pass requires both a matching image and zero validation
  messages. `LSL_CHECK_VVL=0` skips that half; `LSL_VVL_DIR` points at the
  manifest.
* Before the clean run, `LSL_LAYER_BREAK_LAYOUT=1` runs one short capture pass
  with the image deliberately left in the wrong layout at present. That must be
  reported (`VUID-VkPresentInfoKHR-pImageIndices-01430`); if it is not, a clean
  pass would only mean validation is not looking at the layer at all.
  `LSL_CHECK_SELFTEST=0` skips it. This is the same trick
  `tools/validation-check.sh` uses on the Phase 1 scaler, where the thing being
  asserted on cannot prove its own absence.

`LSL_CHECK_MODES=nis,cnn` checks a subset when iterating on one method.

The same script runs for the native Wayland path with `LSL_CHECK_WSI=wayland`.
`ctest` runs the XCB form as `layer-upscale`.

### Resizing

An engine does not just present a single swapchain. It also recreates it: it
calls `vkCreateSwapchainKHR` with the previous `oldSwapchain` and a new
`imageExtent`, and the old one is destroyed as soon as the new one exists. If
the layer frees its buffers while a dispatch submitted after the app's
submission is still reading them, that is a use-after-free -- exactly the
correctness class the pixel match cannot see.

`tools/resize-check.c` is a 150-line WSI harness for this: a hand-sized XCB
window that cycles through eight sizes, creating each new swapchain from the
previous one, keeping two frames in flight (with per-frame render-finished
semaphores so VVL's `VUID-vkQueueSubmit-pSignalSemaphores-00067` does not
trigger), `vkDeviceWaitIdle`ing before destroying a swapchain (for
`VUID-vkDestroySwapchainKHR-swapchain-01282`), and painting a fixed pattern so
the upscaler has pixels to work with. It prints `RESIZE_CHECK_OK` and exits 0 on
success.

`tools/resize-check.sh` then verifies four things:

1. **every swapchain created was upscaled.** The layer logs `- upscaling` for
   each successful `vkCreateSwapchainKHR` -- all eight are, so the lie and the
   rebuild survive every recreate;
2. **the extent-only path ran.** At least one line reads `resized to ... (pipelines kept)`, proving the engine rebuilt only the extent-sized buffers and did not
   tear down pipelines or descriptor sets;
3. **a frame dumped *after* a recreate is correct.** With `LSL_LAYER_DUMP` the
   layer dumps the first frame it upscales. The script asserts the dumped
   dimensions are those of a *recreated* swapchain (not the first), compares
   against `lsl-upscale --modes fsr1rcas` byte-for-byte for 8-bit, and swaps BGRA
   into the shader's channel order before comparing; and
4. **zero validation messages.** With the Khronos validation layer underneath
   the injected layer, the clean run reports nothing. As with `upscale-check.sh`,
   a deliberate `LSL_LAYER_CAPTURE=1` + `LSL_LAYER_BREAK_LAYOUT=1` run must
   report `VUID-VkPresentInfoKHR-pImageIndices-01430`, so silence means validation
   looked at the layer.

The resize test is also wired into `ctest` as `layer-resize` and skips (77)
without a display.

### Layer limits (prototype)

* **One device at a time.** The upscale engine holds one device: a swapchain on
  a second live device tears the engine down and rebuilds it there, so two
  devices presenting at once would thrash rather than corrupt (the layer logs
  the rebuild). The capture path and the engine both resolve their entry points
  per device, because in a two-device process the *last* device's objects are
  not interchangeable with the first's. (Two *instances* are normal — several
  implicit layers create their own.)
* **One present queue family.** If the present queue is not from the family the
  upscale runs on, the correct fix is a queue family ownership transfer, which
  the prototype does not do, so it declines instead.
* **Formats** are the three listed above; anything else disables scaling for
  that application.

### Three bugs this phase found with the validation layers underneath

All three were invisible without VVL below the layer, and they are recorded
because they are exactly the class of bug the Phase 1 gates could not see:

1. **The layer's own command buffers were invisible to validation.** The loader
   stamps every command buffer it hands the application with the device dispatch
   word (`loader_set_dispatch`), and VVL resolves a command buffer's device
   through that word. A layer that allocates its own through the next-chain
   entry point bypasses the loader's trampoline, so the word stays whatever the
   driver left there and VVL aborts with *"The VkDevice dispatch handle was not
   found"* — a crash, with zero VUIDs, before any of the layer's commands were
   even looked at. `lsl_stamp_command_buffers()` writes the same word the loader
   would have written (`*cb = *dev`).
2. **Two instances, one dispatch.** An instance-level call has to be dispatched
   through the instance its arguments came from. `pick_queue_family()` resolved
   `vkGetPhysicalDeviceQueueFamilyProperties`/`…SurfaceSupportKHR` through
   whichever instance was created last, which on this host is another implicit
   layer's — so the surface in the call belonged to a different instance than the
   command. Three validation errors (`…-commonparent`, `ExtensionNotEnabled`,
   and a "Couldn't find VkSurfaceKHR" from the loader), only on the Wayland path
   because that is the path where the second instance had a surface. The engine
   now resolves through the instance that owns the device's physical device
   (`lsl_pd_proc`).
3. **Two devices, one dispatch table.** The capture path kept one global
   function table and device handle — whichever device was created last — while
   using the *swapchain's* image and queue. With one device that is invisible;
   with two it is `VUID-vkCmdCopyImageToBuffer-commonparent` and friends. The
   upscale engine already kept a table per device for exactly this reason; the
   capture path, which predates it, did not. It now resolves per device, rebuilds
   its objects when the device changes, and `vkDestroyDevice` only tears down the
   objects of the device going away.

None of them is a wrong *picture* — they are exactly the kind of thing that
would have shipped as "works on my machine" without the validation layer sitting
in the right place.

---

## Phase 3 — the GUI

`lsl-gui` (GTK4) is a writer of one string. The layer's whole interface is
`VK_ADD_LAYER_PATH` plus `LSL_LAYER_SCALE`, `LSL_LAYER_SHARPNESS` and
`LSL_LAYER_MODE` in a game's launch options, so the window's job is to remember a
per-game profile and splice that line into `localconfig.vdf`:

```
./build/lsl-gui            # or --selftest / --selftest-window, see below
```

The split matters more than the window does. Everything that decides *what* to
write — Steam root discovery, library and manifest parsing, the in-place VDF
splice, the launch-options merge — lives in `src/steam.cpp` and `src/profile.cpp`
with no GTK anywhere in sight, and is covered by `tests/test_steam.cpp` (156
checks). The window is a thin shell over that, which is why it is also the least
tested part: what can be checked without a human is checked by
`lsl-gui --selftest`, which needs no display and puts *every game in the real
Steam installation* through an apply-then-clear round trip, comparing the result
with clearing the original. That is the check that matters, because the failure
mode is not a crash: it is a user's `DXVK_HUD` or `%command%` quietly going
missing. `--selftest-window` does the same and then builds the window on that
data (skipping, exit 77, with no display) and prints the widget tree, so the
layout can be reviewed from a log.

Both are ctest tests (`gui-selftest`, `gui-window-selftest`), and the GUI target
itself is optional: it is built only when `pkg-config` finds `gtkmm-4.0`, so the
layer, the tools and the tests still build on a machine without GTK.

A profile is a scale, a sharpness and a method. The method dropdown is built
from `src/method.hpp`'s table — `auto` first, then every method name — so the
strings in the window and the strings `LSL_LAYER_MODE` accepts come from one
place; a game that has never been configured gets `auto`. Sharpness is disabled
for any method but `fsr1rcas`, because that is the only one that reads it.

Three properties are deliberate:

* **The launch options are shown before they are written, in full.** A layer's
  point is that the game is untouched; the same should hold for the user's
  configuration, so there is no hidden checkbox that silently rewrites it.
* **After a write the line is re-read from the file.** The "In Steam right now"
  box shows what Steam will actually do, not what lsl intended.
* **Steam is never guessed about.** If Steam is running, the window says so —
  Steam rewrites `localconfig.vdf` when it exits, so a write made while it runs
  is likely to be lost — and the launch button warns when a profile has not been
  applied yet.

Two known limits:

* **Valve's tools look exactly like games** in everything lsl can read:
  `appmanifest_*.acf` and `libraryfolders.vdf` are the same shape for Proton and
  for a game, `appinfo.vdf` stores its records as opaque hashed blobs in current
  Steam, and the store metadata in Steam's own cache is absent for a game that
  has never been launched — so using it would hide real games. The window hides
  Proton / Steam Linux Runtime / the redistributables by *name*
  (`lsl::steamLooksLikeTool`, `"Hide Valve's tools"`, on by default, off with one
  click), which on this host removes 8 of the 18 installed apps.
* **One Steam user.** Steam's per-user configuration is edited for the user
  directory that `localconfig.vdf`'s owner logic picks (the most recently used),
  not for an account the window lets you choose.

---

## Run: the Phase 1 tools

# fidelity, self-contained: synthetic 1600x1000 -> 2560x1600, writes the CSV
./build/lsl-upscale --make-test 1600x1000 --dst 2560x1600 \
                    --bench 300 --csv results/scalers.csv --out out.ppm

./build/lsl-upscale --in shot.ppm --dst 2560x1600 --modes fsr1,fsr1rcas

# quality needs a real image; the table below uses exactly this conversion
ffmpeg -i ~/Pictures/wallhaven-jeeq3q.png -vf scale=1600:1000:flags=lanczos \
       -pix_fmt rgb24 /tmp/photo.ppm
./build/lsl-upscale --in /tmp/photo.ppm --render 800x500 --bench 100  # all nine modes

./build/lsl-upscale --in /tmp/photo.ppm --render 800x500 \
                    --modes fsr1rcas --sharpness 1   # RCAS strength in stops

./build/lsl-upscale --list-modes                     # the names --modes takes
```

`--device N` forces a physical device index; otherwise a discrete GPU is
preferred. `--sharpness N` (RCAS stops) is only read by `fsr1rcas`, and `--modes
auto` is rejected — `auto` is a run-time policy in the layer, not a filter with a
CPU reference to score.

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
nearest      0.5369 ms      999.00  1.0000
bilinear     0.6068 ms       89.73  1.0000
cubic        1.1962 ms      103.11  1.0000
lanczos2     1.7475 ms       93.05  1.0000
fsr1         1.6165 ms       84.10  1.0000
fsr1rcas     1.9674 ms       57.26  1.0000
lanczos3     3.3897 ms      109.03  1.0000
nis          2.8694 ms       81.63  1.0000
cnn         20.0350 ms       79.41  1.0000

mode      dispatch_avg   psnr_vs_gt  ssim  edge    # quality (vs original)
nearest      0.2246 ms       34.53  0.9817  3.60
bilinear     0.3056 ms       35.04  0.9833  1.38
cubic        0.7909 ms       36.33  0.9878  1.79
lanczos2     1.1017 ms       36.37  0.9879  1.78
fsr1         0.8371 ms       35.87  0.9864  1.81
fsr1rcas     0.8654 ms       35.36  0.9852  4.23
lanczos3     1.3283 ms       36.85  0.9893  1.94
nis          0.8616 ms       36.05  0.9871  3.01
cnn          7.7980 ms       36.15  0.9875  2.33
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
| nearest | 0.537 | 0.567 | 999 (bit-exact) | 1.0000 | 26.24 |
| bilinear | 0.607 | 0.632 | 89.73 dB | 1.0000 | 13.47 |
| cubic (Catmull-Rom) | 1.196 | 1.204 | 103.11 dB | 1.0000 | 16.00 |
| lanczos2 | 1.748 | 1.749 | 93.05 dB | 1.0000 | 15.78 |
| fsr1 (EASU) | 1.617 | 1.621 | 84.10 dB | 1.0000 | 17.49 |
| fsr1rcas (EASU→RCAS) | 1.967 | 1.969 | 57.26 dB | 0.999996 | 18.34 |
| lanczos3 | 3.390 | 3.415 | 109.03 dB | 1.0000 | 18.23 |
| nis (EASU→NIS sharpen) | 2.869 | 11.209 | 81.63 dB | 1.0000 | 21.19 |
| cnn (EASU→Anime4K CNN) | 20.035 | 20.109 | 79.41 dB | 1.0000 | 23.07 |

**AMD Radeon Graphics (RADV RENOIR, iGPU)**

| mode | avg | p99 | PSNR vs CPU ref | SSIM | edge |
| --- | --- | --- | --- | --- | --- |
| nearest | 0.809 | 0.906 | 999 (bit-exact) | 1.0000 | 26.24 |
| bilinear | 0.965 | 1.363 | 89.73 dB | 1.0000 | 13.47 |
| cubic (Catmull-Rom) | 2.470 | 2.500 | 999 (bit-exact) | 1.0000 | 16.00 |
| lanczos2 | 3.877 | 3.892 | 94.03 dB | 1.0000 | 15.78 |
| fsr1 (EASU) | 2.867 | 2.869 | 84.10 dB | 1.0000 | 17.49 |
| fsr1rcas (EASU→RCAS) | 3.838 | 3.983 | 57.26 dB | 0.999996 | 18.34 |
| lanczos3 | 7.616 | 7.634 | 108.23 dB | 1.0000 | 18.23 |
| nis (EASU→NIS sharpen) | 4.183 | 4.229 | 81.61 dB | 1.0000 | 21.19 |
| cnn (EASU→Anime4K CNN) | 18.129 | 18.794 | 79.39 dB | 1.0000 | 23.07 |

PSNR 999 is the "identical" sentinel. Every other figure is the GPU and the CPU
reference performing the *same* arithmetic in a different order, so it measures
remaining floating-point slack rather than filter quality: 100+ dB is a handful
of one-level differences among millions of samples, and the 89.7 dB bilinear
figure is simply the 8-bit rounding boundary landing on the other side for a few
samples. FSR1 sits at 84 dB because it is the only *spatial* mode whose reference
is a literal recompile of the same source rather than a parallel port, and the
hand-written ports sit in the same band: `nis` at 81.6 dB, `cnn` at 79.4 dB.

`fsr1rcas` is lower (57 dB) and that is expected, not a defect: RCAS is a second
pass *on top of* EASU, so every one-level disagreement EASU leaves behind is
locally amplified by the sharpening term before it is quantised a second time.
57 dB is still ~12 dB above the gate, i.e. only a small fraction of samples
differ by one level. On this synthetic pattern (hard black/white edges) `edge`
barely moves, 17.49 → 18.34 — see the *Quality* table below, where RCAS works on
natural mid-contrast detail instead.

Read the timings as: **Lanczos-2 over a 1600×1000 → 2560×1600 upscale costs
~1.75 ms on the dGPU**, against an 8.33 ms budget at 120 Hz. FSR1 is a little
cheaper still (1.62 ms) for a better-looking result, and the full EASU→RCAS chain
adds ~0.35 ms (1.97 ms) — still under a quarter of the 120 Hz budget. The spatial
tiers are affordable. The two heavyweights are not free: `nis` adds a second
full-resolution dispatch (2.87 ms) and `cnn` four float convolutions per pixel
(20.0 ms), which is why `auto` only reaches for `cnn` on a small destination. The
iGPU is ~1.5–3× slower and bandwidth-bound (`nearest` and `bilinear` cost about
the same there; it has no PCIe hop but shares DRAM).

### Quality (the number that actually ranks the filters)

1600×1000 source (`~/Pictures/wallhaven-jeeq3q.png` — the exact conversion is in
*Run*) → area-box downsampled to 800×500 → upscaled back to 1600×1000 and
compared against the original. Both GPUs produce **identical** PSNR/SSIM/`edge`
here, which is a useful cross-driver correctness signal; only the timings differ.
The ground truth's own edge energy is **3.69** on the 0–255 scale.

| mode | avg, GTX 1650 | avg, RADV Renoir | PSNR vs original | SSIM | edge |
| --- | --- | --- | --- | --- | --- |
| nearest | 0.225 | 0.307 | 34.53 dB | 0.9817 | 3.60 |
| bilinear | 0.306 | 0.355 | 35.04 dB | 0.9833 | 1.38 |
| cubic (Catmull-Rom) | 0.791 | 0.969 | 36.33 dB | 0.9878 | 1.79 |
| lanczos2 | 1.102 | 1.527 | 36.37 dB | 0.9879 | 1.78 |
| fsr1 (EASU) | 0.837 | 1.129 | 35.87 dB | 0.9864 | 1.81 |
| fsr1rcas (EASU→RCAS) | 0.865 | 1.504 | 35.36 dB | 0.9852 | 4.23 |
| lanczos3 | 1.328 | 2.988 | 36.85 dB | 0.9893 | 1.94 |
| nis (EASU→NIS sharpen) | 0.862 | 1.653 | 36.05 dB | 0.9871 | 3.01 |
| cnn (EASU→Anime4K CNN) | 7.798 | 6.880 | 36.15 dB | 0.9875 | 2.33 |

This is where RCAS earns its place, and where PSNR stops being useful on its own.
Every smooth reconstruction kernel undershoots the ground truth's edge energy
badly — bilinear 1.38, cubic 1.79, Lanczos-2 1.78, Lanczos-3 1.94, EASU alone
1.81 — because over a 2× upscale they can only *interpolate*; none of them can
put back detail the downsampler removed. `nearest` sits closest (3.60) but at the
cost of visible stair-stepping, and it has the worst PSNR of the nine.

`fsr1rcas` at AMD's default strength (sharpness 0) pushes edge energy to 4.23,
past the ground truth, and pays for it in PSNR (35.36, the lowest). RCAS is a
sharpener, not a reconstruction kernel: it cannot recover detail either, it only
stops the result from looking soft. `nis` and `cnn` sit between: `nis` sharpens
to 3.01, closer to the ground truth's 3.69 than RCAS and with 0.7 dB more PSNR,
and `cnn` reconstructs to 2.33 while also scoring 36.15 dB — the only method that
both retains some sharpening and lands above every plain interpolator. `lanczos3`
tops PSNR (36.85) but leaves edges soft (1.94). The setting is therefore a
trade-off, and the control behaves exactly as documented — each whole *stop*
halves the lobe:

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
* bilinear/cubic/Lanczos-2/Lanczos-3/FSR1/NIS/CNN must stay above 45 dB vs the CPU
  reference. `fsr1rcas` is held to the same 45 dB gate; it measures 57 dB at
  1600×1000, the extra spread coming from RCAS amplifying EASU's residual
  one-level differences.
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
* Zero core-validation errors on both GPUs, for all nine modes (see *Validation
  layers* below).
* `lsl-gui --selftest` puts every installed game's launch options through
  apply-then-clear against the real `localconfig.vdf` and requires the round trip
  to equal clearing the original — the failure it guards against is a dropped
  `DXVK_HUD` or `%command%`, not a crash. `--selftest-window` adds the window and
  skips without a display. Nothing in either mode writes to Steam's files.

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
reports zero errors on both GPUs for all nine modes, and on this increment it
earned its keep by catching bug 10 above.

It also validates the layer's own commands, as long as it is loaded
*underneath* the injected layer rather than over it: the injected layer has to
be closest to the application for the validation layer to see what it records.
`tools/upscale-check.sh` runs that configuration and requires zero messages
alongside the byte-exact image comparison; it found the three Phase 2 bugs
listed earlier. Before the clean pass it runs `LSL_LAYER_BREAK_LAYOUT=1` once —
the layout left wrong at present, which must be reported as
`VUID-VkPresentInfoKHR-pImageIndices-01430` — so "the layer is clean under
validation" cannot be confused with "validation never looked at the layer".

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
   * ~~**NIS** as an alternative `balanced` implementation to compare against.~~
     — **done**, along with `lanczos3`, `cnn` and the `auto` policy; see
     *Methods* above.
5. ~~Work out how to enable synchronization validation~~ — **done**, and it
   changed the answer: the setting is `VK_LAYER_VALIDATE_SYNC=1`, and this VVL
   does not report compute/shader-storage hazards (issues 6061, 6096), so Phase
   2 cannot lean on the layer to check its barriers.
6. ~~How does a layer get a compute pass between a game's render target and
   `vkQueuePresentKHR`?~~ — **answered and built**, see *Phase 2* above: the
   surface-size lie, a real-size swapchain, and EASU→RCAS on the present queue,
   byte-exact against the Phase 1 reference and clean under validation on both
   XWayland and native Wayland.
7. **Next.** In rough order:
   * **Phase 0 gate, still manual.** Run a real game once with
     `DXVK_HUD=devinfo,fps %command%` plus the layer line, and confirm it renders
     small, upscales, and picks the GPU Proton expects. Everything so far is
     `vkcube`; a Proton process is the point of the exercise.
   * **The swapchain under stress.** `oldSwapchain` reuse, resize storms,
     `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` handling, and what happens
     when the number of images changes under the engine. The benches here are
     single-window and never resize.
   * ~~**The GUI** (GTK4)~~ — **done**, see *Phase 3* above: game list from
     `appmanifest_*.acf`, a per-game profile, the launch-options line written
     into `localconfig.vdf`, and a launch button. Its packaging half is also
     done — see *Installing the layer*.
   * ~~**More methods, and `auto`**~~ — **done**, see *Methods* above: `lanczos3`,
     `nis` and Anime4K v4's Restore CNN (`cnn`), selected by `LSL_LAYER_MODE`,
     with `auto` resolving from geometry. Licensing matches the existing
     pattern: `third_party/ffx` and `third_party/anime4k` are both MIT and
     vendored unmodified, and the layer has no other third-party code.
   * ~~**Packaging**~~ — **done**, see *Installing the layer*: `cmake --install`
     plus `tools/install-layer.sh` put the layer and an absolute-path manifest
     into a prefix, and `layer-install` loads the installed copy to prove it.
     Still open here: shipping a built artifact for machines that will not build
     the tree at all.

## Layout

```
layer/lsl_layer.c         the layer: the size lie, swapchain capture, present interception
layer/lsl_upscale.c       the in-layer present engine: every method, auto, PPM dumps
layer/lsl_layer.h         shared layer/engine state, per-device function tables
layer/lsl_method.{h,c}    the layer's C copy of the method vocabulary (src/method.hpp is C++)
layer/VkLayer_lsl_frame.json  layer manifest, loaded per game
shaders/scale.comp        GLSL compute scaler (nearest/bilinear/cubic/lanczos2/lanczos3/EASU)
shaders/rcas.comp         GLSL compute sharpener — FSR1's RCAS, a second dispatch
shaders/nis.comp          GLSL compute sharpener — NIS's, written for the buffer model
shaders/cnn.comp          GLSL compute — Anime4K v4's Restore CNN, four float passes
shaders/                  compiled to SPIR-V at build time, embedded in the binary
third_party/ffx/          AMD ffx_a.h + ffx_fsr1.h (MIT), vendored unmodified
third_party/anime4k/      Anime4K v4 GLSL + MIT license text, vendored unmodified
src/method.hpp            the C++ method table: ids, names, labels, `auto`
src/lsl_vk.*              Vulkan scaffolding
gui/main.cpp             lsl-gui: the GTK4 window (optional target)
src/steam.{hpp,cpp}       Steam discovery, in-place VDF edit, launch-options merge
src/profile.{hpp,cpp}     the per-game profile store (plain text, atomic writes)
tests/test_steam.cpp      the above, tested without a display
src/reference.*           CPU filters, box downsampler, PSNR/SSIM, edge energy
src/fsr1_ref.cpp          CPU FSR1 EASU + RCAS; compiles the vendored header via a shim
src/nis_ref.*             CPU reference for the NIS sharpening pass
src/cnn_ref.*             CPU reference for the Anime4K Restore CNN
src/image_io.*            PPM/PGM I/O
src/probe.cpp             lsl-probe
src/upscale.cpp           lsl-upscale (fidelity + quality modes, `--list-modes`)
tests/test_reference.cpp  CPU reference tests
tools/embed_spv.py        SPIR-V -> C++ array
tools/ffx_cpp_shim.py     GLSL 'inout'/'out' params -> C++ references
tools/anime4k_weights.py  bake Anime4K's weights into a header the layer and CLI share
tools/upscale-check.sh            layer gate: every method, pixel match + validation + self-test
tools/ppm_tool.py                 PPM compare (bytes / max delta / PSNR) and swap
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
