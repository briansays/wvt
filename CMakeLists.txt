cmake_minimum_required(VERSION 3.16)
project(wvt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

set(IMGUI_DIR  ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)

# ---------------------------------------------------------------------------
# X11 + GLX.  On macOS we deliberately link XQuartz's libGL/libX11 (not the
# Cocoa OpenGL framework) so the binary is a real X11 client.
# ---------------------------------------------------------------------------
if(APPLE)
  set(X11_ROOT /opt/X11 CACHE PATH "XQuartz install prefix")
  if(NOT EXISTS ${X11_ROOT}/lib/libX11.dylib)
    message(FATAL_ERROR "XQuartz not found at ${X11_ROOT} (install from https://www.xquartz.org)")
  endif()
  set(PLATFORM_INCLUDES ${X11_ROOT}/include)
  set(PLATFORM_LIBS ${X11_ROOT}/lib/libX11.dylib ${X11_ROOT}/lib/libGL.dylib)
else()
  find_package(X11 REQUIRED)
  # LEGACY selects libGL.so, which provides both GL and GLX entry points.
  set(OpenGL_GL_PREFERENCE LEGACY)
  find_package(OpenGL REQUIRED)
  set(PLATFORM_LIBS X11::X11 OpenGL::GL)
endif()
find_package(Threads REQUIRED)

# Embed a TTF font so the binary is self-contained.
set(FONT_TTF ${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf)
set(FONT_CPP ${CMAKE_BINARY_DIR}/embedded_font.cpp)
add_custom_command(
  OUTPUT ${FONT_CPP}
  COMMAND ${CMAKE_COMMAND} -DIN=${FONT_TTF} -DOUT=${FONT_CPP} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${FONT_TTF} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)

add_library(imgui_implot STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp)
target_include_directories(imgui_implot PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMPLOT_DIR})
if(APPLE)
  # Make the OpenGL2 backend pick up XQuartz's <GL/gl.h>-compatible prototypes.
  target_include_directories(imgui_implot PRIVATE ${PLATFORM_INCLUDES})
else()
  # The OpenGL2 backend needs <GL/gl.h>; the imported target carries its include path.
  target_link_libraries(imgui_implot PUBLIC OpenGL::GL)
endif()

add_executable(wvt
  src/main.cpp
  src/app.cpp
  src/engine.cpp
  src/dsp.cpp
  src/iq_file.cpp
  src/platform_x11.cpp
  src/sw_renderer.cpp
  src/ui_util.cpp
  src/app_live.cpp
  src/app_siggen.cpp
  src/live.cpp
  src/recorder.cpp
  src/sdr_device.cpp
  src/sdr_sim.cpp
  src/sdr_siggen.cpp
  src/siggen.cpp
  src/siggen_out.cpp
  ${FONT_CPP})
target_include_directories(wvt PRIVATE src ${PLATFORM_INCLUDES})
target_link_libraries(wvt PRIVATE imgui_implot ${PLATFORM_LIBS} Threads::Threads)
target_compile_options(wvt PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Optional SDR backends. AUTO uses a library if it is found, ON requires it.
# ---------------------------------------------------------------------------
set(WVT_WITH_RTLSDR AUTO CACHE STRING "Build the RTL-SDR backend (AUTO, ON, OFF)")
set(WVT_WITH_UHD AUTO CACHE STRING "Build the UHD (USRP B200/B205) backend (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_RTLSDR PROPERTY STRINGS AUTO ON OFF)
set_property(CACHE WVT_WITH_UHD PROPERTY STRINGS AUTO ON OFF)
find_package(PkgConfig QUIET)

if(NOT WVT_WITH_RTLSDR STREQUAL "OFF")
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(RTLSDR IMPORTED_TARGET librtlsdr)
  endif()
  if(RTLSDR_FOUND)
    target_sources(wvt PRIVATE src/sdr_rtlsdr.cpp)
    target_link_libraries(wvt PRIVATE PkgConfig::RTLSDR)
    target_compile_definitions(wvt PRIVATE WVT_HAVE_RTLSDR)
    # rtlsdr_set_bias_tee() only exists in librtlsdr 0.6.0+ builds from 2018 on.
    include(CheckSymbolExists)
    # Link through the imported target: raw LDFLAGS end up before the test
    # object on the link line, and the linker then drops the library.
    set(CMAKE_REQUIRED_LIBRARIES PkgConfig::RTLSDR)
    check_symbol_exists(rtlsdr_set_bias_tee "rtl-sdr.h" WVT_RTLSDR_HAS_BIAS_TEE)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(WVT_RTLSDR_HAS_BIAS_TEE)
      target_compile_definitions(wvt PRIVATE WVT_RTLSDR_HAS_BIAS_TEE)
    endif()
    message(STATUS "wvt: RTL-SDR support enabled (librtlsdr ${RTLSDR_VERSION})")
  elseif(WVT_WITH_RTLSDR STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_RTLSDR=ON but librtlsdr was not found (install librtlsdr-dev / librtlsdr)")
  else()
    message(STATUS "wvt: RTL-SDR support disabled (librtlsdr not found)")
  endif()
endif()

if(NOT WVT_WITH_UHD STREQUAL "OFF")
  # wvt uses UHD's C API (uhd.h), so Boost headers are not needed.
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(UHDPC IMPORTED_TARGET uhd)
  endif()
  if(UHDPC_FOUND)
    target_link_libraries(wvt PRIVATE PkgConfig::UHDPC)
    set(WVT_UHD_OK TRUE)
    set(WVT_UHD_VERSION ${UHDPC_VERSION})
  else()
    find_package(UHD QUIET)
    if(UHD_FOUND)
      target_include_directories(wvt PRIVATE ${UHD_INCLUDE_DIRS})
      target_link_libraries(wvt PRIVATE ${UHD_LIBRARIES})
      set(WVT_UHD_OK TRUE)
      set(WVT_UHD_VERSION ${UHD_VERSION})
    endif()
  endif()
  if(WVT_UHD_OK)
    target_sources(wvt PRIVATE src/sdr_uhd.cpp)
    target_compile_definitions(wvt PRIVATE WVT_HAVE_UHD)
    message(STATUS "wvt: UHD support enabled (UHD ${WVT_UHD_VERSION})")
  elseif(WVT_WITH_UHD STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_UHD=ON but UHD was not found (install libuhd-dev / uhd)")
  else()
    message(STATUS "wvt: UHD support disabled (UHD not found)")
  endif()
endif()

# Signal Hound BB60 (bb_api). The SDK has no pkg-config file and ships the
# library versioned (libbb_api.so.5.0.x) in per-platform folders, so search
# SIGNALHOUND_SDK_DIR for the header and for a library built for this CPU.
set(WVT_WITH_BB AUTO CACHE STRING "Build the Signal Hound BB60 backend (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_BB PROPERTY STRINGS AUTO ON OFF)
set(SIGNALHOUND_SDK_DIR "" CACHE PATH "Signal Hound SDK folder (or a prefix with include/ and lib/)")
if(NOT WVT_WITH_BB STREQUAL "OFF")
  find_path(BB_API_INCLUDE_DIR bb_api.h
    HINTS ${SIGNALHOUND_SDK_DIR}
    PATH_SUFFIXES include device_apis/bb_series/include bb_series/include)
  find_library(BB_API_LIBRARY NAMES bb_api HINTS ${SIGNALHOUND_SDK_DIR} PATH_SUFFIXES lib lib64)
  if(NOT BB_API_LIBRARY AND SIGNALHOUND_SDK_DIR)
    file(GLOB_RECURSE _bb_libs "${SIGNALHOUND_SDK_DIR}/libbb_api.so*")
    foreach(_lib IN LISTS _bb_libs)
      string(TOLOWER "${_lib}" _l)
      if(_l MATCHES "aarch64|arm64")
        set(_arm TRUE)
      else()
        set(_arm FALSE)
      endif()
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_want_arm TRUE)
      else()
        set(_want_arm FALSE)
      endif()
      if(_arm STREQUAL _want_arm AND NOT IS_SYMLINK "${_lib}")
        set(BB_API_LIBRARY "${_lib}" CACHE FILEPATH "bb_api library" FORCE)
        break()
      endif()
    endforeach()
  endif()
  if(BB_API_INCLUDE_DIR AND BB_API_LIBRARY)
    # An imported target links the exact file; a bare path ending in .so.5.0.x
    # would become -lbb_api and miss it.
    add_library(bb_api SHARED IMPORTED)
    set_target_properties(bb_api PROPERTIES
      IMPORTED_LOCATION ${BB_API_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${BB_API_INCLUDE_DIR})
    target_sources(wvt PRIVATE src/sdr_bb60.cpp)
    target_link_libraries(wvt PRIVATE bb_api)
    target_compile_definitions(wvt PRIVATE WVT_HAVE_BB)
    message(STATUS "wvt: Signal Hound BB60 support enabled (${BB_API_LIBRARY})")
  elseif(WVT_WITH_BB STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_BB=ON but bb_api was not found (download the Signal Hound SDK and set -DSIGNALHOUND_SDK_DIR=...)")
  else()
    message(STATUS "wvt: Signal Hound BB60 support disabled (bb_api not found; set SIGNALHOUND_SDK_DIR)")
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional NVIDIA GPU acceleration (CUDA + cuFFT), see docs/cuda-gpu-plan.md.
# Targets desktop GeForce RTX cards on x86-64 Linux. The CUDA runtime and
# cuFFT are linked statically, so the binary needs only the NVIDIA driver.
# Without CUDA, gpu_stub.cpp reports "no GPU" and everything runs on the CPU.
# ---------------------------------------------------------------------------
set(WVT_WITH_CUDA AUTO CACHE STRING "Build NVIDIA GPU (CUDA) acceleration (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_CUDA PROPERTY STRINGS AUTO ON OFF)
set(WVT_CUDA_OK FALSE)
set(_cuda_why "")
if(WVT_WITH_CUDA STREQUAL "OFF")
  set(_cuda_why "WVT_WITH_CUDA=OFF")
elseif(APPLE)
  set(_cuda_why "not available on macOS")
elseif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(_cuda_why "only x86-64 is supported (desktop RTX cards)")
elseif(CMAKE_VERSION VERSION_LESS 3.18)
  set(_cuda_why "needs CMake 3.18 or later")
else()
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    find_package(CUDAToolkit 12 QUIET)
  endif()
  if(CMAKE_CUDA_COMPILER AND CUDAToolkit_FOUND)
    cmake_policy(SET CMP0104 NEW)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES AND NOT DEFINED ENV{CUDAARCHS})
      # One cubin per RTX generation: 20 = sm_75, 30 = sm_86, 40 = sm_89, 50 = sm_120.
      # Before CUDA 12.8, RTX 50 cards JIT-compile the sm_89 PTX.
      if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL 12.8)
        set(CMAKE_CUDA_ARCHITECTURES 75-real 86-real 89-real 120)
      else()
        set(CMAKE_CUDA_ARCHITECTURES 75-real 86-real 89)
      endif()
    endif()
    enable_language(CUDA)
    set(WVT_CUDA_OK TRUE)
  else()
    set(_cuda_why "no CUDA 12+ toolkit found (nvcc)")
  endif()
endif()

if(WVT_CUDA_OK)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  add_library(wvt_gpu STATIC src/gpu_cuda.cu src/gpu_common.cpp)
  set_target_properties(wvt_gpu PROPERTIES CUDA_RUNTIME_LIBRARY Static)
  target_compile_options(wvt_gpu PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-O3>)
  target_link_libraries(wvt_gpu PUBLIC CUDA::cudart_static CUDA::cufft_static CUDA::culibos)
  string(REPLACE ";" "," _archs "${CMAKE_CUDA_ARCHITECTURES}")
  message(STATUS "wvt: CUDA support enabled (CUDA ${CUDAToolkit_VERSION}, archs ${_archs}, static cuFFT)")
  if(CUDAToolkit_VERSION VERSION_LESS 12.8)
    message(STATUS "wvt: note: CUDA 12.8 or later is recommended (native RTX 50 support)")
  endif()
else()
  if(WVT_WITH_CUDA STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_CUDA=ON but CUDA cannot be used: ${_cuda_why}")
  endif()
  add_library(wvt_gpu STATIC src/gpu_stub.cpp src/gpu_common.cpp)
  message(STATUS "wvt: CUDA support disabled (${_cuda_why})")
endif()
target_include_directories(wvt_gpu PUBLIC src)
target_link_libraries(wvt PRIVATE wvt_gpu)

# Synthetic test-signal generator
add_executable(iqgen tools/iqgen.cpp)

# Signal generator self-test (DSP and file output; no device needed)
add_executable(siggen_check tools/siggen_check.cpp src/siggen.cpp src/siggen_out.cpp src/recorder.cpp
  src/sdr_device.cpp src/sdr_sim.cpp src/sdr_siggen.cpp src/iq_file.cpp src/dsp.cpp src/ui_util.cpp)
target_include_directories(siggen_check PRIVATE src)
target_link_libraries(siggen_check PRIVATE imgui_implot Threads::Threads)

# GPU self-test and benchmark: compares GPU and CPU waterfall / live spectrum
# results and prints timings (CPU timings only in a build without CUDA).
add_executable(gpu_check tools/gpu_check.cpp src/engine.cpp src/dsp.cpp src/iq_file.cpp)
target_include_directories(gpu_check PRIVATE src)
target_link_libraries(gpu_check PRIVATE wvt_gpu Threads::Threads)
