cmake_minimum_required(VERSION 3.20)
project(lsl LANGUAGES CXX)

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
# sharpening pass, run as a second dispatch over a scratch buffer.
foreach(NAME scale rcas)
  set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME}.comp)
  set(SPV ${LSL_SPV_DIR}/${NAME}.comp.spv)
  add_custom_command(
    OUTPUT ${SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LSL_SPV_DIR}
    COMMAND ${GLSLC} --target-env=vulkan1.2 -O -I ${LSL_FFX_DIR} -o ${SPV} ${SRC}
    DEPENDS ${SRC} ${LSL_FFX_DIR}/ffx_a.h ${LSL_FFX_DIR}/ffx_fsr1.h
    COMMENT "SPIR-V: ${NAME}.comp")
  list(APPEND LSL_SPV ${SPV})
endforeach()

set(LSL_BLOB ${CMAKE_CURRENT_BINARY_DIR}/spv_blob.cpp)
add_custom_command(
  OUTPUT ${LSL_BLOB}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
          ${LSL_SPV_DIR} ${LSL_BLOB} ${CMAKE_CURRENT_BINARY_DIR}/spv_blob.hpp
  DEPENDS ${LSL_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spv.py
  COMMENT "embedding SPIR-V blobs")

add_custom_target(lsl_shaders DEPENDS ${LSL_SPV} ${LSL_BLOB})

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
# core library
# ---------------------------------------------------------------------------
add_library(lsl_core STATIC
  src/lsl_vk.cpp
  src/image_io.cpp
  src/reference.cpp
  src/fsr1_ref.cpp
  ${LSL_BLOB})
target_include_directories(lsl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(lsl_core PRIVATE ${LSL_FFX_DIR} ${LSL_FFX_CPP_DIR})
target_link_libraries(lsl_core PUBLIC Vulkan::Vulkan)
target_compile_options(lsl_core PRIVATE -Wall -Wextra)
# The FSR1 reference has to reproduce the GPU's floating-point results, and
# glslc emits separate multiplies and adds where GCC would by default contract
# a*b+c into a fused multiply-add. Turning contraction off for this one file
# keeps the two sides doing the same operations.
set_source_files_properties(src/fsr1_ref.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
add_dependencies(lsl_core lsl_shaders lsl_ffx_cpp)

# ---------------------------------------------------------------------------
# executables
# ---------------------------------------------------------------------------
add_executable(lsl-probe   src/probe.cpp)
target_link_libraries(lsl-probe PRIVATE lsl_core)

add_executable(lsl-upscale src/upscale.cpp)
target_link_libraries(lsl-upscale PRIVATE lsl_core)

add_executable(lsl-tests   tests/test_reference.cpp)
target_link_libraries(lsl-tests PRIVATE lsl_core)

enable_testing()
add_test(NAME reference COMMAND lsl-tests)

# Runs the scaler under the Khronos validation layer and fails on any validation
# error.  Skips itself when the layers are absent (tools/setup-validation-layers.sh).
add_test(NAME validation
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/validation-check.sh
                 $<TARGET_FILE:lsl-upscale>
                 --make-test 320x200 --dst 800x500 --bench 2
                 --modes nearest,bilinear,cubic,lanczos2,fsr1,fsr1rcas)
