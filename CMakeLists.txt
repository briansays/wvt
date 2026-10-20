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
if(APPLE)
  # dsp.cpp uses Accelerate (vDSP) for the FFT; every target that builds it needs it.
  link_libraries("-framework Accelerate")
endif()

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
  src/app_chain.cpp
  src/live.cpp
  src/recorder.cpp
  src/sdr_device.cpp
  src/sdr_sim.cpp
  src/sdr_siggen.cpp
  src/siggen.cpp
  src/siggen_out.cpp
  src/json.cpp
  src/audio_dsp.cpp
  src/audio_engine.cpp
  src/audio_out.cpp
  src/wav_writer.cpp
  src/app_audio.cpp
  src/log.cpp
  src/extract.cpp
  src/app_export.cpp
  src/app_menu.cpp
  ${FONT_CPP})
target_include_directories(wvt PRIVATE src ${PLATFORM_INCLUDES})
target_link_libraries(wvt PRIVATE imgui_implot ${PLATFORM_LIBS} Threads::Threads)
target_compile_options(wvt PRIVATE -Wall -Wextra -Wno-unused-parameter)

# The git revision goes into the log file header. It is read at configure time.
execute_process(COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE WVT_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(WVT_GIT_HASH)
  set_property(SOURCE src/log.cpp APPEND PROPERTY COMPILE_DEFINITIONS WVT_GIT_HASH="${WVT_GIT_HASH}")
endif()

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

# ---------------------------------------------------------------------------
# Optional NVIDIA GPU acceleration (issue #3). AUTO builds the CUDA backend
# when the CUDA toolkit (12.0 or later, with cuFFT) is found, ON requires it.
# A CUDA build still runs on computers without a GPU: wvt checks at start.
# Override the GPU targets with -DCMAKE_CUDA_ARCHITECTURES=...
# ---------------------------------------------------------------------------
set(WVT_WITH_CUDA AUTO CACHE STRING "Build the CUDA GPU backend (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_CUDA PROPERTY STRINGS AUTO ON OFF)
option(WVT_CUDA_STATIC "Link the CUDA runtime and cuFFT statically (runs with only the NVIDIA driver)" ON)
if(DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(WVT_CUDA_ARCHS_GIVEN TRUE)
endif()
set(WVT_CUDA_OK FALSE)
set(WVT_CUDA_WHY "")
if(WVT_WITH_CUDA STREQUAL "OFF")
  set(WVT_CUDA_WHY "WVT_WITH_CUDA=OFF")
elseif(APPLE)
  set(WVT_CUDA_WHY "CUDA is not available on macOS")
elseif(CMAKE_VERSION VERSION_LESS 3.18)
  set(WVT_CUDA_WHY "needs CMake 3.18 or later")
else()
  include(CheckLanguage)
  check_language(CUDA)
  if(NOT CMAKE_CUDA_COMPILER)
    set(WVT_CUDA_WHY "CUDA toolkit (nvcc) not found")
  else()
    enable_language(CUDA)
    find_package(CUDAToolkit)
    if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.0)
      set(WVT_CUDA_WHY "CUDA ${CMAKE_CUDA_COMPILER_VERSION} is older than 12.0")
    elseif(NOT TARGET CUDA::cufft)
      set(WVT_CUDA_WHY "cuFFT not found")
    else()
      set(WVT_CUDA_OK TRUE)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional Apple GPU acceleration (issue #21): Metal and MPSGraph, macOS only.
# The kernels compile at run time, so Xcode is not needed. The GPU path needs
# macOS 14 at run time; older systems use the CPU.
# ---------------------------------------------------------------------------
set(WVT_WITH_METAL AUTO CACHE STRING "Build the Metal GPU backend on macOS (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_METAL PROPERTY STRINGS AUTO ON OFF)
set(WVT_METAL_OK FALSE)
if(WVT_WITH_METAL STREQUAL "OFF")
  set(WVT_METAL_WHY "WVT_WITH_METAL=OFF")
elseif(NOT APPLE)
  set(WVT_METAL_WHY "Metal is macOS only")
else()
  include(CheckLanguage)
  check_language(OBJCXX)
  find_library(WVT_METAL_FW Metal)
  find_library(WVT_MPSGRAPH_FW MetalPerformanceShadersGraph)
  if(NOT CMAKE_OBJCXX_COMPILER)
    set(WVT_METAL_WHY "no Objective-C++ compiler")
  elseif(NOT WVT_METAL_FW OR NOT WVT_MPSGRAPH_FW)
    set(WVT_METAL_WHY "Metal or MetalPerformanceShadersGraph framework not found")
  else()
    enable_language(OBJCXX)
    set(WVT_METAL_OK TRUE)
  endif()
endif()

add_library(wvt_gpu STATIC src/gpu_common.cpp)
target_include_directories(wvt_gpu PUBLIC src)
target_compile_options(wvt_gpu PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
if(WVT_CUDA_OK)
  target_sources(wvt_gpu PRIVATE src/gpu_cuda.cu)
  if(NOT WVT_CUDA_ARCHS_GIVEN)
    # Desktop RTX 20xx to 40xx and data-center A100 / H100, plus PTX for newer GPUs.
    set(WVT_ARCHS 75-real 80-real 86-real 89-real 90)
    if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 12.8)
      set(WVT_ARCHS 75-real 80-real 86-real 89-real 90-real 120)  # RTX 50xx
    endif()
    set_target_properties(wvt_gpu PROPERTIES CUDA_ARCHITECTURES "${WVT_ARCHS}")
  endif()
  get_target_property(WVT_ARCHS_USED wvt_gpu CUDA_ARCHITECTURES)
  if(NOT CMAKE_CUDA20_STANDARD_COMPILE_OPTION)
    # CMake before 3.25 (Ubuntu 22.04 has 3.22) does not know nvcc's C++20 flag.
    set(CMAKE_CUDA20_STANDARD_COMPILE_OPTION "-std=c++20")
    set(CMAKE_CUDA20_EXTENSION_COMPILE_OPTION "-std=c++20")
  endif()
  set_target_properties(wvt_gpu PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
  if(WVT_CUDA_STATIC)
    target_link_libraries(wvt_gpu PUBLIC CUDA::cudart_static CUDA::cufft_static)
    # Static cuFFT has relocatable device code: the library needs a device-link step.
    set_target_properties(wvt_gpu PROPERTIES CUDA_SEPARABLE_COMPILATION ON CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    set(WVT_CUDA_LINK static)
  else()
    target_link_libraries(wvt_gpu PUBLIC CUDA::cudart CUDA::cufft)
    set(WVT_CUDA_LINK shared)
  endif()
  message(STATUS "wvt: CUDA support enabled (CUDA ${CMAKE_CUDA_COMPILER_VERSION}, archs ${WVT_ARCHS_USED}, ${WVT_CUDA_LINK} cuFFT)")
elseif(WVT_METAL_OK)
  target_sources(wvt_gpu PRIVATE src/gpu_metal.mm)
  set_target_properties(wvt_gpu PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(wvt_gpu PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc -Wall -Wextra -Wno-unused-parameter>)
  target_link_libraries(wvt_gpu PUBLIC "-framework Foundation" "-framework Metal" "-framework MetalPerformanceShaders"
                        "-framework MetalPerformanceShadersGraph")
  message(STATUS "wvt: Metal support enabled (MPSGraph FFT; the GPU path needs macOS 14 at run time)")
else()
  target_sources(wvt_gpu PRIVATE src/gpu_stub.cpp)
endif()
if(NOT WVT_CUDA_OK)
  if(WVT_WITH_CUDA STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_CUDA=ON but CUDA cannot be built: ${WVT_CUDA_WHY}")
  endif()
  message(STATUS "wvt: CUDA support disabled (${WVT_CUDA_WHY})")
endif()
if(NOT WVT_METAL_OK AND APPLE)
  if(WVT_WITH_METAL STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_METAL=ON but Metal cannot be built: ${WVT_METAL_WHY}")
  endif()
  message(STATUS "wvt: Metal support disabled (${WVT_METAL_WHY})")
elseif(WVT_WITH_METAL STREQUAL "ON" AND NOT APPLE)
  message(FATAL_ERROR "WVT_WITH_METAL=ON but ${WVT_METAL_WHY}")
endif()
target_link_libraries(wvt PRIVATE wvt_gpu)

# ---------------------------------------------------------------------------
# Audio output through the bundled miniaudio. It needs no extra packages:
# Linux backends (PipeWire, PulseAudio, ALSA) load at run time; macOS links
# CoreAudio. OFF builds without playback (WAV export still works).
# ---------------------------------------------------------------------------
set(WVT_WITH_AUDIO AUTO CACHE STRING "Build audio playback with miniaudio (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_AUDIO PROPERTY STRINGS AUTO ON OFF)
if(NOT WVT_WITH_AUDIO STREQUAL "OFF")
  set(MINIAUDIO_DIR ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
  set(WVT_MA_DEFS MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
  add_library(miniaudio STATIC src/miniaudio_impl.c)
  target_include_directories(miniaudio PUBLIC ${MINIAUDIO_DIR})
  target_compile_options(miniaudio PRIVATE -w)
  if(APPLE)
    # Link the frameworks directly instead of loading them at run time.
    list(APPEND WVT_MA_DEFS MA_NO_RUNTIME_LINKING)
    target_link_libraries(miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  else()
    target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m Threads::Threads)
  endif()
  target_compile_definitions(miniaudio PUBLIC ${WVT_MA_DEFS})
  target_link_libraries(wvt PRIVATE miniaudio)
  target_compile_definitions(wvt PRIVATE WVT_HAVE_AUDIO)
  message(STATUS "wvt: audio playback enabled (miniaudio)")
else()
  message(STATUS "wvt: audio playback disabled (WVT_WITH_AUDIO=OFF)")
endif()

# Synthetic test-signal generator
add_executable(iqgen tools/iqgen.cpp)

# Signal generator self-test (DSP and file output; no device needed)
add_executable(siggen_check tools/siggen_check.cpp src/siggen.cpp src/siggen_out.cpp src/json.cpp src/recorder.cpp
  src/sdr_device.cpp src/sdr_sim.cpp src/sdr_siggen.cpp src/iq_file.cpp src/dsp.cpp src/ui_util.cpp src/log.cpp)
target_include_directories(siggen_check PRIVATE src)
target_link_libraries(siggen_check PRIVATE imgui_implot Threads::Threads)

# Audio demodulator self-test (no sound device needed)
add_executable(audio_check tools/audio_check.cpp src/audio_dsp.cpp src/wav_writer.cpp src/recorder.cpp
  src/iq_file.cpp src/dsp.cpp src/log.cpp)
target_include_directories(audio_check PRIVATE src)
target_link_libraries(audio_check PRIVATE Threads::Threads)

# Logger self-test (levels, argument parsing, log file, threads)
add_executable(log_check tools/log_check.cpp src/log.cpp)
target_include_directories(log_check PRIVATE src)
target_link_libraries(log_check PRIVATE Threads::Threads)

# Time segment self-test (segment rules, overview on a sample range)
add_executable(segment_check tools/segment_check.cpp src/engine.cpp src/dsp.cpp src/iq_file.cpp src/ui_util.cpp
  src/log.cpp)
target_include_directories(segment_check PRIVATE src)
target_link_libraries(segment_check PRIVATE wvt_gpu imgui_implot Threads::Threads)

# Region export self-test (copies, band filter, metadata, checks, cancel)
add_executable(extract_check tools/extract_check.cpp src/extract.cpp src/recorder.cpp src/engine.cpp src/dsp.cpp
  src/iq_file.cpp src/ui_util.cpp src/log.cpp)
target_include_directories(extract_check PRIVATE src)
target_link_libraries(extract_check PRIVATE wvt_gpu imgui_implot Threads::Threads)
target_compile_options(extract_check PRIVATE -Wall -Wextra -Wno-unused-parameter)

# GPU self-test and benchmark (CPU times; GPU vs CPU once a GPU backend exists)
add_executable(gpu_check tools/gpu_check.cpp src/engine.cpp src/dsp.cpp src/iq_file.cpp src/ui_util.cpp src/log.cpp)
target_include_directories(gpu_check PRIVATE src)
target_link_libraries(gpu_check PRIVATE wvt_gpu imgui_implot Threads::Threads)
target_compile_options(gpu_check PRIVATE -Wall -Wextra -Wno-unused-parameter)
