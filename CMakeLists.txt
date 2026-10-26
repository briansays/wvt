cmake_minimum_required(VERSION 3.16)
project(wvt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# CMake's Release flags for GCC and Clang are -O3 -DNDEBUG. They are not set
# here, so flags from the user or a distribution package stay (issue #54).

# Sanitizer builds for the self-checks (issue #52): "address" is ASan with
# UBSan, "thread" is TSan (the two cannot be combined). Every target gets the
# flags, so one option builds wvt, the tools and the self-checks. CUDA code
# does not: nvcc does not know these flags.
set(WVT_SANITIZE "" CACHE STRING "Build with sanitizers: address (ASan + UBSan), thread (TSan), or empty for none")
set_property(CACHE WVT_SANITIZE PROPERTY STRINGS "" address thread)
if(WVT_SANITIZE STREQUAL "address")
  set(WVT_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(WVT_SANITIZE STREQUAL "thread")
  set(WVT_SAN_FLAGS -fsanitize=thread)
elseif(WVT_SANITIZE)
  message(FATAL_ERROR "WVT_SANITIZE must be address, thread or empty (got '${WVT_SANITIZE}')")
endif()
if(WVT_SAN_FLAGS)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:${WVT_SAN_FLAGS};-fno-omit-frame-pointer;-g>")
  add_link_options(${WVT_SAN_FLAGS})
  message(STATUS "wvt: sanitizers enabled (${WVT_SANITIZE}: ${WVT_SAN_FLAGS})")
endif()

# Warnings for wvt's own code (not third_party/). WVT_WERROR makes them errors;
# CI uses it.
option(WVT_WERROR "Treat compiler warnings in wvt's own code as errors" OFF)
set(WVT_WARNINGS -Wall -Wextra -Wno-unused-parameter)
if(WVT_WERROR)
  list(APPEND WVT_WARNINGS -Werror)
endif()

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
  src/app/main.cpp
  src/app/cli.cpp
  src/app/app.cpp
  src/dsp/engine.cpp
  src/dsp/dsp.cpp
  src/io/iq_file.cpp
  src/app/platform_x11.cpp
  src/app/sw_renderer.cpp
  src/common/ui_util.cpp
  src/app/app_live.cpp
  src/app/config.cpp
  src/app/app_siggen.cpp
  src/app/app_chain.cpp
  src/device/live.cpp
  src/io/recorder.cpp
  src/device/sdr_device.cpp
  src/device/sdr_sim.cpp
  src/device/sdr_siggen.cpp
  src/siggen/siggen.cpp
  src/siggen/siggen_out.cpp
  src/common/json.cpp
  src/audio/audio_dsp.cpp
  src/audio/audio_engine.cpp
  src/audio/audio_out.cpp
  src/io/wav_writer.cpp
  src/app/app_audio.cpp
  src/common/log.cpp
  src/dsp/extract.cpp
  src/app/app_export.cpp
  src/app/app_menu.cpp
  src/app/app_snapshot.cpp
  src/app/app_session.cpp
  src/io/session.cpp
  src/io/snapshot.cpp
  src/dsp/measure.cpp
  src/app/app_measure.cpp
  ${FONT_CPP})
target_include_directories(wvt PRIVATE src third_party/stb ${PLATFORM_INCLUDES})
target_link_libraries(wvt PRIVATE imgui_implot ${PLATFORM_LIBS} Threads::Threads)
target_compile_options(wvt PRIVATE ${WVT_WARNINGS})

# The git commit and version tag for the About window and the log file
# header (issue #61). Read at every build, not only at configure time.
set(BUILD_INFO_CPP ${CMAKE_BINARY_DIR}/build_info.cpp)
add_custom_target(wvt_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${BUILD_INFO_CPP} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${BUILD_INFO_CPP}
  VERBATIM)
target_sources(wvt PRIVATE ${BUILD_INFO_CPP})
add_dependencies(wvt wvt_build_info)

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
    target_sources(wvt PRIVATE src/device/sdr_rtlsdr.cpp)
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
    target_sources(wvt PRIVATE src/device/sdr_uhd.cpp)
    target_compile_definitions(wvt PRIVATE WVT_HAVE_UHD)
    message(STATUS "wvt: UHD support enabled (UHD ${WVT_UHD_VERSION})")
  elseif(WVT_WITH_UHD STREQUAL "ON")
    message(FATAL_ERROR "WVT_WITH_UHD=ON but UHD was not found (install libuhd-dev / uhd)")
  else()
    message(STATUS "wvt: UHD support disabled (UHD not found)")
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional Signal Hound BB60 backend (issue #2) through bb_api from the Signal
# Hound SDK. The SDK has no pkg-config file and ships the library versioned in
# per-platform folders (Linux x86-64 and aarch64 .so, macOS arm64 .dylib), so
# search SIGNALHOUND_SDK_DIR for the header and a library for this CPU. The
# library is copied into the build folder under its runtime name, and wvt
# finds it there through its rpath. WVT_BB_SIM builds the backend against a
# simulated device instead (tools/bb_sim), for CI and tests.
# ---------------------------------------------------------------------------
set(WVT_WITH_BB AUTO CACHE STRING "Build the Signal Hound BB60 backend (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_BB PROPERTY STRINGS AUTO ON OFF)
set(SIGNALHOUND_SDK_DIR "" CACHE PATH "Signal Hound SDK folder (or a prefix with include/ and lib/)")
option(WVT_BB_SIM "Build the BB60 backend against a simulated device (no SDK, no hardware)" OFF)
set(WVT_BB_OK FALSE)
set(WVT_BB_WHY "")
if(WVT_BB_SIM)
  add_library(bb_api_sim STATIC tools/bb_sim/bb_api_sim.cpp)
  target_include_directories(bb_api_sim PUBLIC tools/bb_sim)
  target_compile_options(bb_api_sim PRIVATE ${WVT_WARNINGS})
  set(WVT_BB_TARGET bb_api_sim)
  set(WVT_BB_OK TRUE)
  set(WVT_BB_WHAT "simulated device, tools/bb_sim")
elseif(WVT_WITH_BB STREQUAL "OFF")
  set(WVT_BB_WHY "WVT_WITH_BB=OFF")
else()
  find_path(BB_API_INCLUDE_DIR bb_api.h
    HINTS ${SIGNALHOUND_SDK_DIR}
    PATH_SUFFIXES include device_apis/bb_series/include bb_series/include)
  find_library(BB_API_LIBRARY NAMES bb_api HINTS ${SIGNALHOUND_SDK_DIR} PATH_SUFFIXES lib lib64)
  if(CMAKE_OSX_ARCHITECTURES)
    set(_bb_cpu "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(_bb_cpu "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(NOT BB_API_LIBRARY AND SIGNALHOUND_SDK_DIR)
    if(APPLE)
      if(_bb_cpu MATCHES "arm64")
        file(GLOB_RECURSE _bb_libs "${SIGNALHOUND_SDK_DIR}/libbb_api*.dylib")
      else()
        set(WVT_BB_WHY "the macOS bb_api is arm64 only; configure with -DCMAKE_OSX_ARCHITECTURES=arm64 (this build is ${_bb_cpu})")
      endif()
    else()
      file(GLOB_RECURSE _bb_libs "${SIGNALHOUND_SDK_DIR}/libbb_api.so.*")
      set(_bb_keep "")
      foreach(_lib IN LISTS _bb_libs)
        string(TOLOWER "${_lib}" _l)
        # Ubuntu 14.04 has the old version 3 API.
        if(_l MATCHES "14\\.04" OR IS_SYMLINK "${_lib}")
          continue()
        endif()
        if((_l MATCHES "aarch64|arm64") STREQUAL (_bb_cpu MATCHES "aarch64|arm64"))
          list(APPEND _bb_keep "${_lib}")
        endif()
      endforeach()
      set(_bb_libs ${_bb_keep})
    endif()
    # The newest version: sort by the version suffix.
    set(_bb_best "")
    set(_bb_best_ver "0")
    foreach(_lib IN LISTS _bb_libs)
      string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _ver "${_lib}")
      if(_ver VERSION_GREATER _bb_best_ver)
        set(_bb_best "${_lib}")
        set(_bb_best_ver "${_ver}")
      endif()
    endforeach()
    if(_bb_best)
      set(BB_API_LIBRARY "${_bb_best}" CACHE FILEPATH "bb_api library" FORCE)
    endif()
  endif()
  if(BB_API_INCLUDE_DIR AND BB_API_LIBRARY)
    # Copy the library under the name the runtime loader looks for.
    set(_bb_dir "${CMAKE_BINARY_DIR}/bb_api")
    file(MAKE_DIRECTORY "${_bb_dir}")
    get_filename_component(_bb_src_dir "${BB_API_LIBRARY}" DIRECTORY)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _ver "${BB_API_LIBRARY}")
    string(REGEX MATCH "^[0-9]+" _major "${_ver}")
    if(APPLE)
      # The dylib's install name is a bare "libbb_api.5.dylib"; give the copy an
      # @rpath name so wvt finds it next to the build.
      set(_bb_lib "${_bb_dir}/libbb_api.${_major}.dylib")
      configure_file("${BB_API_LIBRARY}" "${_bb_lib}" COPYONLY)
      execute_process(COMMAND install_name_tool -id "@rpath/libbb_api.${_major}.dylib" "${_bb_lib}" RESULT_VARIABLE _r)
      execute_process(COMMAND codesign --force --sign - "${_bb_lib}" OUTPUT_QUIET ERROR_QUIET)
    else()
      set(_bb_lib "${_bb_dir}/libbb_api.so.${_ver}")
      configure_file("${BB_API_LIBRARY}" "${_bb_lib}" COPYONLY)
      foreach(_n libbb_api.so.${_major} libbb_api.so)
        file(CREATE_LINK "libbb_api.so.${_ver}" "${_bb_dir}/${_n}" SYMBOLIC)
      endforeach()
    endif()
    add_library(bb_api SHARED IMPORTED)
    set_target_properties(bb_api PROPERTIES IMPORTED_LOCATION "${_bb_lib}"
      INTERFACE_INCLUDE_DIRECTORIES "${BB_API_INCLUDE_DIR}")
    # The x86-64 Linux library needs FTDI's libftd2xx, which the SDK ships next to it.
    # libbb_api has no RUNPATH of its own, and a RUNPATH on wvt applies only to wvt's
    # own NEEDED entries, so wvt gets an old-style RPATH, which the loader also uses for
    # libbb_api's libftd2xx. wvt must not list libftd2xx itself: it has its own copy of
    # libusb and exports the libusb_* functions, so ahead of the system libusb it takes
    # over librtlsdr's and UHD's libusb calls, and they crash.
    if(EXISTS "${_bb_src_dir}/libftd2xx.so")
      configure_file("${_bb_src_dir}/libftd2xx.so" "${_bb_dir}/libftd2xx.so" COPYONLY)
      target_link_options(wvt PRIVATE "LINKER:--disable-new-dtags")
    endif()
    set(WVT_BB_TARGET bb_api)
    set(WVT_BB_OK TRUE)
    set(WVT_BB_WHAT "bb_api ${_ver}, ${BB_API_LIBRARY}")
    set_property(TARGET wvt APPEND PROPERTY BUILD_RPATH "${_bb_dir}")
  elseif(NOT WVT_BB_WHY)
    set(WVT_BB_WHY "bb_api not found; download the Signal Hound SDK and set -DSIGNALHOUND_SDK_DIR=...")
  endif()
endif()
if(WVT_BB_OK)
  target_sources(wvt PRIVATE src/device/sdr_bb60.cpp)
  target_link_libraries(wvt PRIVATE ${WVT_BB_TARGET})
  target_compile_definitions(wvt PRIVATE WVT_HAVE_BB)
  message(STATUS "wvt: Signal Hound BB60 support enabled (${WVT_BB_WHAT})")
elseif(WVT_WITH_BB STREQUAL "ON")
  message(FATAL_ERROR "WVT_WITH_BB=ON but the BB60 backend cannot be built: ${WVT_BB_WHY}")
else()
  message(STATUS "wvt: Signal Hound BB60 support disabled (${WVT_BB_WHY})")
endif()

# ---------------------------------------------------------------------------
# Optional Epiq Solutions Sidekiq backend (issue #4) through libsidekiq from
# the Sidekiq SDK, which is for Linux only. The SDK has no pkg-config file:
# its bin/sidekiq-config gives the include folder and the static link flags,
# which are kept in their order (the static libraries need it). The SDK is the
# first of SIDEKIQ_SDK_DIR, $SIDEKIQ_SDK_DIR, ~/sidekiq_sdk_current and the
# newest third_party/sidekiq_sdk/sidekiq_sdk_v* folder. An SDK without
# sidekiq-config is linked from its lib/ folder. WVT_SKIQ_SIM builds the
# backend against a simulated card instead (tools/skiq_sim), for CI and tests.
# ---------------------------------------------------------------------------
set(WVT_WITH_SKIQ AUTO CACHE STRING "Build the Epiq Sidekiq backend (AUTO, ON, OFF)")
set_property(CACHE WVT_WITH_SKIQ PROPERTY STRINGS AUTO ON OFF)
set(SIDEKIQ_SDK_DIR "" CACHE PATH "Sidekiq SDK folder (the one with bin/sidekiq-config and sidekiq_core/)")
option(WVT_SKIQ_SIM "Build the Sidekiq backend against a simulated card (no SDK, no hardware)" OFF)
set(WVT_SKIQ_MIN_VERSION 4.14.0)  # skiq_write_ref_clock_select
set(WVT_SKIQ_OK FALSE)
set(WVT_SKIQ_WHY "")
if(WVT_SKIQ_SIM)
  add_library(skiq_api_sim STATIC tools/skiq_sim/skiq_api_sim.cpp)
  target_include_directories(skiq_api_sim PUBLIC tools/skiq_sim)
  target_compile_options(skiq_api_sim PRIVATE ${WVT_WARNINGS})
  set(WVT_SKIQ_TARGET skiq_api_sim)
  set(WVT_SKIQ_OK TRUE)
  set(WVT_SKIQ_WHAT "simulated card, tools/skiq_sim")
elseif(WVT_WITH_SKIQ STREQUAL "OFF")
  set(WVT_SKIQ_WHY "WVT_WITH_SKIQ=OFF")
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(WVT_SKIQ_WHY "the Sidekiq SDK is for Linux only")
else()
  set(_skiq_dirs "${SIDEKIQ_SDK_DIR}" "$ENV{SIDEKIQ_SDK_DIR}" "$ENV{HOME}/sidekiq_sdk_current")
  file(GLOB _skiq_local "${CMAKE_SOURCE_DIR}/third_party/sidekiq_sdk/sidekiq_sdk_v*")
  set(_skiq_newest "")
  set(_skiq_newest_ver "0")
  foreach(_d IN LISTS _skiq_local)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+$" _v "${_d}")
    if(IS_DIRECTORY "${_d}" AND _v AND _v VERSION_GREATER _skiq_newest_ver)
      set(_skiq_newest "${_d}")
      set(_skiq_newest_ver "${_v}")
    endif()
  endforeach()
  list(APPEND _skiq_dirs "${_skiq_newest}")
  set(_skiq_dir "")
  foreach(_d IN LISTS _skiq_dirs)
    if(_d AND EXISTS "${_d}/sidekiq_core/inc/sidekiq_api.h")
      set(_skiq_dir "${_d}")
      break()
    endif()
  endforeach()
  # The SDK's name for this CPU's build (one SDK holds several).
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_skiq_cfg aarch64)
  else()
    set(_skiq_cfg x86_64.gcc)
  endif()
  set(_skiq_inc "")
  set(_skiq_libs "")
  set(_skiq_ver "")
  if(NOT _skiq_dir)
    set(WVT_SKIQ_WHY "Sidekiq SDK not found; get it from Epiq Solutions and set -DSIDEKIQ_SDK_DIR=...")
  elseif(EXISTS "${_skiq_dir}/bin/sidekiq-config")
    set(_skiq_config ${CMAKE_COMMAND} -E env SIDEKIQ_CONFIG_BUILD_CONFIG=${_skiq_cfg} "${_skiq_dir}/bin/sidekiq-config")
    foreach(_q version cflags libs-static)
      execute_process(COMMAND ${_skiq_config} --${_q} OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _r
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
      if(_r)
        set(WVT_SKIQ_WHY "${_skiq_dir}/bin/sidekiq-config --${_q} failed: ${_err}")
        break()
      endif()
      set(_skiq_${_q} "${_out}")
    endforeach()
    if(NOT WVT_SKIQ_WHY)
      set(_skiq_ver "${_skiq_version}")
      separate_arguments(_skiq_cflags UNIX_COMMAND "${_skiq_cflags}")
      foreach(_f IN LISTS _skiq_cflags)
        if(_f MATCHES "^-I(.+)")
          list(APPEND _skiq_inc "${CMAKE_MATCH_1}")
        endif()
      endforeach()
      separate_arguments(_skiq_libs UNIX_COMMAND "${_skiq_libs-static}")
      # It comes after wvt's own link options and would turn the RPATH that the BB60
      # backend needs into a RUNPATH. The Sidekiq rpaths work in either form.
      list(REMOVE_ITEM _skiq_libs "-Wl,--enable-new-dtags")
    endif()
  else()
    # The link flags of SDK 4.26.0's share/sidekiq/sidekiq-config/<build>.conf.
    set(_skiq_inc "${_skiq_dir}/sidekiq_core/inc")
    set(_skiq_support "${_skiq_dir}/lib/support/${_skiq_cfg}/usr/lib/epiq")
    set(_skiq_a "${_skiq_dir}/lib/libsidekiq__${_skiq_cfg}.a")
    file(STRINGS "${_skiq_inc}/sidekiq_api.h" _vl REGEX "#define LIBSIDEKIQ_VERSION_(MAJOR|MINOR|PATCH) ")
    string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*" "\\1.\\2.\\3" _skiq_ver "${_vl}")
    if(EXISTS "${_skiq_a}")
      set(_skiq_libs -L${_skiq_support} -Wl,-rpath,${_skiq_support} -Wl,-rpath,/usr/lib/epiq "${_skiq_a}"
        -lusb-1.0 -lglib-2.0 -ltirpc -lpthread -lrt -lm -lstdc++ -ldl)
    else()
      set(WVT_SKIQ_WHY "${_skiq_a} not found (no Sidekiq library for ${CMAKE_SYSTEM_PROCESSOR} in ${_skiq_dir})")
    endif()
  endif()
  # The SDK's support folder has its own libusb 1.0.20, glib and tirpc. Its -L and rpath apply to
  # the full link, so librtlsdr and UHD then get libusb 1.0.20, which has no libusb_dev_mem_alloc
  # (1.0.21+). The system libraries are used instead when they are installed.
  if(_skiq_libs AND PKG_CONFIG_FOUND)
    pkg_check_modules(SKIQ_DEPS QUIET IMPORTED_TARGET libusb-1.0 glib-2.0 libtirpc)
  endif()
  if(_skiq_libs AND SKIQ_DEPS_FOUND)
    set(_skiq_keep "")
    foreach(_f IN LISTS _skiq_libs)
      if(NOT _f MATCHES "/lib/support/|/usr/lib/epiq" AND NOT _f MATCHES "^-l(usb-1\\.0|glib-2\\.0|tirpc)$")
        list(APPEND _skiq_keep "${_f}")
      endif()
    endforeach()
    set(_skiq_libs ${_skiq_keep} PkgConfig::SKIQ_DEPS)
  elseif(_skiq_libs AND (RTLSDR_FOUND OR WVT_UHD_OK))
    message(WARNING "wvt: Sidekiq links the SDK's libusb 1.0.20, which librtlsdr and UHD cannot use. "
      "Install libusb-1.0-0-dev, libglib2.0-dev and libtirpc-dev, or set WVT_WITH_SKIQ=OFF.")
  endif()
  if(_skiq_libs AND _skiq_ver VERSION_LESS WVT_SKIQ_MIN_VERSION)
    set(WVT_SKIQ_WHY "Sidekiq SDK ${_skiq_ver} in ${_skiq_dir} is older than ${WVT_SKIQ_MIN_VERSION}")
  elseif(_skiq_libs AND NOT WVT_SKIQ_WHY)
    add_library(sidekiq INTERFACE IMPORTED)
    # Link items that start with "-" go to the linker unchanged and in order.
    set_target_properties(sidekiq PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_skiq_inc}"
      INTERFACE_LINK_LIBRARIES "${_skiq_libs}")
    set(WVT_SKIQ_TARGET sidekiq)
    set(WVT_SKIQ_OK TRUE)
    set(WVT_SKIQ_WHAT "SDK ${_skiq_ver}, ${_skiq_dir}")
  endif()
endif()
if(WVT_SKIQ_OK)
  target_sources(wvt PRIVATE src/device/sdr_skiq.cpp)
  target_link_libraries(wvt PRIVATE ${WVT_SKIQ_TARGET})
  target_compile_definitions(wvt PRIVATE WVT_HAVE_SKIQ)
  message(STATUS "wvt: Sidekiq support enabled (${WVT_SKIQ_WHAT})")
elseif(WVT_WITH_SKIQ STREQUAL "ON")
  message(FATAL_ERROR "WVT_WITH_SKIQ=ON but the Sidekiq backend cannot be built: ${WVT_SKIQ_WHY}")
else()
  message(STATUS "wvt: Sidekiq support disabled (${WVT_SKIQ_WHY})")
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

add_library(wvt_gpu STATIC src/gpu/gpu_common.cpp)
target_include_directories(wvt_gpu PUBLIC src)
target_compile_options(wvt_gpu PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${WVT_WARNINGS}>")
if(WVT_CUDA_OK)
  target_sources(wvt_gpu PRIVATE src/gpu/gpu_cuda.cu)
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
  target_sources(wvt_gpu PRIVATE src/gpu/gpu_metal.mm)
  set_target_properties(wvt_gpu PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(wvt_gpu PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;${WVT_WARNINGS}>")
  target_link_libraries(wvt_gpu PUBLIC "-framework Foundation" "-framework Metal" "-framework MetalPerformanceShaders"
                        "-framework MetalPerformanceShadersGraph")
  message(STATUS "wvt: Metal support enabled (MPSGraph FFT; the GPU path needs macOS 14 at run time)")
else()
  target_sources(wvt_gpu PRIVATE src/gpu/gpu_stub.cpp)
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
  add_library(miniaudio STATIC src/audio/miniaudio_impl.c)
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
target_compile_options(iqgen PRIVATE ${WVT_WARNINGS})

# Signal generator self-test (DSP and file output; no device needed)
add_executable(siggen_check tools/siggen_check.cpp src/siggen/siggen.cpp src/siggen/siggen_out.cpp src/common/json.cpp src/io/recorder.cpp
  src/device/sdr_device.cpp src/device/sdr_sim.cpp src/device/sdr_siggen.cpp src/io/iq_file.cpp src/dsp/dsp.cpp src/common/ui_util.cpp src/common/log.cpp)
target_include_directories(siggen_check PRIVATE src)
target_link_libraries(siggen_check PRIVATE imgui_implot Threads::Threads)
target_compile_options(siggen_check PRIVATE ${WVT_WARNINGS})

# Audio demodulator self-test (no sound device needed)
add_executable(audio_check tools/audio_check.cpp src/audio/audio_dsp.cpp src/io/wav_writer.cpp src/io/recorder.cpp
  src/io/iq_file.cpp src/common/json.cpp src/dsp/dsp.cpp src/common/log.cpp)
target_include_directories(audio_check PRIVATE src)
target_link_libraries(audio_check PRIVATE Threads::Threads)
target_compile_options(audio_check PRIVATE ${WVT_WARNINGS})

# Logger self-test (levels, argument parsing, log file, threads)
add_executable(log_check tools/log_check.cpp src/common/log.cpp)
target_include_directories(log_check PRIVATE src)
target_link_libraries(log_check PRIVATE Threads::Threads)
target_compile_options(log_check PRIVATE ${WVT_WARNINGS})

# Time segment self-test (segment rules, overview on a sample range)
add_executable(segment_check tools/segment_check.cpp src/dsp/engine.cpp src/dsp/dsp.cpp src/io/iq_file.cpp src/common/json.cpp
  src/common/ui_util.cpp src/common/log.cpp)
target_include_directories(segment_check PRIVATE src)
target_link_libraries(segment_check PRIVATE wvt_gpu imgui_implot Threads::Threads)
target_compile_options(segment_check PRIVATE ${WVT_WARNINGS})

# Region export self-test (copies, band filter, metadata, checks, cancel)
add_executable(extract_check tools/extract_check.cpp src/dsp/extract.cpp src/io/recorder.cpp src/dsp/engine.cpp src/dsp/dsp.cpp
  src/io/iq_file.cpp src/common/json.cpp src/common/ui_util.cpp src/common/log.cpp)
target_include_directories(extract_check PRIVATE src)
target_link_libraries(extract_check PRIVATE wvt_gpu imgui_implot Threads::Threads)
target_compile_options(extract_check PRIVATE ${WVT_WARNINGS})

# GPU self-test and benchmark (CPU times; GPU vs CPU once a GPU backend exists)
add_executable(gpu_check tools/gpu_check.cpp src/dsp/engine.cpp src/dsp/dsp.cpp src/io/iq_file.cpp src/common/json.cpp src/common/ui_util.cpp
  src/common/log.cpp)
target_include_directories(gpu_check PRIVATE src)
target_link_libraries(gpu_check PRIVATE wvt_gpu imgui_implot Threads::Threads)
target_compile_options(gpu_check PRIVATE ${WVT_WARNINGS})

# Plot snapshot self-test (file names, numbering, crop, PNG writer)
# Clipboard self-test (issue #72): needs an X server (Xvfb in CI); skipped without $DISPLAY
add_executable(clipboard_check tools/clipboard_check.cpp src/app/platform_x11.cpp src/app/sw_renderer.cpp
  src/common/log.cpp)
target_include_directories(clipboard_check PRIVATE src ${PLATFORM_INCLUDES})
target_link_libraries(clipboard_check PRIVATE imgui_implot ${PLATFORM_LIBS} Threads::Threads)
target_compile_options(clipboard_check PRIVATE ${WVT_WARNINGS})

# Session file self-test (issue #76)
add_executable(session_check tools/session_check.cpp src/io/session.cpp src/io/iq_file.cpp src/io/recorder.cpp
  src/dsp/measure.cpp src/common/json.cpp src/common/log.cpp src/common/ui_util.cpp)
target_include_directories(session_check PRIVATE src)
target_link_libraries(session_check PRIVATE imgui_implot Threads::Threads)
target_compile_options(session_check PRIVATE ${WVT_WARNINGS})

# Point measurement self-test (issue #80)
add_executable(measure_check tools/measure_check.cpp src/dsp/measure.cpp)
target_include_directories(measure_check PRIVATE src)
target_compile_options(measure_check PRIVATE ${WVT_WARNINGS})

# Configuration file self-test (issue #88)
add_executable(config_check tools/config_check.cpp src/app/config.cpp)
target_include_directories(config_check PRIVATE src)
target_compile_options(config_check PRIVATE ${WVT_WARNINGS})

add_executable(snapshot_check tools/snapshot_check.cpp src/io/snapshot.cpp)
target_include_directories(snapshot_check PRIVATE src third_party/stb)
target_compile_options(snapshot_check PRIVATE ${WVT_WARNINGS})

# Device self-test (UHD search results, device arguments, sample rate rules)
add_executable(device_check tools/device_check.cpp src/siggen/siggen.cpp src/siggen/siggen_out.cpp src/common/json.cpp src/io/recorder.cpp
  src/device/sdr_device.cpp src/device/sdr_sim.cpp src/device/sdr_siggen.cpp src/io/iq_file.cpp src/dsp/dsp.cpp src/common/ui_util.cpp src/common/log.cpp)
target_include_directories(device_check PRIVATE src)
target_link_libraries(device_check PRIVATE imgui_implot Threads::Threads)
target_compile_options(device_check PRIVATE ${WVT_WARNINGS})

# BB60 backend self-test against the simulated device (only with WVT_BB_SIM)
if(WVT_BB_SIM)
  add_executable(bb_check tools/bb_check.cpp src/device/sdr_bb60.cpp src/device/live.cpp src/io/recorder.cpp src/device/sdr_device.cpp
    src/device/sdr_sim.cpp src/device/sdr_siggen.cpp src/siggen/siggen.cpp src/siggen/siggen_out.cpp src/common/json.cpp src/dsp/engine.cpp src/io/iq_file.cpp
    src/dsp/dsp.cpp src/common/ui_util.cpp src/common/log.cpp)
  target_include_directories(bb_check PRIVATE src)
  target_compile_definitions(bb_check PRIVATE WVT_HAVE_BB)
  target_link_libraries(bb_check PRIVATE bb_api_sim wvt_gpu imgui_implot Threads::Threads)
  target_compile_options(bb_check PRIVATE ${WVT_WARNINGS})
endif()

# Sidekiq self-test (issue #4): the logic without the SDK in every build, and
# the backend against the simulated card with WVT_SKIQ_SIM
add_executable(skiq_check tools/skiq_check.cpp src/device/sdr_device.cpp src/device/sdr_sim.cpp
  src/device/sdr_siggen.cpp src/siggen/siggen.cpp src/siggen/siggen_out.cpp src/common/json.cpp src/io/recorder.cpp
  src/io/iq_file.cpp src/dsp/dsp.cpp src/common/ui_util.cpp src/common/log.cpp)
target_include_directories(skiq_check PRIVATE src)
target_link_libraries(skiq_check PRIVATE imgui_implot Threads::Threads)
target_compile_options(skiq_check PRIVATE ${WVT_WARNINGS})
if(WVT_SKIQ_SIM)
  target_sources(skiq_check PRIVATE src/device/sdr_skiq.cpp)
  target_compile_definitions(skiq_check PRIVATE WVT_HAVE_SKIQ)
  target_link_libraries(skiq_check PRIVATE skiq_api_sim)
endif()

# ---------------------------------------------------------------------------
# CTest (issue #53): "ctest --test-dir build --output-on-failure" runs every
# self-check. The checks need no hardware.
# ---------------------------------------------------------------------------
enable_testing()
set(WVT_CHECKS siggen_check audio_check log_check segment_check gpu_check extract_check snapshot_check device_check
  clipboard_check session_check measure_check config_check skiq_check)
if(WVT_BB_SIM)
  list(APPEND WVT_CHECKS bb_check)
endif()
foreach(_t IN LISTS WVT_CHECKS)
  add_test(NAME ${_t} COMMAND ${_t})
endforeach()
set_tests_properties(clipboard_check PROPERTIES SKIP_RETURN_CODE 77)
# Open a session and save it again through wvt itself (issue #76); needs an X server.
add_test(NAME session_roundtrip COMMAND ${CMAKE_COMMAND} -DWVT=$<TARGET_FILE:wvt> -DIQGEN=$<TARGET_FILE:iqgen>
  -DDIR=${CMAKE_BINARY_DIR}/session_roundtrip -P ${CMAKE_SOURCE_DIR}/tools/session_roundtrip.cmake)
set_tests_properties(session_roundtrip PROPERTIES SKIP_REGULAR_EXPRESSION "skipped")
# The configuration file (issue #88): the defaults written and shown without a file.
add_test(NAME config_write COMMAND wvt --no-log-file --no-config --write-config -)
add_test(NAME config_show COMMAND wvt --no-log-file --no-config --show-config)
# The option table: --help fails if an option name is defined twice.
add_test(NAME cli_help COMMAND wvt --no-log-file --help)
# --tx-dry-run checks the transmit limits and opens no device (issue #49).
add_test(NAME tx_dry_run COMMAND wvt --no-log-file --gen cw -c 915M -r 1M --tx-gain 10 --tx-max-gain 20
  --tx-allow 902M:928M --tx-dry-run)
add_test(NAME tx_dry_run_refused COMMAND wvt --no-log-file --gen cw -c 927.8M -r 1M --tx-allow 902M:928M --tx-dry-run)
set_tests_properties(tx_dry_run_refused PROPERTIES WILL_FAIL TRUE)

# ---------------------------------------------------------------------------
# libFuzzer targets for the parsers of user-supplied text (issue #53), clang
# only. Each target is built with ASan and UBSan. CTest runs each one over its
# seed inputs in tools/fuzz/corpus; to fuzz, refer to docs/self-checks.md.
# ---------------------------------------------------------------------------
option(WVT_FUZZ "Build the libFuzzer targets (clang only)" OFF)
if(WVT_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "WVT_FUZZ=ON needs clang (libFuzzer); this compiler is ${CMAKE_CXX_COMPILER_ID}")
  endif()
  set(_fuzz_flags -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  # name: sources beyond the target's own .cpp
  set(_fuzz_json src/common/json.cpp)
  set(_fuzz_chain src/siggen/siggen.cpp src/siggen/siggen_out.cpp src/common/json.cpp src/io/recorder.cpp src/device/sdr_device.cpp src/device/sdr_sim.cpp
    src/device/sdr_siggen.cpp src/io/iq_file.cpp src/dsp/dsp.cpp src/common/ui_util.cpp src/common/log.cpp)
  set(_fuzz_meta src/io/iq_file.cpp src/common/json.cpp src/common/log.cpp)
  set(_fuzz_parse_si src/common/ui_util.cpp src/common/log.cpp)
  set(_fuzz_config src/app/config.cpp)
  foreach(_f json chain meta parse_si config)
    add_executable(fuzz_${_f} tools/fuzz/fuzz_${_f}.cpp ${_fuzz_${_f}})
    target_include_directories(fuzz_${_f} PRIVATE src tools/fuzz)
    target_compile_options(fuzz_${_f} PRIVATE ${_fuzz_flags} ${WVT_WARNINGS})
    target_link_options(fuzz_${_f} PRIVATE ${_fuzz_flags})
    target_link_libraries(fuzz_${_f} PRIVATE Threads::Threads)
    if(_f STREQUAL "chain" OR _f STREQUAL "parse_si")
      target_link_libraries(fuzz_${_f} PRIVATE imgui_implot)
    endif()
    file(GLOB _seeds "${CMAKE_SOURCE_DIR}/tools/fuzz/corpus/${_f}/*")
    add_test(NAME fuzz_${_f}_seeds COMMAND fuzz_${_f} ${_seeds})
  endforeach()
  message(STATUS "wvt: libFuzzer targets enabled (fuzz_json, fuzz_chain, fuzz_meta, fuzz_parse_si, fuzz_config)")
endif()
