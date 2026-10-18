cmake_minimum_required(VERSION 3.20)
# C for the layer (it must load into arbitrary processes, so no C++ runtime
# dependency), C++ for everything else.
project(lsl LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ---------------------------------------------------------------------------
# GLSL -> SPIR-V, then embed the bytes so binaries have no runtime data dep
# ---------------------------------------------------------------------------
find_program(GLSLC glslc REQUIRED)

set(LSL_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
set(LSL_SPV)
# FSR1's EASU filter is AMD's MIT-licensed ffx_fsr1.h, included by the shader as
# well as by the CPU reference so both compile the same algorithm.
set(LSL_FFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx)
# scale.comp holds the spatial scalers plus FSR1's EASU; rcas.comp is FSR1's
# sharpening pass, run as a second dispatch over a scratch buffer; nis.comp is
# the same idea with NVIDIA Image Scaling's sharpening; cnn.comp is the Anime4K
# Restore CNN, a refinement pass made of four convolutions.
foreach(NAME scale rcas nis cnn)
  set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME}.comp)
  set(SPV ${LSL_SPV_DIR}/${NAME}.comp.spv)
  add_custom_command(
    OUTPUT ${SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LSL_SPV_DIR}
    COMMAND ${GLSLC} --target-env=vulkan1.0 -O -I ${LSL_FFX_DIR} -o ${SPV} ${SRC}
    DEPENDS ${SRC} ${LSL_FFX_DIR}/ffx_a.h ${LSL_FFX_DIR}/ffx_fsr1.h
    COMMENT "SPIR-V: ${NAME}.comp")
  list(APPEND LSL_SPV ${SPV})

  # Same source, half-float pixel I/O. A VK_FORMAT_R16G16B16A16_SFLOAT
  # swapchain -- which is what vkcube gets on this host's Wayland session, and
  # what an HDR game would get -- stores four half floats per pixel rather than
  # a packed RGBA8 word, so the layer needs both flavours. Only load/store
  # differs; every filter is the same code.
  set(SPV16 ${LSL_SPV_DIR}/${NAME}16.comp.spv)
  add_custom_command(
    OUTPUT ${SPV16}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LSL_SPV_DIR}
    COMMAND ${GLSLC} --target-env=vulkan1.0 -O -DLSL_IO_16F -I ${LSL_FFX_DIR} -o ${SPV16} ${SRC}
    DEPENDS ${SRC} ${LSL_FFX_DIR}/ffx_a.h ${LSL_FFX_DIR}/ffx_fsr1.h
    COMMENT "SPIR-V: ${NAME}.comp (16F I/O)")
  list(APPEND LSL_SPV ${SPV16})
endforeach()

