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
  ${FONT_CPP})
target_include_directories(wvt PRIVATE src ${PLATFORM_INCLUDES})
target_link_libraries(wvt PRIVATE imgui_implot ${PLATFORM_LIBS} Threads::Threads)
target_compile_options(wvt PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Synthetic test-signal generator
add_executable(iqgen tools/iqgen.cpp)