set(LSL_BLOB ${CMAKE_CURRENT_BINARY_DIR}/spv_blob.cpp)
add_custom_command(
  OUTPUT ${LSL_BLOB}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
          ${LSL_SPV_DIR} ${LSL_BLOB} ${CMAKE_CURRENT_BINARY_DIR}/spv_blob.hpp
  DEPENDS ${LSL_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
  COMMENT "embedding SPIR-V blobs")

# The C++ blob above is what lsl-upscale and lsl-tests link. The layer is C and
# must not drag in a C++ runtime, so it gets the same blobs through a C header.
set(LSL_BLOB_C ${CMAKE_CURRENT_BINARY_DIR}/spv_blob_c.c)
set(LSL_BLOB_C_H ${CMAKE_CURRENT_BINARY_DIR}/spv_blob_c.h)
add_custom_command(
  OUTPUT ${LSL_BLOB_C} ${LSL_BLOB_C_H}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
          --lang c ${LSL_SPV_DIR} ${LSL_BLOB_C} ${LSL_BLOB_C_H}
  DEPENDS ${LSL_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
  COMMENT "embedding SPIR-V blobs (C)")

add_custom_target(lsl_shaders DEPENDS ${LSL_SPV} ${LSL_BLOB})
add_custom_target(lsl_shaders_c DEPENDS ${LSL_BLOB_C})

# ---------------------------------------------------------------------------
# FSR1 reference header
#
# The FSR1 CPU reference (src/fsr1_ref.cpp) compiles AMD's own EASU source, so
# that the reference and the shader cannot drift apart. C++ needs one spelling
# fix in that header (see tools/ffx_cpp_shim.py); the vendored original stays
# untouched and is the copy the shader includes.
# ---------------------------------------------------------------------------
set(LSL_FFX_CPP_DIR ${CMAKE_CURRENT_BINARY_DIR}/ffx_cpp)
set(LSL_FFX_CPP ${LSL_FFX_CPP_DIR}/ffx_fsr1_cpp.h)
add_custom_command(
  OUTPUT ${LSL_FFX_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${LSL_FFX_CPP_DIR}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/ffx_cpp_shim.py
          ${LSL_FFX_DIR}/ffx_fsr1.h ${LSL_FFX_CPP}
  DEPENDS ${LSL_FFX_DIR}/ffx_fsr1.h ${CMAKE_CURRENT_SOURCE_DIR}/tools/ffx_cpp_shim.py
  COMMENT "preparing ffx_fsr1_cpp.h")
add_custom_target(lsl_ffx_cpp DEPENDS ${LSL_FFX_CPP})

# ---------------------------------------------------------------------------
# Anime4K Restore CNN weights
#
# The `cnn` method's four convolutions come from bloc97's MIT-licensed GLSL,
# vendored in third_party/anime4k.  The weight tables are generated from that
# file (and validated: the generator refuses anything whose taps are not in the
# one order the shader and the CPU reference walk), so the vendored source is
# the only copy of the numbers anyone edits.
# ---------------------------------------------------------------------------
set(LSL_ANIME4K_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/anime4k/Anime4K_Restore_CNN_S.glsl)
set(LSL_ANIME4K_DIR ${CMAKE_CURRENT_BINARY_DIR}/anime4k)
set(LSL_ANIME4K_H ${LSL_ANIME4K_DIR}/anime4k_restore_s.h)
add_custom_command(
  OUTPUT ${LSL_ANIME4K_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${LSL_ANIME4K_DIR}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/anime4k_weights.py
          ${LSL_ANIME4K_GLSL} ${LSL_ANIME4K_H}
  DEPENDS ${LSL_ANIME4K_GLSL} ${CMAKE_CURRENT_SOURCE_DIR}/tools/anime4k_weights.py
  COMMENT "generating anime4k_restore_s.h")
add_custom_target(lsl_anime4k_weights DEPENDS ${LSL_ANIME4K_H})

# ---------------------------------------------------------------------------
# core library
# ---------------------------------------------------------------------------
add_library(lsl_core STATIC
  src/lsl_vk.cpp
  src/image_io.cpp
  src/reference.cpp
  src/fsr1_ref.cpp
  src/nis_ref.cpp
  src/cnn_ref.cpp
  ${LSL_BLOB})
target_include_directories(lsl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR} ${LSL_ANIME4K_DIR})
target_include_directories(lsl_core PRIVATE ${LSL_FFX_DIR} ${LSL_FFX_CPP_DIR})
target_link_libraries(lsl_core PUBLIC Vulkan::Vulkan)
target_compile_options(lsl_core PRIVATE -Wall -Wextra)
# The FSR1 reference has to reproduce the GPU's floating-point results, and
# glslc emits separate multiplies and adds where GCC would by default contract
# a*b+c into a fused multiply-add. Turning contraction off for this one file
# keeps the two sides doing the same operations.
set_source_files_properties(src/fsr1_ref.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
add_dependencies(lsl_core lsl_shaders lsl_ffx_cpp lsl_anime4k_weights)

# ---------------------------------------------------------------------------
# Steam configuration (the GUI's core)
#
# No Vulkan and no GTK: this is the part that decides what to write into
# Steam's own files, so it is kept separate and tested on its own.
# ---------------------------------------------------------------------------
add_library(lsl_steam STATIC src/steam.cpp src/profile.cpp)
target_include_directories(lsl_steam PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(lsl_steam PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# executables
# ---------------------------------------------------------------------------
add_executable(lsl-probe   src/probe.cpp)
target_link_libraries(lsl-probe PRIVATE lsl_core)

add_executable(lsl-upscale src/upscale.cpp)
target_link_libraries(lsl-upscale PRIVATE lsl_core)

add_executable(lsl-tests   tests/test_reference.cpp)
target_link_libraries(lsl-tests PRIVATE lsl_core)

add_executable(lsl-steam-tests tests/test_steam.cpp)
target_link_libraries(lsl-steam-tests PRIVATE lsl_steam)

# The layer's two configuration files, built and run as a normal program rather
# than through the shared object, so a failure names the function at fault.  The
# layer is C, so this is: it links the sources directly.
add_executable(lsl-layer-tests
  tests/test_layer_profile.c
  layer/lsl_config.c
  layer/lsl_profile.c
  layer/lsl_method.c)
target_include_directories(lsl-layer-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/layer)
target_compile_options(lsl-layer-tests PRIVATE -Wall -Wextra)

# The seam between the two implementations of the profile store: the GUI's C++
# writer and the layer's C reader.  This one links both, so it is C++ rather
# than C, and it is the only test that puts "the window saved this" next to "the
# layer read this".
add_executable(lsl-layer-store-tests
  tests/test_layer_store.cpp
  layer/lsl_config.c
  layer/lsl_profile.c
  layer/lsl_method.c)
target_include_directories(lsl-layer-store-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/layer)
target_link_libraries(lsl-layer-store-tests PRIVATE lsl_steam)
target_compile_options(lsl-layer-store-tests PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# GUI (optional)
#
# The window is the last and least interesting part: everything it does is
# "call into lsl_steam and show the result".  gtkmm-4.0 is the only dependency
# lsl has that is not already needed to build the layer, so it is optional --
# the layer, the tools and the tests build without it, and a machine with only
# the command line still works.
# ---------------------------------------------------------------------------
include(FindPkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(GTKMM4 QUIET IMPORTED_TARGET gtkmm-4.0)
endif()
if(GTKMM4_FOUND)
  add_executable(lsl-gui gui/main.cpp)
  target_link_libraries(lsl-gui PRIVATE lsl_steam PkgConfig::GTKMM4)
  target_compile_options(lsl-gui PRIVATE -Wall -Wextra)
else()
  message(STATUS
          "gtkmm-4.0 not found -- building without the GUI (lsl-gui)")
endif()

# ---------------------------------------------------------------------------
# Phase 2: the frame-injection layer
#
# A Vulkan layer, not a program: the loader injects it into the instance and
# device chain, which is what makes a game patchable without cooperating.
#
# It observes vkQueuePresentKHR always; with LSL_LAYER_CAPTURE=1 it also reads
# the presented image back; with LSL_LAYER_SCALE=<n> it lies to the app about
# the surface size and upscales the smaller frame it renders back to full
# resolution with the Phase 1 FSR1 chain.
# ---------------------------------------------------------------------------
add_library(lsl-layer SHARED layer/lsl_layer.c layer/lsl_upscale.c layer/lsl_method.c
                          layer/lsl_config.c layer/lsl_profile.c ${LSL_BLOB_C})
target_link_libraries(lsl-layer PRIVATE Vulkan::Vulkan)
# -g unconditionally: when this layer misbehaves, the only useful evidence is a
# core dump, and a backtrace without line numbers costs far more than the
# symbols do.
target_compile_options(lsl-layer PRIVATE -Wall -Wextra -g)
target_include_directories(lsl-layer PRIVATE ${VULKAN_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR} ${LSL_ANIME4K_DIR})
# lsl_upscale.c includes the generated Anime4K weight header, which is not a
# source of this target, so the dependency has to be stated explicitly.
add_dependencies(lsl-layer lsl_anime4k_weights)

# The manifest has to carry an absolute library_path: the loader does not
# resolve it relative to the manifest, which is exactly the reason the
# system-installed lsfg-vk layer never loads on this host (see README).
#
# One template serves the build tree and `cmake --install` (see the install
# block at the end of this file), so the name, type and entry points a test
# loads cannot drift from the ones an installer writes.
set(LSL_LAYER_DIR ${CMAKE_CURRENT_BINARY_DIR}/layer)
set(LSL_LAYER_LIBRARY_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}lsl-layer${CMAKE_SHARED_LIBRARY_SUFFIX}")
file(MAKE_DIRECTORY ${LSL_LAYER_DIR})
configure_file(cmake/VkLayer_lsl_frame.json.in
               ${LSL_LAYER_DIR}/VkLayer_lsl_frame.json @ONLY)

enable_testing()
add_test(NAME reference COMMAND lsl-tests)

# The GUI's core: Steam discovery and the launch-options editor.  Pure text and
# files, so it runs anywhere.
add_test(NAME steam COMMAND lsl-steam-tests)

# The layer's per-game configuration: reading the GUI's profile store and
# deciding, from SteamAppId, whether this process is one of the games it names.
# No display and no GPU, so it runs everywhere.
add_test(NAME layer-config COMMAND lsl-layer-tests)

# The same seam from the other side: a store written by the GUI's own writer,
# read back by the layer's C parser.  No display and no GPU either.
add_test(NAME layer-store COMMAND lsl-layer-store-tests)

# Runs the scaler under the Khronos validation layer and fails on any validation
# error.  Skips itself when the layers are absent (tools/setup-validation-layers.sh).
add_test(NAME validation
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/validation-check.sh
                 $<TARGET_FILE:lsl-upscale>
                 --make-test 320x200 --dst 800x500 --bench 2
                 --modes nearest,bilinear,cubic,lanczos2,fsr1,fsr1rcas,lanczos3,nis,cnn)

# End to end: vkcube runs with the layer injected (LSL_LAYER_SCALE=2), the layer
# dumps the frame it upscaled at present time, and the same source is fed to
# lsl-upscale and compared -- exactly, for 8-bit swapchains.  This is the test
# that says the layer really upscales, rather than that it links.  It opens a
# window, so it skips (77) without a session.
add_test(NAME layer-upscale
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/upscale-check.sh
                 ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(layer-upscale PROPERTIES SKIP_RETURN_CODE 77)

# The layer's other half: a swapchain that is recreated rather than presented
# once.  The harness is the smallest WSI program that recreates -- oldSwapchain
# set, the previous swapchain destroyed right after, a different extent each
# time, frames still in flight -- and the script requires the four things
# upscale-check.sh cannot see: every recreated swapchain upscaled, the
# extent-only rebuild taken, a frame dumped *after* a recreate matching the
# Phase 1 upscaler byte for byte, and zero validation messages.  Skips (77)
# without a display.
add_test(NAME layer-resize
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/resize-check.sh
                 ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(layer-resize PROPERTIES SKIP_RETURN_CODE 77)

# The implicit path: the layer is found by the loader on its own and decides from
# SteamAppId whether this process is a game the GUI enabled.  Nothing is passed
# through launch options, and the negative controls prove the layer is loaded but
# silent for everything else -- which is the property that makes it safe to put
# in the loader's own search path.  Skips (77) without a display.
add_test(NAME layer-appid
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/appid-check.sh
                 ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(layer-appid PROPERTIES SKIP_RETURN_CODE 77)

# The GUI's launch-options logic, checked against whatever Steam installation
# this machine actually has: every game is put through an apply/clear round
# trip, which is the part that could damage someone's configuration.  Needs no
# display, so it runs everywhere.
if(TARGET lsl-gui)
  add_test(NAME gui-selftest COMMAND lsl-gui --selftest)

  # The same, plus building the window on that data.  Skips (77) with no
  # display; the window is never presented either way.
  add_test(NAME gui-window-selftest COMMAND lsl-gui --selftest-window)
  set_tests_properties(gui-window-selftest PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Packaging: install the layer into a throwaway prefix and prove the *installed*
# manifest and shared object load and upscale -- the copy the tests above use is
# the build tree's, which says nothing about what `cmake --install` writes.  The
# gate reuses upscale-check.sh with LSL_LAYER_DIR pointed at the installed
# directory.  Skips (77) without a display, like the other layer tests.
add_test(NAME layer-install
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/install-check.sh
                 ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(layer-install PROPERTIES SKIP_RETURN_CODE 77)

# ---------------------------------------------------------------------------
# install
#
# The layer is enabled per game from its launch options (VK_ADD_LAYER_PATH +
# VK_INSTANCE_LAYERS), so packaging means placing the shared object and its
# manifest somewhere stable and pointing the game's launch line at it -- not
# registering it under vulkan/implicit_layer.d, which would load it into every
# Vulkan application on the machine.  tools/install-layer.sh does exactly that
# and prints the line to paste.
#
# The manifest's library_path must be absolute and must name the *installed*
# object, so it is written here at install time rather than configure time:
# `cmake --install --prefix <dir>` then leaves a correct path behind, with
# $ENV{DESTDIR} honoured for staged (packager) installs.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS lsl-layer
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/lsl/layer)
install(CODE "
  set(_runtime_dir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/lsl/layer\")
  set(_disk_dir \"\$ENV{DESTDIR}\${_runtime_dir}\")
  file(MAKE_DIRECTORY \"\${_disk_dir}\")
  set(LSL_LAYER_LIBRARY_PATH
      \"\${_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}lsl-layer${CMAKE_SHARED_LIBRARY_SUFFIX}\")
  configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/VkLayer_lsl_frame.json.in\"
                 \"\${_disk_dir}/VkLayer_lsl_frame.json\" @ONLY)
  message(STATUS \"lsl: layer installed in \${_runtime_dir}\")
")
