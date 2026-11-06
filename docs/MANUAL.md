# wvt User Manual

wvt is an X11 desktop tool for raw IQ recordings from software-defined radios (SDRs).

- The left side of the window shows a spectrum and a waterfall.
- The right side shows I/Q, AM, FM and PM plots. These plots share one time axis.
- When you pan or zoom one plot, all the other plots follow.

wvt has these other functions:

- **Live devices:** wvt streams from an RTL-SDR v3, a USRP (B200 family, N3xx, X3xx, X4xx), a
  Signal Hound BB60 or an Epiq Sidekiq. It shows the spectrum and the waterfall in real time, and
  it records the stream to a file. It can send the stream over the network to another wvt or to
  another program.
- **Signal generator:** wvt makes CW, pulsed, swept and recorded waveforms. It writes them to a
  file, shows them in the live view, or transmits them with a USRP.
- **Audio:** wvt demodulates one channel and plays it through the speakers or writes it to a WAV
  file.
- **Region export:** wvt writes a time range, and as an option a frequency band, to a new file.
- **Signal analysis:** wvt measures a part of a recording, finds and measures pulses, finds the
  symbol rate, and demodulates OOK, FSK, BPSK and QPSK to bits.
- **GPU processing:** wvt can do the signal processing on an NVIDIA GPU (CUDA) or an Apple GPU
  (Metal).

This manual has four parts:

1. [Getting started](#part-1-getting-started): install the packages, build wvt and open your
   first recording. The [Build reference](#build-reference) gives all the build settings.
2. [Front end](#part-2-front-end): the window and each tool in it.
3. [Back end](#part-3-back-end): the log, the configuration file and the command line.
4. [In the weeds](#part-4-in-the-weeds): the signal processing, the analysis calculations, the
   GPU, file sessions and the ZMQ message formats.

## Part 1: Getting started

### Install and build

#### Requirements

- A C++20 compiler.
- CMake 3.16 or later. A CUDA build needs CMake 3.18 or later.
- The X11 and OpenGL headers and libraries.

The repository contains Dear ImGui 1.91.9, ImPlot 0.16, miniaudio 0.11.25 and stb_image_write 1.16
in `third_party/`. Thus the build does not need network access. The UI font is in the binary.

#### Install the packages

Install the compiler, CMake and the X11 and OpenGL packages for your system:

| System | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential cmake libx11-dev libgl1-mesa-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake libX11-devel mesa-libGL-devel` |
| Arch | `sudo pacman -S base-devel cmake libx11 mesa` |
| macOS | Install [XQuartz](https://www.xquartz.org) and the Xcode Command Line Tools |

On Linux, also install `zenity` (for example, `sudo apt install zenity`) to choose files in a
dialog. Without it, you type the paths. Refer to [WVT_WITH_NFD](#wvt_with_nfd).

On macOS, the build links `/opt/X11/lib/libX11` and `libGL` from XQuartz. Thus the binary is a
real X11 client. On Apple silicon, use the ARM Homebrew (`/opt/homebrew`) for CMake and the
optional libraries, so that the binary is native ARM. The Intel Homebrew (`/usr/local`) makes an
x86-64 binary that runs under Rosetta. That binary cannot use the Signal Hound BB60 library.

#### Install the device libraries

Live streaming needs librtlsdr (for an RTL-SDR), UHD (for a USRP), or both. Install them before
you configure the build. CMake finds them automatically.

| System | RTL-SDR | UHD (USRP) |
|---|---|---|
| Debian / Ubuntu | `sudo apt install librtlsdr-dev` | `sudo apt install libuhd-dev uhd-host` |
| Fedora | `sudo dnf install rtl-sdr-devel` | `sudo dnf install uhd-devel` |
| Arch | `sudo pacman -S rtl-sdr` | `sudo pacman -S libuhd` |
| macOS | `brew install librtlsdr` | `brew install uhd` |

The Signal Hound BB60 needs the Signal Hound SDK. Refer to [Signal Hound BB60](#signal-hound-bb60).
The Epiq Sidekiq needs the Sidekiq SDK, on Linux only. Refer to [Epiq Sidekiq](#epiq-sidekiq). For
an NVIDIA GPU, install the CUDA toolkit. Refer to [GPU processing](#gpu-processing).

Without these libraries, wvt still builds. It then has the file viewer, recording from the
simulator, and the signal generator.

#### Build wvt

Do these steps in the top folder of the repository:

1. Configure the build:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   ```

2. Read the `wvt:` lines in the CMake output. Each line tells you which optional function the
   build has, and why a function is not available.
3. Compile:

   ```sh
   cmake --build build -j
   ```

The build puts the `wvt` program and the self-check programs in `build/`. To build for one CPU
architecture with one command, use [the build script](#the-build-script).

### First run

#### Make a test recording

If you do not have a recording, make one with the `iqgen` tool:

```sh
./build/iqgen out.sigmf-data --fmt ci16
```

This command writes a 2 s recording at 2 MS/s. The recording has these signals:

| Offset | Signal |
|---|---|
| +300 kHz | FM, 1 kHz tone, 50 kHz deviation, slow fade |
| −250 kHz | AM, 400 Hz tone, 60 % depth |
| −600 kHz | PM, 2 rad sine, plus 90° steps each 20 ms |
| +650 kHz | 2-FSK bursts |
| +100 kHz | A weak CW tone |
| — | A chirp from 1.0 s to 1.4 s |

To stream synthetic signals without hardware, use the `sim` device: `wvt --device sim -c 100M -r 2.4M`.

#### Open the recording

```sh
./build/wvt out.sigmf-meta
```

wvt reads the sample format, the sample rate and the center frequency from the SigMF metadata. For
the other ways that wvt finds these values, refer to [The file viewer](#the-file-viewer).

To examine the FM signal with the channel filter on and a short view, type:

```sh
./build/wvt out.sigmf-meta --chan 300k:150k --view 0.5:0.505
```

To pan, drag a plot. To zoom, turn the mouse wheel. For all the controls, refer to
[Mouse and keyboard](#mouse-and-keyboard), or select **Help → Keyboard and mouse** in the window.

#### Remote and virtual displays

wvt uses GLX and OpenGL when the X server supports them. If not, wvt uses its built-in software
renderer. Thus wvt also works over `ssh -X`, in Xvfb, and in VNC sessions without GL. To use the
software renderer always, start wvt with `--software`.

### Build reference

This chapter gives all the CMake settings of wvt, what each setting enables or disables, and the
programs that the build makes.

#### Configure and compile

```sh
cmake -S . -B build [OPTIONS]
cmake --build build -j
```

Give each option as `-DNAME=VALUE`. CMake keeps the values in `build/CMakeCache.txt`. Thus, when
you configure the same build folder again, you do not have to give the options again. To go back
to the default of an option, delete `build/CMakeCache.txt`, or give the default value.

#### The build script

`scripts/build.sh` does the two steps above for one CPU architecture. It builds in the folder
`build.<arch>` at the top of the source tree, for example `build.arm64` or `build.x86_64`. A
sanitizer build goes in `build.<arch>.<sanitizer>`, for example `build.arm64.thread`.

```sh
scripts/build.sh [options] [-- extra CMake arguments]
```

| Option | Function |
|---|---|
| `-a`, `--arch ARCH` | `x86_64` or `arm64` (`amd64` and `aarch64` are also accepted). The default is the architecture of the computer |
| `-t`, `--type TYPE` | `Release` (default), `Debug`, `RelWithDebInfo` or `MinSizeRel` |
| `-j`, `--jobs N` | The number of parallel compile jobs. The default is the number of CPUs |
| `-B`, `--build-dir DIR` | Use `DIR`, not `build.<arch>` |
| `--toolchain FILE` | A CMake toolchain file. On Linux, a build for a different architecture needs one |
| `--clean` | Delete the build folder first |
| `--test` | Run the self-checks (`ctest`) after the build |
| `-n`, `--dry-run` | Show the commands, but do not run them |
| `--no-rtlsdr`, `--no-uhd`, `--no-bb`, `--no-skiq`, `--no-audio`, `--no-cuda`, `--no-metal` | Build without the feature (`WVT_WITH_...=OFF`) |
| `--with-rtlsdr`, `--with-uhd`, `--with-bb`, `--with-skiq`, `--with-audio`, `--with-cuda`, `--with-metal` | The feature must be in the build. The configure step stops if its library is missing (`WVT_WITH_...=ON`) |
| `--no-gpu` | The same as `--no-cuda --no-metal` |
| `--minimal` | Build without all the features: the file viewer, the simulator and the signal generator only |
| `--werror`, `--bb-sim`, `--fuzz` | `WVT_WERROR=ON`, `WVT_BB_SIM=ON`, `WVT_FUZZ=ON` |
| `--sanitize KIND` | `address` or `thread` (`WVT_SANITIZE`) |

Without a `--no-` or `--with-` option, a feature is in the build if CMake finds its library. This
is the same as a `cmake` build without options.

To build for a different architecture:

- **macOS** compiles for it directly (`CMAKE_OSX_ARCHITECTURES`). An `x86_64` build runs on Apple
  silicon with Rosetta. But the Homebrew libraries are for one architecture only. Thus build without
  the features that use them (for example `--minimal`), or install the libraries for the other
  architecture.
- **Linux** needs a cross compiler and libraries for the other architecture. Give them with
  `--toolchain FILE`.

Examples:

```sh
scripts/build.sh                                   # this computer, all the features that are found
scripts/build.sh --no-cuda --no-uhd --test         # two features off, then the self-checks
scripts/build.sh --arch x86_64 --minimal           # macOS: an Intel build on Apple silicon
scripts/build.sh --sanitize thread --type RelWithDebInfo
scripts/build.sh -- -DCMAKE_CUDA_ARCHITECTURES=89  # extra CMake arguments
```

#### Build options

| Option | Values | Default | Platform |
|---|---|---|---|
| [`CMAKE_BUILD_TYPE`](#cmake_build_type) | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel` | `Release` | All |
| [`X11_ROOT`](#x11_root) | A folder | `/opt/X11` | macOS |
| [`WVT_WITH_RTLSDR`](#wvt_with_rtlsdr) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_UHD`](#wvt_with_uhd) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_BB`](#wvt_with_bb) | `AUTO`, `ON`, `OFF` | `AUTO` | Linux, macOS ARM |
| [`SIGNALHOUND_SDK_DIR`](#signalhound_sdk_dir) | A folder | Empty | Linux, macOS ARM |
| [`WVT_BB_SIM`](#wvt_bb_sim) | `ON`, `OFF` | `OFF` | All |
| [`WVT_WITH_SKIQ`](#wvt_with_skiq) | `AUTO`, `ON`, `OFF` | `AUTO` | Linux |
| [`SIDEKIQ_SDK_DIR`](#sidekiq_sdk_dir) | A folder | Empty | Linux |
| [`WVT_SKIQ_SIM`](#wvt_skiq_sim) | `ON`, `OFF` | `OFF` | All |
| [`WVT_WITH_ZMQ`](#wvt_with_zmq) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_ZMQ_SOURCE_DIR`](#wvt_zmq_source_dir) | A folder | Empty | All |
| [`WVT_WITH_AUDIO`](#wvt_with_audio) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_NFD`](#wvt_with_nfd) | `ON`, `OFF` | `ON` | All |
| [`WVT_WITH_CUDA`](#wvt_with_cuda) | `AUTO`, `ON`, `OFF` | `AUTO` | Linux |
| [`WVT_CUDA_STATIC`](#wvt_cuda_static) | `ON`, `OFF` | `ON` | Linux |
| [`CMAKE_CUDA_ARCHITECTURES`](#cmake_cuda_architectures) | A list of GPU architectures | Refer to the section | Linux |
| [`WVT_WITH_METAL`](#wvt_with_metal) | `AUTO`, `ON`, `OFF` | `AUTO` | macOS |
| [`WVT_WERROR`](#wvt_werror) | `ON`, `OFF` | `OFF` | All |
| [`WVT_SANITIZE`](#wvt_sanitize) | `address`, `thread` or empty | Empty | All |
| [`WVT_FUZZ`](#wvt_fuzz) | `ON`, `OFF` | `OFF` | All (clang only) |

The `AUTO`, `ON` and `OFF` values have the same meaning for all options:

AUTO
: CMake builds the function if it finds the necessary library or toolkit. If not, CMake builds
  wvt without the function and tells you why.

ON
: CMake must build the function. If the library or toolkit is missing, the configure step stops
  with an error.

OFF
: CMake does not build the function. CMake does not look for the library.

Use `ON` in scripts and CI jobs, where a missing library must cause an error.

##### CMAKE_BUILD_TYPE

Sets the compiler optimization. If you do not give a value, wvt uses `Release`.

- `Release` compiles with `-O3 -DNDEBUG` (the CMake default for GCC and clang). Use this value for
  normal use. The waterfall and the demodulators are much slower without optimization.
- `Debug` compiles with `-g` and no optimization. Use it with a debugger.
- `RelWithDebInfo` compiles with optimization and debug symbols.

##### X11_ROOT

macOS only. Sets the folder of XQuartz. The default is `/opt/X11`. The build links
`${X11_ROOT}/lib/libX11.dylib` and `${X11_ROOT}/lib/libGL.dylib`, and uses the headers in
`${X11_ROOT}/include`. If `libX11.dylib` is not in this folder, the configure step stops.

On Linux, CMake finds X11 and OpenGL with its standard `FindX11` and `FindOpenGL` modules. The
build uses the legacy `libGL.so`, which has the GL and the GLX functions.

##### WVT_WITH_RTLSDR

Enables or disables the RTL-SDR device backend (`src/device/sdr_rtlsdr.cpp`).

- CMake finds librtlsdr with `pkg-config` (the `librtlsdr` module).
- CMake also checks for `rtlsdr_set_bias_tee()`. This function is in librtlsdr builds from 2018
  and later. Without it, the **bias-tee** option is not available.
- Without this backend, the device list does not show RTL-SDR devices.

Output: `wvt: RTL-SDR support enabled (librtlsdr <version>)`.

##### WVT_WITH_UHD

Enables or disables the USRP backend (`src/device/sdr_uhd.cpp`). This backend receives and
transmits with all USRPs that UHD supports: the USB B200 family and the network N3xx, X3xx and X4xx.

- CMake finds UHD with `pkg-config` (the `uhd` module). If `pkg-config` does not find it, CMake
  uses the `UHDConfig.cmake` file of UHD.
- wvt uses the C API of UHD (`uhd.h`). Thus the build does not need the Boost headers.
- Without this backend, wvt cannot receive from a USRP, and the signal generator cannot transmit.

Output: `wvt: UHD support enabled (UHD <version>)`.

##### WVT_WITH_BB

Enables or disables the Signal Hound BB60 backend (`src/device/sdr_bb60.cpp`). It needs `bb_api`
from the Signal Hound SDK. Refer to [Signal Hound BB60](#signal-hound-bb60).

- CMake looks for `bb_api.h` and the library in `SIGNALHOUND_SDK_DIR`. It uses the library for the
  CPU of the build: Linux x86-64 (the newest version 5 library), Linux ARM64, or macOS ARM.
- CMake copies the library into `build/bb_api/` under its run-time name. wvt finds it there
  through its rpath. On Linux x86-64, CMake also copies `libftd2xx.so`.
- An x86-64 macOS build cannot use the ARM library. CMake then tells you to configure with
  `-DCMAKE_OSX_ARCHITECTURES=arm64`.
- Without this backend, the device list does not show BB60 devices.

Output: `wvt: Signal Hound BB60 support enabled (bb_api 5.0.x, <library>)`.

##### SIGNALHOUND_SDK_DIR

The folder of the extracted Signal Hound SDK, for example `$HOME/signal_hound_sdk`. A prefix
with `include/bb_api.h` and `lib/libbb_api.*` also works. If the library is installed in a system
folder, such as `/usr/local/lib`, CMake finds it without this option.

##### WVT_BB_SIM

`ON` builds the BB60 backend against a simulated BB60D (`tools/bb_sim/`) instead of the SDK. Use
it for CI and tests. The build then also makes `bb_check`, which tests the backend. Do not use it
for normal use, because the device list then shows the simulated BB60.

##### WVT_WITH_SKIQ

Enables or disables the Epiq Sidekiq backend (`src/device/sdr_skiq.cpp`). It needs `libsidekiq`
from the Sidekiq SDK, version 4.14.0 or later. Refer to [Epiq Sidekiq](#epiq-sidekiq).

- CMake uses the first SDK folder in this list: `SIDEKIQ_SDK_DIR`, the environment variable
  `SIDEKIQ_SDK_DIR`, `~/sidekiq_sdk_current`, the newest `third_party/sidekiq_sdk/sidekiq_sdk_v*`.
- CMake runs `bin/sidekiq-config` of the SDK for the version, the include folder and the static
  link flags. It keeps the link flags in their order. If an SDK has no `sidekiq-config`, CMake
  links `lib/libsidekiq__<cpu>.a` and the support libraries in `lib/support/`.
- If the system has libusb, glib and tirpc (`libusb-1.0-0-dev`, `libglib2.0-dev`,
  `libtirpc-dev`), CMake links them instead of the SDK copies in `lib/support/`. The SDK has
  libusb 1.0.20, and librtlsdr and UHD need 1.0.21 or later.
- The SDK is for Linux only. On macOS, the backend is not built.
- Without this backend, the device list does not show Sidekiq cards.

Output: `wvt: Sidekiq support enabled (SDK 4.26.0, <folder>)`.

##### SIDEKIQ_SDK_DIR

The folder of the extracted Sidekiq SDK: the folder with `bin/sidekiq-config` and
`sidekiq_core/inc/sidekiq_api.h`, for example `$HOME/sidekiq_sdk_v4.26.0`.

##### WVT_SKIQ_SIM

`ON` builds the Sidekiq backend against a simulated card (`tools/skiq_sim/`) instead of the SDK,
also on macOS. Use it for CI and tests. `skiq_check` then also tests the backend. Do not use it
for normal use, because the device list then shows the simulated card.

##### WVT_WITH_ZMQ

Enables or disables streaming over the network with ZMQ. The built-in stream uses the RADIO and
DISH sockets on UDP. These sockets are in the draft API of libzmq. Most libzmq packages (Homebrew,
Debian, Fedora) do not have the draft API. CMake gets libzmq in this order:

1. A system libzmq (`pkg-config libzmq`), if it has the draft API.
2. The libzmq source in [`WVT_ZMQ_SOURCE_DIR`](#wvt_zmq_source_dir).
3. The libzmq 4.3.5 release from GitHub. CMake downloads it to `build/_deps/` and checks its
   SHA-256 before it uses it. The configure step needs the network the first time only.

For 2 and 3, CMake builds a static libzmq with the draft API, without CURVE, WebSocket or TLS.
libzmq shows one CMake warning about its old `cmake_minimum_required`. You can ignore it.

- With `AUTO`, if the download fails (for example, on a computer without a network), wvt builds
  without ZMQ and CMake tells you why.
- Without ZMQ, the self-check `zmq_check` tests the message format only.

Output: `wvt: ZMQ streaming enabled (libzmq 4.3.5, static, draft API, built here)`.

##### WVT_ZMQ_SOURCE_DIR

An extracted libzmq 4.3.x release, for a build without a network. For example:

```sh
curl -LO https://github.com/zeromq/libzmq/releases/download/v4.3.5/zeromq-4.3.5.tar.gz
shasum -a 256 zeromq-4.3.5.tar.gz   # 6653ef5910f17954861fe72332e68b03ca6e4d9c7160eb3a8de5a5a913bfab43
tar -xzf zeromq-4.3.5.tar.gz
cmake -S . -B build -DWVT_ZMQ_SOURCE_DIR=$PWD/zeromq-4.3.5
```

CMake does not check this folder. Use a release that you checked.

##### WVT_WITH_AUDIO

Enables or disables audio playback through the bundled miniaudio library.

- On macOS, the build links CoreAudio, AudioToolbox and CoreFoundation.
- On Linux, the build does not need development packages. At run time, miniaudio loads
  PipeWire, PulseAudio or ALSA, whichever is available.
- `AUTO` and `ON` have the same result, because the library is in the repository.
- With `OFF`, the audio window cannot play sound. The demodulator and the WAV export still work.

Output: `wvt: audio playback enabled (miniaudio)`.

##### WVT_WITH_NFD

Enables or disables the file dialogs through the bundled Native File Dialog library
(`third_party/nativefiledialog`). With the dialogs, each path box has a **Browse...** or
**Folder...** button, and **File → Open file...** opens a dialog.

- On macOS, the build links AppKit. The dialogs are the macOS open and save panels.
- On Linux, the build does not need development packages. At run time, wvt runs the `zenity`
  program for the dialogs (for example, `sudo apt install zenity`). If wvt does not find `zenity`,
  the buttons are disabled and their tooltips tell you to install it.
- With `OFF`, or without `zenity`, you type the paths in the boxes.

A dialog stops the window until you close it. Streams, recordings, audio and transmissions
continue.

Output: `wvt: native file dialogs enabled (Cocoa)` or
`wvt: native file dialogs enabled (zenity, found at run time)`.

##### WVT_WITH_CUDA

Enables or disables the CUDA GPU backend (`src/gpu/gpu_cuda.cu`) for NVIDIA GPUs. Refer to
[GPU processing](#gpu-processing).

The backend needs all of these conditions:

- Linux. CUDA is not available on macOS.
- CMake 3.18 or later.
- The CUDA toolkit 12.0 or later, with `nvcc` and cuFFT. For RTX 50-series GPUs, use CUDA 12.8 or
  later.

If CMake does not find `nvcc` on the `PATH`, give its location with
`-DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc`.

A CUDA build also runs on a computer without an NVIDIA GPU or driver. wvt then uses the CPU, and
the log tells you why.

Output: `wvt: CUDA support enabled (CUDA 12.x, archs ..., static cuFFT)`.

##### WVT_CUDA_STATIC

Sets how the build links the CUDA runtime and cuFFT. Applies only when the CUDA backend builds.

- `ON` (default) links the CUDA runtime and cuFFT into the binary. The binary is approximately
  270 MB larger. It needs only the NVIDIA driver at run time.
- `OFF` links the shared libraries. The binary is small, but the computer that runs it must have
  the CUDA runtime and cuFFT libraries.

##### CMAKE_CUDA_ARCHITECTURES

Sets the GPU architectures that `nvcc` compiles the CUDA code for. Applies only when the CUDA backend
builds. If you do not give a value, wvt uses:

| CUDA version | Architectures | GPUs |
|---|---|---|
| 12.0 to 12.7 | `75-real 80-real 86-real 89-real 90` | RTX 20xx to 40xx, A100, H100, plus PTX for newer GPUs |
| 12.8 and later | `75-real 80-real 86-real 89-real 90-real 120` | The same, plus RTX 50xx |

A shorter list makes a smaller binary and a faster build. For example, for an RTX 4090 only, use
`-DCMAKE_CUDA_ARCHITECTURES=89`. The GPU must have compute capability 7.5 or later.

##### WVT_WITH_METAL

macOS only. Enables or disables the Metal GPU backend (`src/gpu/gpu_metal.mm`) for Apple GPUs.
Refer to [GPU processing](#gpu-processing).

- The backend needs an Objective-C++ compiler and the Metal and MetalPerformanceShadersGraph
  frameworks. The Command Line Tools have all of them. You do not need Xcode.
- On Linux, `ON` causes an error, and `AUTO` disables the backend.

Output: `wvt: Metal support enabled (MPSGraph FFT; the GPU path needs macOS 14 at run time)`.

##### WVT_WERROR

`ON` makes the compiler warnings in the wvt code errors (`-Werror`). The code in `third_party/` is
not included. All CI build jobs use `ON`. Thus a change that adds a warning fails CI. All the wvt
targets compile with `-Wall -Wextra -Wno-unused-parameter`.

To add your own compiler flags, use `CMAKE_CXX_FLAGS` or `CMAKE_CXX_FLAGS_RELEASE`. wvt does not
replace them.

##### WVT_SANITIZE

Builds all the programs with compiler sanitizers. Use it for tests, not for normal use, because
the programs are slower and use more memory.

- `address`: AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`). They
  find out-of-bounds access, use after free, memory leaks (Linux) and undefined behavior. An
  undefined-behavior error stops the program.
- `thread`: ThreadSanitizer (`-fsanitize=thread`). It finds data races between threads. You cannot
  use it together with `address`.
- Empty (the default): no sanitizers.

The CUDA code (`gpu_cuda.cu`) does not get the sanitizer flags. Use a separate build folder for
each value.

Output: `wvt: sanitizers enabled (address: -fsanitize=address,undefined ...)`.

##### WVT_FUZZ

`ON` also builds the libFuzzer targets `fuzz_json`, `fuzz_chain`, `fuzz_meta`, `fuzz_parse_si`,
`fuzz_config`, `fuzz_zmq_wire` and `fuzz_zmq_format`, with AddressSanitizer and
UndefinedBehaviorSanitizer. CTest then also runs each target on its seed inputs in
`tools/fuzz/corpus/`. The option needs clang. With a different compiler, the configure step stops
with an error. On macOS, use the clang of the Homebrew `llvm` package, because Apple clang has no
libFuzzer.

To fuzz one parser for 10 minutes, use a copy of its seed inputs. libFuzzer adds new inputs to
the first folder that you give it, and these inputs must not go into the repository:

```sh
cp -r tools/fuzz/corpus/chain /tmp/corpus-chain
./build-fuzz/fuzz_chain -max_total_time=600 -max_len=4096 /tmp/corpus-chain
```

If the program finds a problem, it stops with a report and writes the input to a `crash-...` file.
To run that input again, type `./build-fuzz/fuzz_chain crash-...`.

##### The CPU FFT

The CPU FFT is not an option. On macOS, the build always links the Accelerate framework and uses
its vDSP FFT. On Linux, wvt uses its own radix-2 FFT.

#### Build examples

A Linux build with the two device backends. The configure step stops if one is missing:

```sh
cmake -S . -B build -DWVT_WITH_RTLSDR=ON -DWVT_WITH_UHD=ON
```

A minimum build, with the file viewer, the simulator and the signal generator file output only:

```sh
cmake -S . -B build -DWVT_WITH_RTLSDR=OFF -DWVT_WITH_UHD=OFF -DWVT_WITH_AUDIO=OFF \
      -DWVT_WITH_CUDA=OFF -DWVT_WITH_METAL=OFF
```

A CUDA build for one GPU type, with shared CUDA libraries:

```sh
cmake -S . -B build -DWVT_WITH_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 -DWVT_CUDA_STATIC=OFF
```

A build for ThreadSanitizer, in a separate folder:

```sh
cmake -S . -B build-tsan -DWVT_SANITIZE=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

A debug build:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
```

#### Build targets

`cmake --build build` makes all these programs. To make one program only, add `--target NAME`.

| Target | Contents |
|---|---|
| `wvt` | The application |
| `iqgen` | Writes a synthetic test recording. Refer to [Make a test recording](#make-a-test-recording) |
| `siggen_check` | Self-check for the signal generator |
| `audio_check` | Self-check for the audio demodulator |
| `log_check` | Self-check for the logger |
| `segment_check` | Self-check for the time segment rules |
| `extract_check` | Self-check for the region export |
| `snapshot_check` | Self-check for the plot snapshots |
| `session_check` | Self-check for the file sessions |
| `config_check` | Self-check for the configuration file parser |
| `measure_check` | Self-check for the point measurements |
| `analysis_check` | Self-check for the tools of the Analysis window |
| `clipboard_check` | Self-check for the system clipboard. It needs an X server. Without one, CTest skips it |
| `device_check` | Self-check for the device list and the sample rate rules |
| `bb_check` | Self-check for the BB60 backend, with the simulated device. Only with `WVT_BB_SIM=ON` |
| `skiq_check` | Self-check for the Sidekiq logic. With `WVT_SKIQ_SIM=ON`, also for the backend with the simulated card |
| `zmq_check` | Self-check for the network stream format. With ZMQ ([`WVT_WITH_ZMQ`](#wvt_with_zmq)), also for the sockets |
| `gpu_check` | Compares the GPU results with the CPU results, and measures the speed. Refer to [Check the GPU](#check-the-gpu) |
| `fuzz_json`, `fuzz_chain`, `fuzz_meta`, `fuzz_parse_si`, `fuzz_config`, `fuzz_zmq_wire`, `fuzz_zmq_format` | The fuzz targets. Only with `WVT_FUZZ=ON` |

The self-checks do not need hardware. To run all of them, type
`ctest --test-dir build --output-on-failure`. To run one of them, add `-R NAME`, for example
`-R extract_check`.

#### Version information

At each build, CMake reads two values from git:

- **Build:** the short commit hash, for example `523ac6c`. If tracked files have changes that are
  not committed, CMake adds `-dirty`. Outside a git checkout (for example, a source archive), the
  value is `unknown`.
- **Version:** the version tag on that commit, for example `v1.1`. If the commit has no tag, the
  value is blank.

**Help → About wvt** shows the two values. wvt also writes them to the start of each log file.
You do not have to configure again after a commit or a new tag, because the next build updates
the values.

#### Build the documentation

This manual uses Sphinx with the MyST Markdown parser. To make the HTML pages:

```sh
pip install -r docs/requirements.txt
sphinx-build -W -b html docs build/docs
```

Open `build/docs/index.html` in a browser. The `-W` option makes each warning an error. CI uses
the same command.

## Part 2: Front end

### The file viewer

The file viewer shows a raw IQ recording. This chapter tells you how wvt opens a file, and what
each area of the window shows.

#### Open a file

Give the file on the command line, or use one of these procedures:

- Click **File → Open file...** or **Browse...** in the file row, and select the file in the
  dialog. On Linux, the dialog needs the `zenity` program. Refer to [WVT_WITH_NFD](#wvt_with_nfd).
- Type the path in the file row and click **Open**.

On the command line:

```sh
wvt recording.sigmf-meta
```

To open a saved session (the recording and the view, plots and channel that you saved), open the
session file in the same way. Refer to [File sessions](#file-sessions).

#### Sample formats

wvt reads these sample formats. Add `_be` for big-endian samples, for example `ci16_be`.

| Format | Sample |
|---|---|
| `cf32` | Complex 32-bit float |
| `cf64` | Complex 64-bit float |
| `ci32` | Complex 32-bit signed integer |
| `ci16` | Complex 16-bit signed integer |
| `ci8` | Complex 8-bit signed integer |
| `cu8` | Complex 8-bit unsigned integer (RTL-SDR) |

#### How wvt finds the file settings

wvt gets the sample format, the sample rate and the center frequency from these sources, in this
order:

1. **SigMF** `.sigmf-meta` and `.sigmf-data` files.
2. **gqrx** file names, for example `gqrx_YYYYMMDD_HHMMSS_<fc>_<rate>_fc.raw`.
3. Values in the file name, for example `2.4MSps` or `915MHz`.
4. The file extension, for example `.cu8`, `.cs8`, `.cs16`, `.cf32`, `.cfile` or `.raw`.

From SigMF, wvt reads `core:datatype` from `global`. It reads `core:sample_rate`,
`core:frequency` and `core:datetime` from `global` or from the first capture.

- The datatype must be a complex type in the table above (`_le` or `_be`).
- wvt does not open a SigMF file with a different datatype (for example `ri16_le`, real
  samples), because the plots then show noise. To read the file with a different format, give
  `-f`.
- If the metadata has no datatype, wvt reads the file as `cf32` and shows a warning.

The command-line options override all the values that wvt finds:

| Option | Value |
|---|---|
| `-f`, `--format FMT` | The sample format: `cf32`, `cf64`, `ci32`, `ci16`, `ci8` or `cu8`. Add `_be` for big-endian |
| `-r`, `--rate HZ` | The sample rate. SI suffixes are permitted, for example `2.4M` |
| `-c`, `--fc HZ` | The center frequency |
| `-H`, `--header BYTES` | The number of bytes to skip at the start of the file |
| `--swap-iq` | Swap I and Q |

While the file is open, you can change each value in the file row. The **Format** drop-down has
the sample format, the I/Q swap, the byte order and the header size.

#### Large files

wvt memory-maps the file. Thus wvt can open files that are larger than the memory of the
computer. For files that are very large, load a part of the file only. Refer to
[Load a segment](#load-a-segment).

wvt does not support a file that a different program still writes. If the file gets shorter
while it is open (for example, a recorder that starts the file again), wvt shows zeros after the
new end. The status bar then shows a warning. To see the new length, open the file again.

#### Screen layout

| Area | Contents |
|---|---|
| **Menu bar** | The **File**, **View**, **Tools** and **Help** menus. Refer to [Menus](#menus). On the right: **TX ON**, **REC**, a running audio or export, and the **Source** (File or Device) |
| **File row** | The file path, **Format**, sample rate, center frequency, segment (start and end time to load), and file size |
| **Navigation row** | Buttons to go to the start or the end, and to move back or forward by 10 % or by one page. Zoom in, zoom out and zoom all. The span presets, the span and the start time. **Go to** has a sample index, *Add bookmark* and the bookmarks |
| **Overview strip** | The power over the full file or segment. The white box is the current view. Drag the box to move the view. Drag its edges to change the view size. Click a different point to go there. You can hide the strip in the **View** menu. wvt also hides it when the window is short |
| **Scrollbar** | Moves the view through the file. Drag the thumb, click the track to move one page, or turn the mouse wheel |
| **Left** | The waterfall settings: FFT size, window, dB range or auto. The ⚙ button has the overlap, averaging, rows, columns, colormap and peak hold. Below the settings are the spectrum of the current view, the waterfall, and its color bar |
| **Right** | The channel settings: on or off, offset, bandwidth, and *to peak*. The ⚙ button has the taps, decimation, squelch, maximum points and plot height. Below the settings are the I/Q, AM, FM and PM plots, which you can scroll. Each plot header has a show checkbox, a ⚙ button for the plot options, and the measurement readout. Thus no text covers the traces |
| **Status bar** | The **Snapshot...** button (refer to [Plot snapshots](#plot-snapshots)). The values at the pointer: time, sample number, frequency, dB, AM, FM and PM. The marker ΔT, 1/ΔT and ΔF, and the point markers (P2 minus P1). `GPU <model>` or `CPU`, the renderer, and `computing... NN%` while wvt calculates the plots (the percentage done) |

#### Menus

| Menu | Items |
|---|---|
| **File** | Open file, export region, save plot snapshots, save file session, quit |
| **View** | Time markers, frequency markers, point markers, relative frequency, peak hold, the overview strip, the time plots to show, show everything |
| **Tools** | Signal generator, audio, analysis, use the GPU |
| **Help** | Keyboard and mouse, about wvt |

#### Linked views

- All time axes have one range: the vertical axis of the waterfall and the horizontal axis of each
  plot on the right.
- The spectrum and the waterfall have one frequency range.
- When the pointer is on a plot, all the other plots show a crosshair at the same time or
  frequency.
- The time markers (T1 and T2) show in all plots. You can drag them in each plot. Press `Z` to
  zoom to the span between them.

The spectrum above the waterfall shows the average and the peak hold of all the waterfall rows in
the view. When you pan or zoom, wvt calculates the spectrum again. For the calculation of each
plot, refer to [Signal processing](#signal-processing).

#### The channel filter and the time plots

The channel filter selects one signal for the I/Q, AM, FM and PM plots.

- To enable the filter, press `C`, or tick **Channel** in the channel settings.
- To tune the channel, Ctrl+click the spectrum or the waterfall.
- To move the channel, drag its center line (cyan). To change the bandwidth, drag an edge line.
- **To peak** puts the channel on the strongest signal in the visible spectrum.

To start with the filter on, use `--chan OFFSET:BW`, for example `--chan 300k:150k`. To start with
a time range, use `--view T0:T1`, for example `--view 0.5:0.52`.

Each time plot has options. To see them, click its ⚙ button, or right-click the plot.

#### Bookmarks and markers

- Press `B` to add a bookmark at the view. **Go to** shows the bookmarks.
- Press `T` to show or hide the time markers. Press `F` to show or hide the frequency markers.
- The status bar shows the time between the markers (ΔT), its inverse (1/ΔT) and the frequency
  difference (ΔF).

#### Point measurements

Two point markers, P1 and P2, measure between two points of the signal. For example, the time
between two pulse edges, the time between two pulses, or the change in frequency in a pulse.

- To put a marker at the pointer, press `M` or Alt+click a plot: the waterfall, the spectrum, I/Q,
  AM, FM or PM. The first marker is P1 and the second is P2. After that, each new marker replaces
  the older of the two.
- On a time plot (I/Q, AM, FM, PM), the marker goes on the trace. If a peak or a valley is less
  than 8 pixels to the side and nearer to the pointer, the marker goes there. Thus, put the pointer
  a little above the top of a pulse to put the marker on the top. On the I/Q plot, the marker goes
  on the trace (I or Q) that is nearer to the pointer.
- On the waterfall, the marker keeps the time and the frequency of the pointer. On the spectrum, it
  keeps the frequency.
- Without a pointer, press `Shift+P` (or use **View → Point at the next spectrum peak**). The
  marker goes on the strongest peak of the spectrum in view. Press it again to put the other
  marker on the next strongest peak. This gives the frequency and level difference between two
  signals.
- To move a marker, drag it. On a time plot, it moves along the trace.
- To remove the markers, press `Shift+M`, or use **View → Clear point markers**. **View → Point
  markers** shows or hides them.

The status bar shows P2 minus P1 for each value that the two markers have in common. The label at
P2 shows the same values:

| P1 and P2 on | Differences |
|---|---|
| The same time plot | dT (with 1/dT and the number of samples) and the value (for example dAM, dFM in Hz, dPM in rad or deg) |
| I and Q | dT and the amplitude (dA) |
| Two different time plots, or a time plot and the waterfall | dT |
| The waterfall | dT, dF and the power (dP, dB) |
| The spectrum | dF and the power (dP, dB) |
| The spectrum and the waterfall | dF |

The plots share the time axis and the frequency axis. Thus dT and dF are correct between two
plots. The other plots show each marker as a faint line at its time or frequency.

A value comes from the plotted data. When a marker on a time plot goes out of the view, wvt keeps
its last value until a plot setting changes. A value that wvt does not know shows as `?`. Zoom or
pan to the marker. In a sparse view (refer to [Large views](#large-views)), zoom in for exact
values.

Point markers are not available in live mode.

### Mouse and keyboard

**Help → Keyboard and mouse** shows these controls in the window.

#### Mouse controls

| Action | Input |
|---|---|
| Pan | Left-drag |
| Zoom | Mouse wheel. To zoom one axis only, put the pointer on that axis |
| Zoom to a region | Right-drag a box. Hold Alt or Shift to limit the box to one axis |
| Fit to the data | Double-click |
| Tune the channel | Ctrl+click the spectrum or the waterfall. Or drag the cyan lines: the center line moves the channel, and the edge lines change the bandwidth |
| Play audio from a point | Shift+click a time plot or the waterfall |
| Put a point marker (P1, P2) | Alt+click a plot, or press `M`. Drag a marker to move it. Refer to [Point measurements](#point-measurements) |
| Export a region | Right-click the waterfall |
| Save plot snapshots | Click **Snapshot...** in the status bar |
| Plot options | Right-click a time plot (I/Q, AM, FM, PM), or click its ⚙ button |

#### Keyboard controls

| Action | Key |
|---|---|
| Pan the view | `←` and `→` (10 %), `PgUp` and `PgDn` (one page) |
| Zoom the time axis | `+` and `-` |
| Go to the start or the end | `Home` and `End` |
| Show everything | `A` |
| Show or hide the time markers | `T` |
| Show or hide the frequency markers | `F` |
| Put a point marker at the pointer | `M` |
| Put a point marker on the strongest spectrum peak in view. Press again for the next peak | `Shift+P` |
| Clear the point markers | `Shift+M` |
| Zoom to the time markers | `Z` |
| Enable or disable the channel filter | `C` |
| Add a bookmark | `B` |
| Signal generator window | `Ctrl+G` |
| Audio window | `Ctrl+D` |
| Export window | `Ctrl+E` |
| Analysis window | `Ctrl+Y` |
| Stop wvt | `Ctrl+Q` |

#### Copy and paste

The text fields (for example, the file path, the frequencies and the times) use the system
clipboard. Thus you can copy a value from wvt into a different program, and paste a value into
wvt.

| Action | Key |
|---|---|
| Copy or cut the selected text | `Ctrl+C` or `Ctrl+X` |
| Paste | `Ctrl+V` |
| Select all the text in the field | `Ctrl+A` |

On macOS, the Command key (`⌘`) also does the `Ctrl` shortcuts, for example `⌘C` and `⌘V`. For
this, XQuartz must send `⌘` keys to wvt, and must copy between the X11 clipboard and the macOS
clipboard. In **XQuartz → Settings**:

- **Input:** clear **Enable key equivalents under X11**. If it is set, XQuartz uses the `⌘` keys
  itself. `Ctrl+C` and `Ctrl+V` work in each case. For Alt+click (point markers) with the Option
  key, set **Option keys send Alt_L and Alt_R**. If it is not set, press `M` instead.
- **Pasteboard:** set **Enable syncing**, **Update Pasteboard when CLIPBOARD changes** and
  **Update CLIPBOARD when Pasteboard changes** (the defaults).

wvt reads text that other programs give as UTF-8, Latin-1 (`STRING`) or `TEXT`. It also reads
large texts that come in parts.

### Load a segment

On a large file, the first view of the full file can be slow. The waterfall reads samples from all
of the file. Above its work limit, it analyzes only a part of the samples ("FFT sees" below
100 %). To work on a part of the file only, load a segment.

#### Set a segment

1. In the file row, type the start time in the first **Segment** box, in seconds.
2. Type the end time in the second **Segment** box.
3. Press Enter.

wvt then shows the new segment and calculates all the plots again.

You can type the times before you open the file, or change them after. From the command line, use
`--segment T0:T1`.

#### Segment rules

- `-1` in the first box is the start of the file. `-1` in the second box is the end of the file.
  `-1` to `-1` loads the full file. This is the default.
- The plots, readouts, bookmarks and markers show the file time. For example, a segment from 40 s
  to 50 s shows 40 s to 50 s, not 0 s to 10 s.
- The view, the scrollbar, the overview strip and the audio ranges stay in the segment.
- If the start is after the end of the file, or the end is before the start, wvt tells you why.
  wvt then keeps the previous segment.
- If the end is after the end of the file, wvt uses the end of the file.
- The segment stays when you open the next file. If the segment does not fit that file, wvt loads
  the full file.

Examples:

```sh
wvt big.sigmf-meta --segment 40:50                 # 40 s to 50 s
wvt big.sigmf-meta --segment 80:-1 --view 90:91    # 80 s to the end, with a 1 s view at 90 s
wvt big.sigmf-meta --segment 40:50 --chan 300k:200k --audio wfm --audio-out seg.wav   # 10 s of audio
```

#### How segments work

wvt does not copy the segment. The file stays memory-mapped, and all the calculations use the
sample range of the segment. The overview strip calculates its power only over this range. The
self-check `build/segment_check` tests the rules in this chapter.

### Export a region

The export window writes a time range of the file, and as an option a frequency band, to a new
SigMF or raw file.

#### Open the export window

Do one of these steps:

- Select **File → Export region…**.
- Press **Ctrl+E**.
- Right-click the waterfall (do not drag). Then select *Select a region to export…*, *Export this
  view…* or *Export the channel…*.

#### Select the region

**Fill from** sets the region from one of these sources:

| Source | Time range | Band |
|---|---|---|
| **View** | The view | The full band |
| **Channel** | The view | The band of the channel filter |
| **Markers** | The time markers | The frequency markers |
| **Select…** | The box that you drag on the waterfall | The box that you drag on the waterfall |

You can also type or adjust each value. The start and the end are in seconds or in samples. The
band is a low and a high frequency, or a center frequency and a width.

To select a box on the waterfall:

1. Click **Select…**, or right-click the waterfall and select *Select a region to export…*.
2. Drag a box with the left mouse button. The view does not pan while you drag.
3. To cancel, press **Esc**.

A right-drag still zooms to a box.

While the export window is open, the waterfall shows the region:

- To move the region, drag the box.
- To change its size, drag its edges or corners.
- A full-band region shows as two time lines.
- The I/Q, AM, FM and PM plots show the time range as a shaded area.

#### Full band or a band

**Full band:** wvt copies the samples without a change. The output has the same format, rate and
center frequency as the source. If the source is big-endian or has I and Q swapped, wvt writes the
output in the standard order.

**A band:** wvt makes a new recording of the band only. The window shows the output rate, center
frequency, taps, decimation, size and file name before you export. The output format is one of:

- `cf32`.
- `ci16`, at half the size. wvt clips values above full scale and shows the number of clipped
  samples.

#### Write the export file

1. Make sure that the file name and the format are correct.
2. Click **Export**.

The export runs in the background with a progress bar.

- **Cancel** stops the export and deletes the partial file.
- If the file exists, wvt replaces it only after you confirm.
- wvt checks the free disk space before it starts.
- The export stays in the loaded segment.

The default name has the rate and the frequency, for example `capture_12.5s_100.3MHz_200kSps`.
Thus a raw export opens in wvt without options.

The SigMF metadata of the output has:

- The sample rate and the center frequency.
- `core:datetime`: the start time of the source plus the start of the range, if the source has a
  start time.
- A `wvt:extract` object with the source file, the sample range, the band and the filter.

#### Export on the command line

| Option | Function |
|---|---|
| `--extract-out PATH` | Write the region to `PATH` (without the extension), then stop |
| `--extract T0:T1` | The time range in seconds. The default is the full segment |
| `--extract-band LO:HI` | Only this band, in absolute Hz. Without this option, wvt copies the samples |
| `--extract-format cf32\|ci16` | The output format for a band. The default is `cf32` |
| `--extract-overwrite` | Replace output files that exist |
| `--export-window` | Open the export window with the `--chan` band, or with the view |

`--segment` limits the export. `--record-format sigmf|raw` sets the file type.

```sh
wvt cap.sigmf-meta --extract 10:20 --extract-out part                               # 10 s, copied
wvt cap.sigmf-meta --extract 10:20 --extract-band 100.2M:100.4M --extract-out fm    # a 200 kHz band
```

#### How the export works

For a band, wvt does these steps:

1. It mixes the center of the band to 0 Hz.
2. It applies a low-pass FIR filter. The passband is the band, flat to the edges. Each side has a
   25 % transition band. The stopband is approximately 74 dB down.
3. It decimates to `fs / D`, where `D = ⌊fs / (1.25 · BW)⌋`.

If a GPU is available, the filter runs on the GPU. The mixer phase is a function of the absolute
sample number. The self-check `build/extract_check` tests the copies, the band filter, the
metadata, the input checks and the cancel.

### Signal analysis

The **Analysis** window has tools that measure and examine a part of a recording. Use them to find
the parameters of a signal: its power, bandwidth, pulse timing, symbol rate and modulation. The
window is available in File mode only.

#### Open the Analysis window

Select **Tools → Analysis**, or press `Ctrl+Y`. To open it from the command line, use `--analyze
KIND`, for example `--analyze pulses`. Refer to
[Analysis on the command line](#analysis-on-the-command-line).

The window has one tab for each tool. wvt calculates the result of the open tab only. Each tab
keeps its last result. Thus a tab shows its result immediately when you open it again. When the
region or a setting changes, wvt calculates the result again.

#### Select the samples

The top row of the window sets the samples that all the tabs use.

| Control | Function |
|---|---|
| **Region** | **View**: the time range of the view. **Time markers**: the range between the time markers. **Bookmark**: the time range of the bookmark that you select. **Whole segment**: the full file, or the loaded segment |
| **Channel filter** | On: the tools use the output of the channel filter, when the filter is on. Off: the tools use the full band of the file |
| **Max samples** | The maximum number of samples that a tab holds in memory. The default is 16 M (128 MB) |

The line under the row shows the region, the number of samples, the sample rate and the channel.

The **Measurements**, **Spectrum** and **Pulses** tabs read all the samples of the region in
blocks. Thus their region can be the full file. The other tabs use only the first **Max samples**
of the region. When this occurs, the status line shows *the first part of the region only*.

The status line also shows `Calculating... NN %` while wvt calculates, and the time that the last
result took.

To use the channel filter, set it in the file viewer first. Refer to
[The channel filter and the time plots](#the-channel-filter-and-the-time-plots). With a narrow
channel, the tools get fewer samples and a lower rate. Thus they are faster, and the noise outside
the channel does not change the results.

#### Region measurements

The **Measurements** tab shows these values for the region, and the spectrum that it used:

| Value | Description |
|---|---|
| **Mean power**, **Peak power** | The mean and the maximum of the sample power, in dBFS |
| **PAPR** | The peak power minus the mean power (the crest factor) |
| **Carrier** | The frequency of the spectrum peak |
| **Centroid** | The mean frequency, weighted by the power over the noise floor |
| **Occupied BW (99 %)** | The width of the band that has 99 % of the power, and its edges |
| **-26 dB bandwidth** | From the lowest bin to the highest bin that is not more than 26 dB under the peak |
| **Noise floor** | The power of one spectrum bin at the 20th percentile of the bins |
| **SNR** | The signal power in the occupied bandwidth, relative to the noise in the same band |
| **DC offset**, **DC power** | The mean of I and of Q, and the DC power relative to the total power |
| **I/Q gain imbalance** | The I power relative to the Q power, after wvt removes the DC |
| **I/Q phase imbalance** | The angle between I and Q, relative to 90 degrees |
| **RBW** | The resolution bandwidth of the spectrum |

**FFT size** and **Window** set the spectrum. The plot shows the spectrum, the occupied band, the
noise floor and the carrier.

#### High-resolution spectrum

The **Spectrum** tab shows a Welch spectrum of all the samples of the region. The FFT size can be
up to 1 M points. Thus the resolution can be much better than the spectrum of the file viewer.

| Control | Function |
|---|---|
| **FFT size**, **Window**, **Overlap** | The settings of the Welch average |
| **Max**, **Min** | Show the maximum and the minimum of each bin over all the FFTs |

The line over the plot shows the RBW, the number of averages and the total power.

To measure the power in a channel, turn on the frequency markers (`F`) and put them at the
edges of the channel. The line then also shows:

- The power between the markers, in dBFS.
- The ACPR, in dBc. This is the power in the band of the same width below and above the channel,
  relative to the power in the channel.

#### Histograms and CCDF

The **Histograms** tab shows the distribution of the values of the samples.

| Control | Function |
|---|---|
| **Show** | `I`, `Q`, `AM` (the magnitude), `FM` (the frequency, in Hz from the channel center) or `PM` (the phase) |
| **Bins** | The number of histogram bins |
| **Log counts** | A logarithmic count axis. Use it to see small counts |

The right plot is the CCDF of the power. It shows the probability that the power is more than the
mean power by the value on the horizontal axis. The gray line is the CCDF of Gaussian noise, for
comparison. The line over the plots shows the CCDF at 1 %, 0.1 % and 0.01 %, and the peak.

Some examples:

- An FM histogram of an FSK signal has one peak for each frequency of the signal.
- An AM histogram of a constant-envelope signal has one narrow peak.
- A CCDF to the right of the Gaussian line shows a signal with high peaks, for example OFDM.

#### Constellation diagram

The **Constellation** tab shows the samples on the I/Q plane.

| Control | Function |
|---|---|
| **Carrier offset** | How wvt removes a carrier offset that stays after the channel filter. **None**. **Manual**: the offset that you type. **Mean frequency**: for FM, FSK and tones. **x^N peak**: for M-PSK and QAM. This also removes the mean phase |
| **N** | With **x^N peak**: 2 for BPSK, 4 for QPSK and QAM, 8 for 8-PSK |
| **Normalize** | Scale the samples to an RMS magnitude of 1 |
| **Symbols only** | Show one point at each symbol center. This uses the rate and the timing of the **Symbols** tab. Open that tab first |
| **Density** | Show the number of points in each cell, in color. Off: show each point (up to 20000 points) |

The line over the plot shows the offset that wvt removed, the RMS magnitude and the deviation of
the magnitude.

#### Pulse analysis

The **Pulses** tab finds the pulses in the region and measures each pulse.

| Control | Function |
|---|---|
| **Auto threshold** | On: the threshold is the noise floor plus the value in **Over the noise**. Off: the threshold is the value in **Threshold**, in dBFS |
| **Hysteresis** | A pulse ends when the power is this much under the threshold |
| **Min width** | wvt ignores pulses that are shorter |
| **Smooth** | A moving average of the power, in samples, before the threshold. Use it with noisy signals |

The table has one row for each pulse:

| Column | Description |
|---|---|
| **TOA** | The time of arrival: the time of the leading edge at 50 % of the pulse amplitude |
| **Width** | From the leading edge to the trailing edge, at 50 % of the amplitude |
| **PRI** | The time from the TOA of the pulse before |
| **Peak** | The peak power of the pulse |
| **Frequency** | The mean frequency in the center 80 % of the pulse |
| **Chirp** | The slope of the frequency in the pulse. An LFM pulse has a chirp of its bandwidth divided by its width |
| **Rise**, **Fall** | From 10 % to 90 % of the amplitude, and from 90 % to 10 % |

Click a row to show the pulse in the file viewer.

The line over the table shows the number of pulses, the noise floor and the threshold. It also
shows the mean width, the mean PRI, the PRF and the duty cycle. wvt puts PRIs that are within 1 % of each other into
one group, and shows the result:

- One group: a fixed PRI.
- 2 to 8 groups: a stagger. The next line shows the PRIs.
- More than 8 groups: a PRI that varies (jitter).

The plots show:

- The first 200 pulses, aligned on their TOA. A red line shows the selected pulse.
- The histogram of the PRIs.

**Add as bookmarks** adds a bookmark for each pulse (up to 1000). **Copy table** copies all the
pulses as CSV text.

A pulse that starts before the region, or that does not end in the region, is not in the table.

#### Correlation and template search

The **Correlation** tab has two modes.

**Autocorrelation of the region** shows how the region repeats. The plot shows |R| / R(0)
as a function of the lag, and the table shows the strongest lags. A peak at a lag shows a signal
that repeats after that time, for example the PRI, a frame length or a preamble. Click a lag to
copy it.

**Find a template** finds each part of the file that has the same shape as a template signal:

1. Put the time markers around one example of the signal, or make a bookmark of it.
2. Select **Find a template**, and set **Template** to **Time markers** or **Bookmark**.
3. Set **Look in** to **Whole segment** or **View**.
4. Set the **Threshold**. A hit must have a normalized correlation of this value or more. 1 is a
   perfect match.

The plot shows the score over time, and the table shows the hits. Click a hit to show it in the
file viewer. **Add as bookmarks** adds a bookmark for each hit (up to 1000). **Copy hits** copies
them as CSV text.

The template can have a maximum of 131072 samples. For a longer template, use the channel filter to
decrease the sample rate. Two hits are always at least one template length apart.

#### Symbol rate and eye diagram

The **Symbols** tab finds the symbol rate and the symbol timing of a digital signal, and shows
the signal at each symbol.

| Control | Function |
|---|---|
| **Detector** | What shows the symbol clock. **Envelope**: shaped PSK and QAM. **Envelope changes**: OOK and ASK. **Frequency changes**: FSK, and PSK with sharp edges |
| **Rate** | Empty: use the estimate. Type a rate to use a different rate |
| **Search** | The range of rates to search. Empty: automatic |
| **Trace** | The value that the cursors and the eye show: `I`, `Q`, `AM`, `FM` or `PM` |
| **x^N** | The power for the x^N spectrum |

The plots are:

- **The trace with the symbol cursors.** The red dots are the values at the symbol centers. To
  move the timing, drag the red cursor. **Use the estimated timing** goes back to the estimate.
- **The eye diagram**: the trace for two symbols, for all the symbols, in color.
- **The symbol clock line**: the spectrum of the detector. The estimate is the strongest line
  (the red line). If the line is weak, the line over the plots tells you. Then try a different
  detector.
- **The spectrum of x^N.** For an M-PSK signal with N = M, the strongest line is at N times the
  carrier offset. The line over the plots shows the carrier offset.

**Copy symbol values** copies the trace at each cursor (up to 100000 values) as text.
**Use in Demodulation** opens the **Demodulation** tab with this rate.

#### Cyclostationary analysis

The **Cyclostationary** tab shows the spectral correlation of the region. Digital signals have
features at cycle frequencies (alpha) that noise does not have. For example, a symbol rate shows
as a line at alpha equal to the rate. wvt uses the FFT accumulation method (FAM).

| Control | Function |
|---|---|
| **Channels** | The FFT size of the channelizer. The frequency resolution is the rate divided by this value. The time increases as the square of this value |
| **Max blocks** | The maximum number of channelizer blocks. More blocks give a finer alpha resolution and use more samples |
| **Range** | The dB range of the plots |

The top plot shows the spectral correlation as a function of alpha and frequency, in dB relative
to the peak at alpha = 0. The bottom plot is the cyclic profile: the maximum over the frequency for
each alpha. The line over the plots shows the strongest cycle frequencies.

#### Digital demodulation

The **Demodulation** tab changes the region to bits.

| Control | Function |
|---|---|
| **Mode** | `OOK`, `2-FSK`, `BPSK` or `QPSK` |
| **Rate** | The symbol rate. Empty: wvt estimates it |
| **Auto threshold** | OOK and 2-FSK: wvt finds the threshold between the two levels. Off: type the threshold |
| **Differential** | BPSK and QPSK: each symbol is the phase change from the symbol before. This removes the phase ambiguity of the carrier recovery |
| **Timing loop** | The gain of the timing recovery loop. 0 keeps the timing of the estimate |

The tab shows the bits as text (or in hexadecimal), and the decision value of each symbol. For BPSK
and QPSK, the plot shows the symbols on the I/Q plane.

- **Copy bits** copies the bits that show (up to 200000).
- **Save bits** writes all the bits to the file in **File**. A `.txt` file gets the characters `0`
  and `1`. A different file gets the bits packed into bytes, with the first bit in the most
  significant bit. wvt does not replace a file that exists.

Without **Differential**, the BPSK and QPSK bits can have a phase ambiguity. Thus each bit can be
the opposite value, or the I bits and the Q bits can change places. Use **Differential** if the signal uses
differential coding, or compare the bits with a known preamble.

#### Analysis on the command line

| Option | Function |
|---|---|
| `--analyze KIND` | Open the Analysis window on a tab. `KIND` is `measure`, `spectrum`, `histogram`, `constellation`, `pulses`, `correlation`, `symbols`, `cyclo` or `demod` |
| `--analyze-out F` | With `--analyze`: do not open a window. Write the result to `F` as text, then stop. `-` writes it to the terminal |
| `--analyze-opt K=V,...` | The settings of the tab. Refer to the table below |

Without a window, the region is `--view`, or the full segment. `--chan` sets the channel filter.
wvt does not replace a file that exists.

| Key | Tab | Value |
|---|---|---|
| `nfft` | Measurements, Spectrum | The FFT size (64 to 1048576) |
| `window` | Measurements, Spectrum | `rect`, `hann`, `hamming`, `blackman`, `bh` or `flattop` |
| `overlap` | Spectrum | The overlap in percent |
| `band` | Spectrum | `LO:HI` in absolute Hz: the channel power and the ACPR |
| `max-samples` | All | As **Max samples** |
| `bins` | Histograms, Constellation | The number of bins |
| `carrier` | Constellation | `none`, `mean`, `power` or a frequency in Hz |
| `power` | Constellation, Symbols | N of x^N: 2, 4 or 8 |
| `threshold` | Pulses, Correlation, Demodulation | The threshold of the tab |
| `absolute`, `hysteresis`, `min-width`, `smooth` | Pulses | As the controls. `absolute=1` sets the threshold in dBFS |
| `template`, `search` | Correlation | `T0:T1` in seconds. `template` selects the template search |
| `method` | Symbols | `envelope`, `envchange` or `frequency` |
| `rate` | Symbols, Demodulation | The symbol rate |
| `t0` | Symbols | The time of a symbol center |
| `trace` | Symbols | `i`, `q`, `am`, `fm` or `pm` |
| `np`, `blocks` | Cyclostationary | As **Channels** and **Max blocks** |
| `mode` | Demodulation | `ook`, `fsk`, `bpsk` or `qpsk` |
| `differential` | Demodulation | `0` or `1` |
| `loop` | Demodulation | The gain of the timing loop |

The output starts with lines that start with `#`. These lines give the kind, the sample rate, the
channel center, the first time and the number of samples, and the summary values. CSV rows follow.

```sh
wvt cap.sigmf-meta --analyze pulses --analyze-out pulses.csv
wvt cap.sigmf-meta --view 2:2.5 --chan 300k:150k --analyze measure --analyze-out -
wvt cap.sigmf-meta --analyze correlation --analyze-opt template=1.2:1.201,threshold=0.8 --analyze-out hits.csv
wvt cap.sigmf-meta --analyze demod --analyze-opt mode=fsk,rate=9600 --analyze-out bits.txt
```

### Plot snapshots

wvt saves the plots as PNG files, as they show on the screen. Use the snapshots in reports or to
compare two recordings.

#### Save snapshots

1. Set the plots as you want them in the snapshot.
2. Click **Snapshot...** at the bottom left of the window, or select **File → Save plot
   snapshots...**.
3. In the **Save plot snapshots** window, tick the plots to save. **All** and **None** tick all
   the plots or no plots.
4. Make sure that the **Folder** is correct. The default is `~/wvt-recordings/`. wvt makes the
   folder if it does not exist.
5. Click **Export**.

The window closes, and wvt saves one PNG file for each plot. The status bar shows the result. To
close the window without a save, click **Cancel**.

#### Snapshot plots

| Plot | Mode | Contents |
|---|---|---|
| Overview strip | File | The power over the full file or segment, with the view box |
| Spectrum | File, Device | The spectrum of the view, or the live spectrum |
| Waterfall | File, Device | The waterfall with its color bar |
| I / Q, AM, FM, PM | File | The time plot with its header (name, readout) |
| Analysis (the open tab) | File | The main plot of the open tab of the Analysis window |

- A plot that is not on the screen is not available. For example, a hidden time plot, or the time
  plots in Device mode.
- The time plots scroll. If a time plot is only partly on the screen, the window shows **partly
  hidden**. wvt then saves only the part on the screen. To save all of the plot, scroll the plots
  or set a smaller plot height in the channel options.

#### Snapshot file names

The file name is `<recording>_capture_<plot>_<n>.png`, for example
`cap_915MHz_capture_waterfall_3.png`.

- `<recording>` is the name of the open recording without its folder and extension. In Device mode,
  it is `live`.
- `<plot>` is `overview`, `spectrum`, `waterfall`, `iq`, `am`, `fm`, `pm` or `analysis`.
- `<n>` is the first number that no snapshot of this recording in the folder uses. All the files of
  one export have the same number.

The window shows the names before you export. wvt does not replace a snapshot file. If a different
wvt window uses the same number at the same time, wvt uses the next number.

#### Image quality

- The PNG files have no loss. Each pixel is the same as on the screen.
- The image size is the size of the plot on the screen. For larger images, make the window or the
  plot larger, or start wvt with a larger `--scale`.
- The pointer crosshair and tooltips are not in the snapshots.

#### How snapshots work

Each plot records its screen rectangle on each frame. When you click **Export**, wvt hides the
pointer for two frames, so that no crosshair or tooltip shows. After the next frame, wvt reads
the frame (from OpenGL or the software renderer) and cuts out each plot. The PNG encoder is
`stb_image_write` in `third_party/stb/`. The self-check `build/snapshot_check` tests the file
names, the numbering, the crop and the PNG files.

### Audio

The audio window demodulates one channel (AM, NFM, WFM, USB, LSB or CW) and plays it through the
speakers. It plays a recording or the live stream, and it writes WAV files.

#### Open the audio window

Do one of these steps:

- Select **Tools → Audio**.
- Press **Ctrl+D**.
- Start wvt with `--audio MODE`.

The window floats over the main window. It works in File mode and in Device mode.

#### Select the audio channel

With **Follow viewer channel** on (the default), the audio uses the channel of the viewer. This
channel shows as the cyan lines on the spectrum and the waterfall.

- To tune, Ctrl+click the spectrum or the waterfall, or drag the lines.
- Live mode also shows these lines.

With **Follow viewer channel** off, the audio channel is the orange line. Set its offset and its
bandwidth in the audio window.

When you select a mode, wvt sets the bandwidth to the default of that mode:

| Mode | Default width | Notes |
|---|---|---|
| AM | 10 kHz | The envelope. *AGC* divides by the carrier level |
| NFM | 12.5 kHz | Audio up to 5 kHz |
| WFM | 200 kHz | Mono, audio up to 15 kHz, 75 µs de-emphasis. Select 50 µs for Europe |
| USB / LSB | 2.8 kHz | The passband is the shaded channel. The carrier is at its lower edge (USB) or upper edge (LSB) |
| CW | 500 Hz | A carrier at the channel center plays at the *BFO pitch* (200 Hz to 1500 Hz) |

#### Audio levels

- **Gain** adds gain after the demodulator.
- **Squelch** mutes the audio while the channel power is below the threshold. The window shows the
  channel power. Set the threshold just above the noise.
- **Volume** and **Mute** change only the speaker level. They do not change the WAV files.
- **Device** selects the sound output. The change applies at the next **Play**.

#### Audio in File mode

**Play** plays the **Range**. Select one of these ranges:

- *From view start* to the end of the file.
- *View*.
- The part between the *time markers*.
- *Whole file*.

With a segment loaded, each range stays in the segment, and *whole file* becomes *whole segment*.

- A green playhead line moves across the time plots and the waterfall. To seek, drag the line or
  the position slider.
- To play from a point, **Shift+click** a time plot or the waterfall.
- *Loop* repeats the range.
- *Follow playhead* moves the view forward one page when the playhead leaves it. This applies only
  to views of 0.25 s or more.

#### Audio in Device mode

**Play** plays the live stream, with a delay of approximately 0.15 s.

- The sound card and the SDR use different clocks. wvt measures the buffer level and adjusts the
  resampling ratio. The window shows the correction in ppm. Thus the delay stays constant.
- If the demodulator falls behind, it skips to the newest samples and counts a *resync*.
- After a retune or a stream restart, the audio continues automatically.

#### WAV files

The WAV files are 48 kHz, 16-bit mono. The name is the *File prefix* plus the date, the time, the
frequency and the mode, for example `~/wvt-recordings/audio_20261018_143000_100.3MHz_WFM.wav`.

- File mode: **Save WAV** demodulates the range faster than real time.
- Device mode: **Record audio** records the demodulated stream. It also works without a sound
  device.

#### Audio on the command line

`--chan OFFSET:BW` sets the channel. Without it, the channel is at 0 Hz with the default width of
the mode.

| Option | Function |
|---|---|
| `--audio am\|nfm\|wfm\|usb\|lsb\|cw` | Open the audio window and play |
| `--audio-out FILE.wav` | Demodulate to a WAV file, then stop. The range is the file, the `--segment` or the `--view` range. With `--device`, the range is the live stream until Ctrl+C or `--record-duration` |
| `--audio-device NAME` | The sound output. The default is the system default |
| `--list-audio-devices` | Show the sound outputs, then stop |
| `--volume DB` | The playback volume (default −6 dB) |
| `--squelch DBFS` | Mute while the channel power is below this level |

```sh
wvt capture.sigmf-meta --chan 300k:200k --audio wfm                       # play a broadcast FM channel
wvt capture.sigmf-meta --chan -250k:10k --audio am --view 0.5:1.5 --audio-out am.wav   # export 1 s
wvt --device rtlsdr -c 100.4M -r 2.4M --audio wfm --chan -300k:200k       # listen live to 100.1 MHz
wvt --device sim -c 100M -r 2.4M --audio wfm --chan 360k:200k --audio-out fm.wav --record-duration 10
```

#### How the audio works

The demodulator does these steps:

1. It shifts the channel to 0 Hz.
2. It decimates with half-band filters to a minimum of two times the channel width.
3. It filters the channel with a windowed-sinc FIR filter.
4. It demodulates.
5. A polyphase resampler sets the audio bandwidth and the 48 kHz output rate.

All stages keep their state between blocks. Thus the audio is continuous. The self-check
`build/audio_check` tests each mode without hardware.

### Live devices

wvt streams from an RTL-SDR v3, a USRP, a Signal Hound BB60 or an Epiq Solutions Sidekiq. The USRP
can be the USB B200 family, or a network USRP such as the N310, X310 or X410. wvt shows the
spectrum and a scrolling waterfall in real time, and it records the stream to SigMF or raw IQ
files. wvt opens these files directly. It can also
[send the stream over the network](#stream-over-the-network).

The build must have the library of the device. Refer to
[Install the device libraries](#install-the-device-libraries).

#### Prepare the device

##### RTL-SDR v3 on Linux

1. Stop the DVB-T driver of the kernel from using the dongle:

   ```sh
   echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-rtlsdr.conf
   ```

2. Disconnect and connect the dongle again, or type `sudo modprobe -r dvb_usb_rtl28xxu`.
3. Make sure that `rtl_test` works without root. On most distributions, the `rtl-sdr` package
   installs a udev rule. If `rtl_test` works only as root, add your user to the `plugdev` group.
   Then connect the dongle again.

##### USRP B200 family (USB)

This applies to the B200, B210, B200mini and B205mini.

1. Download the FPGA and firmware images one time: `sudo uhd_images_downloader`. On macOS with
   Homebrew, type the command without `sudo`.
2. On Linux, install the UHD udev rule, so that UHD works without root. On Debian and Ubuntu, the
   `uhd-host` package installs this rule.
3. Connect the device to a USB 3 port. Rates above approximately 8 MS/s need USB 3.
4. To make sure that the computer finds the device, type `uhd_find_devices`.

##### Network USRPs (N3xx, X3xx, X4xx)

This applies to the N300, N310, N320, N321, X300, X310 and X410, and to the other USRPs that
connect through Ethernet.

1. Make sure that the FPGA image on the device agrees with the UHD version on the computer. If
   `uhd_usrp_probe` shows an image error, update the image as the Ettus manual tells you.
2. Give the network interface of the computer an address on the subnet of the USRP. For example,
   an X310 has `192.168.10.2` on SFP port 0 (1 GbE) and `192.168.40.2` on SFP port 1 (10 GbE).
3. For rates above approximately 25 MS/s, use a 10 GbE (or faster) link. Set the MTU of the
   interface to 9000.
4. On Linux, make the network buffers of the kernel larger, as UHD tells you when it starts.
5. To make sure that the computer finds the device, type `uhd_find_devices` or
   `uhd_find_devices --args addr=192.168.40.2`.

A network USRP makes its sample rates from a fixed master clock divided by a whole number. For
example, an X310 at 200 MHz gives 200, 100, 66.67, 50 MS/s and so on. If you set a different rate,
the device uses the nearest rate that it can make, and the status bar tells you. To use a
different master clock, add `master_clock_rate=` to the device arguments, for example
`addr=192.168.40.2,master_clock_rate=184.32e6`.

##### Signal Hound BB60

This applies to the BB60D, BB60C and BB60A.

1. Download the Signal Hound SDK from the Signal Hound web site, and extract it.
2. Configure the build with the SDK folder:

   ```sh
   cmake -S . -B build -DSIGNALHOUND_SDK_DIR=$HOME/signal_hound_sdk
   ```

   CMake shows `wvt: Signal Hound BB60 support enabled (bb_api 5.0.x, ...)`. It copies the library
   into `build/bb_api/`, and wvt finds it there.
3. On Linux, install the udev rule of the SDK (`lib/linux_x64/sh_usb.rules` or
   `lib/aarch64/sh_usb.rules`) in `/etc/udev/rules.d/`, so that the BB60 works without root. Then
   connect the BB60 again.
4. Connect the BB60 to a USB 3 port.
5. Close Spike and other programs that use the BB60. Only one program at a time can use it.

The SDK has libraries for these computers:

| Computer | Library | Notes |
|---|---|---|
| Linux x86-64 | `lib/linux_x64/<distribution>/libbb_api.so.5.x` | CMake uses the newest version 5 library. It also copies `libftd2xx.so` |
| Linux ARM64 | `lib/aarch64/libbb_api.so.5.x` | For example a Raspberry Pi 5 or a Jetson |
| macOS ARM (Apple silicon) | `lib/macos_arm/libbb_api.5.x.dylib` | Needs libusb from the ARM Homebrew (`/opt/homebrew`): `brew install libusb`. The rates go down to 312.5 kS/s only (decimation up to 128) |

:::{note}
The macOS library is for ARM only. A wvt build for x86-64 (for example with the Intel Homebrew in
`/usr/local`) cannot use it. On Apple silicon, build with the ARM Homebrew in `/opt/homebrew`.
Then all the backends (RTL-SDR, UHD, BB60, Metal) build together:

```sh
brew install cmake pkgconf libusb librtlsdr uhd      # the ARM Homebrew
PATH=/opt/homebrew/bin:$PATH cmake -S . -B build -DSIGNALHOUND_SDK_DIR=$HOME/signal_hound_sdk
PATH=/opt/homebrew/bin:$PATH cmake --build build -j
```

If CMake shows `this build is x86_64`, the CMake in the `PATH` is the Intel one.
:::

##### Epiq Sidekiq

This applies to the Sidekiq cards in a Linux computer: M.2, M.2-2280 (Stretch), mPCIe, X2, X4 and
NV100. wvt receives on one channel (handle A1). The embedded products (Z2, Z3u, Z4, Matchstiq) run
their own Linux, and wvt does not run on them.

1. Get the Sidekiq SDK from the Epiq Solutions support portal, and extract it. The SDK must be
   version 4.14.0 or later. wvt is tested with 4.26.0. The SDK is not open source. Do not add it to
   the repository.
2. Install the kernel modules and the udev rules of the SDK, as the SDK manual tells you
   (*Installation procedure*). Make sure that the SDK program `sidekiq_probe` finds the card.
3. Configure the build with the SDK folder:

   ```sh
   cmake -S . -B build -DSIDEKIQ_SDK_DIR=$HOME/sidekiq_sdk_v4.26.0
   ```

   CMake shows `wvt: Sidekiq support enabled (SDK 4.26.0, ...)`. For the other folders that CMake
   looks in, refer to [WVT_WITH_SKIQ](#wvt_with_skiq). CMake links `libsidekiq` statically. wvt
   loads the support libraries of the SDK (libusb, glib, tirpc) from the SDK folder at run time.
   Thus keep the SDK in its folder after the build.
4. Close the other programs that use the card. Only one program at a time can use a card. If a
   card is in use, the device list and the error message give the process ID of the program that
   has it.

The SDK has libraries for Linux x86-64 (`lib/libsidekiq__x86_64.gcc.a`) and Linux ARM64
(`lib/libsidekiq__aarch64.a`). CMake selects the library for the CPU of the build.

:::{note}
At high rates, a recording writes 4 bytes per sample: 1 GB/s at 245.76 MS/s on an X2 or X4.
Record to a fast NVMe disk, or use a lower rate. The spectrum analyzes all the samples up to
20 MS/s, or up to approximately 100 MS/s with the GPU. The *FFT sees* readout shows the part that
it analyzes.
:::

#### Start a stream

1. In the **Source** drop-down on the right of the menu bar, select **Device**.
2. Select the device. wvt looks for devices in the background, on USB and on the network. Until
   the search is done, the list shows *Scanning...*. To search again, click **Rescan**.
3. For a USRP that is not in the list, type its device arguments in **Args** and press Enter. For
   example, type `addr=192.168.40.2`. For **ZMQ stream**, type the address in **Address**. Refer to
   [Stream over the network](#stream-over-the-network).
4. Set the center frequency, the sample rate and the gain.
5. Click **Start**.

Each wvt window streams from one device only. You can also start wvt with `--device`:

```sh
wvt --device rtlsdr -c 100.1M -r 2.4M --gain 30          # RTL-SDR v3, FM broadcast band
wvt --device rtlsdr -c 7.1M -r 2.4M --direct-sampling Q  # RTL-SDR v3, HF
wvt --device uhd -c 915M -r 20M --gain 40                # the first USRP found (for example a B205mini)
wvt --device uhd:addr=192.168.40.2 -c 2.4G -r 50M --usrp-channel 1 --antenna RX2   # X310, second daughterboard
wvt --device uhd:addr=192.168.10.2 -c 3.5G -r 122.88M --ref external                # X410 on a 10 MHz / PPS reference
wvt --device bb -c 2.45G -r 20M --ref-level -30          # Signal Hound BB60 (the first one)
wvt --device skiq -c 915M -r 10M --gain 40               # Epiq Sidekiq (the first free card)
wvt --device skiq:8N3H0 -c 2.4G -r 50M --antenna J1      # Sidekiq with serial number 8N3H0, RF port J1
wvt --device skiq -c 2.4G -r 50M --antenna "J3 (RX B1)"  # Sidekiq X4, port J3 of receive handle B1
wvt --device sim -c 100M -r 2.4M                         # built-in simulator, no hardware
```

To see the devices that wvt finds, type `wvt --list-devices`. For all the device options, refer to
[Live devices on the command line](#live-devices-on-the-command-line).

#### Device limits

| | RTL-SDR v3 | USRP | Signal Hound BB60 | Epiq Sidekiq |
|---|---|---|---|---|
| Tuning | 24 MHz to 1.766 GHz. HF below 24 MHz with direct sampling (Q) | The range of the device and its daughterboard | 9 kHz to 6.4 GHz | The range of the card, for example 70 MHz to 6 GHz on the M.2 |
| Sample rate | 225 kS/s to 300 kS/s, or 0.9 MS/s to 3.2 MS/s. 2.4 MS/s or less is reliable | The range of the device, up to 500 MS/s | 40 MS/s divided by a power of 2: 40, 20, 10 ... 4.883 kS/s | The range of the card: up to 61.44 MS/s (M.2, mPCIe, NV100) or 245.76 MS/s (X2, X4) on PCIe. Much less on USB |
| Gain | 0 dB to 49.6 dB in tuner steps, or AGC | The range of the device. AGC on the devices that have it | A reference level, −130 dBm to +20 dBm | dB above the lowest gain index: 0 dB to 76 dB in 1 dB steps (M.2, mPCIe), 0.5 dB steps on the other cards. AGC on the cards that have it |
| Options (⚙) | Bias-tee, direct sampling, PPM correction | Antenna, channel, reference, analog bandwidth | Attenuator, amplifier, I/Q bandwidth, external reference | RF port, RF bandwidth, external reference |
| Samples | `cu8` | `ci16` (sc16) | `ci16` (sc16) | `ci16` (sc16) |

wvt reads the USRP limits from the device when it opens. Thus wvt does not need a table of models.
These are typical values:

| USRP | Link | Tuning | Maximum rate | Receive channels |
|---|---|---|---|---|
| B200, B210, B200mini, B205mini | USB 3 | 70 MHz to 6 GHz | 56 MS/s (61.44 MS/s) | 1 (B210: 2) |
| N310 | 1 GbE, 10 GbE | 10 MHz to 6 GHz | 153.6 MS/s | 4 |
| X310 | 1 GbE, 10 GbE | Depends on the daughterboards (UBX: 10 MHz to 6 GHz) | 200 MS/s | 2 (one for each daughterboard slot) |
| X410 | 10 GbE, 100 GbE | 1 MHz to 7.2 GHz | 491.52 MS/s (500 MS/s) | 4 |

The real limit is often the link and the host. A 1 GbE link carries approximately 25 MS/s. Above
approximately 100 MS/s, the computer must also analyze and record the samples fast enough.

#### Device settings

##### USRP options

The ⚙ button next to **AGC** has these options. A change starts the stream again.

Antenna
: The receive port of the channel, for example `RX2` or `TX/RX`. The list comes from the device
  after it opens. *Default* uses the default port of the device.

Channel
: The receive channel. The X310 has one for each daughterboard slot, and the N310 and X410 have
  four. wvt streams one channel at a time. From the command line, use `--usrp-channel N`.

Reference
: The clock and time reference: *internal*, *external* (the 10 MHz and PPS inputs) or *gpsdo*.
  The reference applies to the full device, so the transmitter uses it too. From the command line,
  use `--ref SRC`.

Bandwidth
: The analog filter bandwidth. 0 sets it automatically. Not all daughterboards have this filter.

##### BB60 settings

Ref level
: The strongest input that you expect, in dBm. The BB60 sets its attenuator and amplifier from
  it. If the level is too low, the ADC overloads, and the status bar shows *ADC overload: raise the
  reference level*. If the level is too high, the noise floor goes up. The BB60 has no AGC. A
  change applies when you release the slider, because the BB60 restarts its stream.

Sample rate
: Select a rate from the list. The BB60 makes only 40 MS/s divided by a power of 2. Above 10 MS/s,
  the API filters the samples on the computer, which needs a fast CPU.

Atten and Amp
: The attenuator has the values 0, 10, 20 and 30 dB. The amplifier has the values 0 to 3 (on
  the BB60D: 0, 5, 15 and 20 dB). *Auto* sets them from the reference level. To set them manually, set both. Then wvt does
  not use the reference level.

Bandwidth
: The I/Q filter bandwidth. 0 sets the widest filter for the rate. For example, 27 MHz at
  40 MS/s, 17.8 MHz at 20 MS/s and 8 MHz at 10 MS/s. wvt limits a wider value, and the status bar
  tells you.

Ext ref
: Lock to a 10 MHz reference on port 1.

A retune or a new reference level restarts the stream of the BB60. The log (DEBUG) shows how long
each restart takes. wvt marks the retune between the last old sample and the first new one. Thus a
SigMF recording starts its new capture segment at the correct sample.

##### Sidekiq settings

Gain
: The gain in dB above the lowest gain index of the card. The M.2 and mPCIe (AD9361) have 1 dB
  steps. The X2, X4 and NV100 have 0.5 dB steps. The *AGC* box shows only if the card has automatic
  gain. If the RF input is more than 0 dBm, the status bar shows *RF overload*.

Sample rate
: Type a rate in the range of the card. The tooltip shows the range after the card opens. A USB
  card (mPCIe or M.2 on USB) streams much less than on PCIe. The device list shows the transport,
  for example *Sidekiq M.2 serial 8N3H0 (USB)*.

RF port
: The receive port, and the receive handle that it belongs to, for example *J3 (RX A2)*. After the
  card opens, the list shows the ports of all the receive handles of the card (an X4 has A1, A2,
  B1 and B2). Before that, the list shows *Start the card to list its ports*. *TRX* ports, for
  example *J1 (TRX A1)*, can also transmit. wvt switches the card to its TRX port configuration and
  sets the port to receive. *Default* receives on handle A1 on the default port of the card.
  `--antenna` and session files take the same names, or a port name alone (`J1`) for the first port
  with that name.

Bandwidth
: The RF bandwidth. 0 sets 80 % of the sample rate.

Ext ref
: Use the external reference clock input. The M.2 and mPCIe cannot change the reference while
  wvt runs. Set their reference with the `ref_clock` program of the SDK.

A retune and a gain change apply while the card streams. 12-bit cards (M.2, mPCIe) have the same
dBFS scale as 16-bit cards: a full-scale signal shows 0 dBFS.

#### The live view

- You can change the **frequency** and the **gain** while the device streams. A change to the rate
  or to a device option starts the stream again.
- The **waterfall** scrolls with the newest row at the top. Its vertical axis shows the seconds
  before now.
- The **FFT**, **Window** and **dB** range are on the settings row. The other settings are behind
  its ⚙ button:
  - *Rows/s* sets the scroll speed.
  - *History* sets the number of rows that wvt keeps.
  - *Avg* `auto` averages all samples since the previous row, up to 20 MS/s (100 MS/s on the GPU).
    The status bar shows the fraction of the samples that the FFT sees. To see short bursts more
    clearly, set a lower *Avg*.
- The **spectrum** shows a running average and a peak hold. *Spec avg* sets the time constant of
  the average, in rows. *Reset*, next to *Peak hold* in the ⚙ options, starts the peak hold again.
- The **status bar** shows the device, the actual rate and frequency, and all overflows. The device
  can change the rate and the frequency that you set. An overflow is a group of samples that the
  device sent but the computer did not receive in time.
- The time plots (I/Q, AM, FM, PM) are not available in live mode. To analyze a signal, record it.
  Then use **Open recording** to open it in file mode.

#### Recording

**Record**, on the device row, records the live stream. Its ⚙ button sets where and how.

**Record to** is a path prefix. wvt adds the date, the time, the frequency and the rate, for
example `~/wvt-recordings/wvt_20261016_142233_915.000MHz_2.4MSps.sigmf-data`. wvt makes the
folders that do not exist. wvt does not replace a file. If the data file or the `.sigmf-meta` file
exists, the recording does not start.

##### Recording file types

**SigMF** (the default) writes `.sigmf-data` and `.sigmf-meta`.

- The metadata has the sample format, the rate, the device, the start time and the frequency.
- If you tune to a new frequency during a recording, wvt starts a new capture segment at that
  sample.
- Each gap (dropped samples) gets an annotation. The annotation tells where the gap is and how
  many samples it lost.
- wvt writes the metadata again during the recording. Thus the metadata is usable if the wvt
  process stops unexpectedly.

**Raw** writes only the samples (`.cu8`, `.cs16` or `.cf32`).

- The file name has the rate and the frequency, where wvt can find them.
- A raw file cannot record a frequency change. Thus wvt locks the tuning during a raw recording.

##### Recording samples

- *native* saves the samples that the device sends: `cu8` for an RTL-SDR and `ci16` for a USRP,
  BB60 or Sidekiq. This format is the smallest, and it has no loss.
- *cf32* changes the samples to complex 32-bit float, for tools that read only float.

##### Recording limits

- One file has one rate. Thus wvt locks the sample rate during a recording.
- **Limit** stops the recording after a set time.
- The storage must be sufficiently fast. For example, 2.4 MS/s `cu8` is 4.8 MB/s, a B205 at
  56 MS/s `ci16` is 224 MB/s, and an X310 at 200 MS/s is 800 MB/s.
- The **buffer** value shows how far the writer is behind. If the buffer is full, wvt drops samples
  and adds an annotation for the gap.
- **Open recording** stops the device and opens the last recording in file mode.

##### Record without a window

`--no-gui` records without a window, for example on a computer without a display. Press Ctrl+C to
stop.

```sh
wvt --device uhd -c 915M -r 10M --record ~/caps/ism --record-duration 30 --no-gui
```

With `--no-gui --record` and no `--device`, wvt records from the `sdr.default` device of the
[configuration file](#the-default-sdr).

#### Stream over the network

A wvt that streams from a device can publish the stream over the network. Another wvt receives it
as the device **ZMQ stream**, and shows it as if the device were attached. Use it to put the SDR on
a remote computer, for example a computer without a display next to the antenna. Then look at the
stream on your desktop.

The stream goes one way, from the publisher to the subscriber, over UDP. It uses the ZMQ RADIO and
DISH sockets. Thus wvt must be built with ZMQ (refer to [WVT_WITH_ZMQ](#wvt_with_zmq)). Each
message has a header with the sample format, the rate, the center frequency, the gain and the
sample index. Thus the subscriber takes these values from the stream.

##### The address

The publisher sends to an address. The subscriber receives on the same address.

- **Unicast** (one subscriber): the address of the computer of the subscriber, for example
  `192.168.1.50:5600`. Use this address on both sides. Use unicast when one computer receives.
- **Multicast** (any number of subscribers): a group from `224.0.0.0` to `239.255.255.255`, for
  example `239.10.0.1:5600`. Use the same group on all computers. To use one network interface,
  put its name or address first: `en0;239.10.0.1:5600`.

The port is 5600 when you do not give one. The default address is the address of this computer.
This is the first interface that is up and is not the loopback, a VPN link or a link-local
address. If there is no such interface, the default is `127.0.0.1:5600`. The configuration file
can set a different default with [`zmq.address`](#network-stream-keys).

##### Publish

On the device row, click **Publish**. Its ⚙ button sets the **Address** (the subscriber, or a
multicast group). For a group, it also sets the **TTL**, the number of router hops. A TTL of 1
keeps the stream on the local network. While wvt publishes, the row shows the address and the data
rate.

- Publishing does not change the device, the spectrum or a recording. You can record and publish
  at the same time.
- If the network is slower than the stream, the publisher skips samples and shows the number. The
  subscriber sees each skip as a gap.
- If you change the sample rate or a device option, the device restarts and wvt publishes again.

To send to a different program, select a format file in **Format** in the ⚙ options. The other
program receives with the socket of the file, for example a ZMQ SUB socket, and wvt sends with the
matching socket. If the file has an address, you can leave **Address** empty. For the details,
refer to [Publish with a format](#publish-with-a-format).

On a computer without a display, use `--publish` with `--no-gui`. Press Ctrl+C to stop:

```sh
# On the computer with the SDR: send to the subscriber at 192.168.1.50
wvt --device uhd -c 915M -r 10M --gain 40 --publish 192.168.1.50:5600 --no-gui

# Publish and record at the same time. The run stops after 60 s
wvt --device rtlsdr -c 100.1M -r 2.4M --publish 239.10.0.1 --record ~/caps/fm --record-duration 60 --no-gui

# Send raw ci16 on a ZMQ PUSH socket, on the address of the format file
wvt --device rtlsdr -c 433.92M -r 1M --publish "" --publish-format raw_pull_ci16 --no-gui
```

##### Receive

1. In the **Device** list, select **ZMQ stream**.
2. In **Address**, type the address to receive on: an address of this computer, or the multicast
   group of the publisher. Press Enter.
3. Click **Start**. wvt waits up to 3 s for the stream.

The frequency, the rate and the gain come from the publisher. Thus these fields are read-only. If
the publisher retunes, the subscriber retunes at the same sample. **Record** works as for a device.
A SigMF recording has the frequency changes of the publisher, and an annotation for each gap. The
status bar shows lost messages as *network loss*.

From the command line:

```sh
wvt --device zmq:192.168.1.50:5600                       # receive on this computer's address
wvt --device zmq:239.10.0.1 --record ~/caps/remote --record-duration 30 --no-gui
```

To try it on one computer, start two wvt windows. Publish from the first one to `127.0.0.1:5600`,
and receive on `127.0.0.1:5600` in the second one.

##### Receive other formats

The **ZMQ stream** device can also receive samples from other programs, for example a GNU Radio
*ZMQ PUB Sink*. A format file in the [config folder](#the-config-folder) tells wvt the structure of
the messages. The file can use a ZMQ SUB or PULL socket on TCP, or a DISH socket on UDP. For the
files, refer to [ZMQ message formats](#zmq-message-formats).

1. Put the format file, `zmq_NAME.ini`, in the config folder.
2. In the **Device** list, select **ZMQ stream**.
3. In the **Format** list, select the format. wvt reads the folder again each time that you open the
   list.
4. In **Address**, type the address of the source, and press Enter. If you leave the field empty,
   wvt uses the address of the format file. The field shows it in gray.
5. Click **Start**. wvt waits up to 3 s for the first message.

The format `wvt (built-in)` is the stream of another wvt. If you change the format, wvt clears
**Address**. `zmq.address` is the address of the built-in format only.

From the command line:

```sh
wvt --list-zmq-formats                                   # the formats, with their errors
wvt --device zmq --zmq-format gnuradio_sub_cf32          # the address of the format file
wvt --device zmq:192.168.1.30:5555 --zmq-format gnuradio_sub_cf32
```

To use a format each time, set [`zmq.format`](#network-stream-keys) in the configuration file.

The frequency, the rate and the gain come from the message header, or from the format file when the
header does not have them.

##### Stream messages

| Message | Cause | What to do |
|---|---|---|
| *no stream on ADDR in 3 s* | Nothing arrives on the address | Start publishing first. Check that the publisher sends to this address, and check the firewall |
| *port N is in use* | Another program receives on the port | Stop the other subscriber, or use a different port on both sides |
| *X is not an address of this computer* | A unicast address of a different computer | Type an address of this computer. The publisher sends to it |
| *the source stopped publishing* | The publisher stopped | Start publishing again, then click **Start** |
| *the source started again with another rate* | The publisher changed its rate | Click **Start** |
| *waiting for the source* | No message for more than 2 s | Check the publisher and the network |
| *no message from ADDR in 3 s (format NAME ...)* | A format file: nothing arrives. With `mode = connect`, a wrong address also gives this message | Start the source. Check the address and the port |
| *N messages arrived on ADDR, but none is of the format NAME* | The messages do not agree with the format file. The message shows the last reason | Select the correct format, or correct the file. Refer to [ZMQ message formats](#zmq-message-formats) |
| *no ZMQ format 'NAME'* | There is no `zmq_NAME.ini` in the config folder | Run `wvt --list-zmq-formats` to see the formats and the folder |

##### Stream bandwidth

The stream has the native samples of the device, and a header of 64 bytes for each 8000 bytes of
samples:

| Device and rate | Samples | Data rate |
|---|---|---|
| RTL-SDR, 2.4 MS/s | `cu8` | 4.8 MB/s (39 Mbit/s) |
| USRP, 10 MS/s | `ci16` | 40 MB/s (323 Mbit/s) |
| USRP, 25 MS/s | `ci16` | 100 MB/s (806 Mbit/s) |
| USRP, 56 MS/s | `ci16` | 224 MB/s (1.8 Gbit/s) |

- 1 Gbit Ethernet carries up to approximately 25 MS/s of `ci16`. Above that, use 10 Gbit Ethernet.
- A message is larger than an Ethernet frame. Thus the network sends it in parts (IP fragments).
  If one part is lost, the subscriber loses the full message.
- Wi-Fi is not good for multicast, because many access points send multicast at a low rate. Use
  unicast over Wi-Fi.

##### Network notes

- **Firewall:** the computer of the subscriber must accept UDP on the port (5600 by default), for
  example `sudo ufw allow 5600/udp` on Ubuntu.
- **macOS:** to send to and receive from other computers and multicast groups, wvt needs the
  *Local Network* permission. Set it in System Settings → Privacy & Security → Local Network. On a Mac with
  multicast routed to Wi-Fi, two programs on the same computer did not always receive a multicast
  stream. Use unicast, or put the interface first, for example `lo0;239.10.0.1:5600` on one Mac.
- **Multicast across switches:** a switch with IGMP snooping sends a group only to the ports that
  joined it. Without an IGMP querier on the network, some switches stop sending the group after a
  few minutes.
- **Security:** the stream has no encryption and no authentication. Any computer on the network
  can receive a multicast stream, or send a stream to a subscriber. Use it only on a network that
  you trust.

#### Live devices on the command line

| Option | Function |
|---|---|
| `--device DEV` | `rtlsdr[:INDEX\|SERIAL]`, `uhd[:ARGS]` (for example `uhd:addr=192.168.40.2`), `bb[:SERIAL]`, `skiq[:SERIAL\|#N]`, `zmq[:ADDR]`, `sim` or `siggen` |
| `--list-devices` | Show the devices and the GPUs, then stop. A device with a profile in the configuration file shows `[config sdr.N]` |
| `-c`, `--fc HZ` | The center frequency |
| `-r`, `--rate HZ` | The sample rate |
| `--gain DB\|auto` | The receiver gain (default 30 dB) |
| `--bandwidth HZ` | The USRP analog bandwidth, the BB60 I/Q bandwidth, or the Sidekiq RF bandwidth (default: automatic) |
| `--antenna NAME` | The USRP receive antenna, for example `RX2` or `TX/RX`, or the Sidekiq RF port, for example `"J3 (RX A2)"` or `J1` |
| `--usrp-channel N` | The USRP channel for receive and transmit (default 0) |
| `--ref SRC` | The USRP clock and time reference: `internal`, `external` or `gpsdo` |
| `--ref-level DBM` | BB60: the strongest expected input (default −20 dBm) |
| `--atten auto\|0\|10\|20\|30` | BB60: the attenuator (default `auto`) |
| `--bb-amp auto\|0-3` | BB60: the amplifier setting (default `auto`). The generator option `--amp` sets the amplitude |
| `--ext-ref` | BB60: lock to a 10 MHz reference on port 1. Sidekiq: use the external reference clock |
| `--bias-tee` | RTL-SDR: supply power to an LNA or an active antenna through the coax |
| `--direct-sampling I\|Q` | RTL-SDR: HF direct sampling (v3: `Q`) |
| `--ppm N` | RTL-SDR frequency correction |
| `--record PREFIX` | Record to `PREFIX_<date>_<time>_<fc>_<rate>.<ext>` |
| `--record-format sigmf\|raw` | The file type (default `sigmf`) |
| `--record-sample native\|cf32` | The sample format (default `native`) |
| `--record-duration T` | Stop after `T` seconds |
| `--no-gui` | Record or publish without a window. Ctrl+C stops |
| `--publish ADDR` | Send the live stream to `ADDR`: the `a.b.c.d[:PORT]` of the subscriber, or a multicast group `[IFACE;]GROUP[:PORT]` (port 5600 by default). With a tcp `--publish-format`: `HOST:PORT`, `*:PORT`, or `""` for the address of the format file |
| `--publish-format NAME` | The message format of `--publish`: `wvt` (the built-in format), or the `NAME` of `zmq_NAME.ini` in the config folder (default `publish.format`, else `wvt`) |
| `--publish-ttl N` | The multicast hops, 1 to 255 (default `zmq.ttl`, 1) |
| `--zmq-format NAME` | The message format of `--device zmq`: `wvt` (the built-in format), or the `NAME` of `zmq_NAME.ini` in the config folder (default `zmq.format`, else `wvt`) |
| `--list-zmq-formats` | Show the ZMQ message formats of the config folder, with their errors, then stop. Refer to [Check a format file](#check-a-format-file) |

With `--device`, the [SDR profile](#sdr-profiles) of that device sets its frequency, rate, gain and
the other device values. The options win over the profile.

`--device zmq[:ADDR]` receives on `ADDR`: an address of this computer or a multicast group. Without
`ADDR`, it uses `zmq.address`, else the address of this computer. With `--zmq-format NAME`, `ADDR`
is the address for the format, for example `zmq:192.168.1.30:5555`. Without `ADDR`, wvt then uses
the address in the format file. `--device zmq` does not accept `-c`, `-r`, `--gain` or the other
device options.

#### How live streaming works

The device thread writes the samples into a ring buffer, in the native format of the device. Three
types of reader use this buffer:

- The spectrum thread copies the newest samples. If the device thread writes over the samples
  during the copy, the spectrum thread skips them.
- The recorder reads all samples. If the recorder is behind, the device thread does not write over
  the samples that the recorder did not read. It drops the new block and records the gap.
- The network publisher copies the samples, as the spectrum thread does. If it is behind, it skips
  to new samples. The sample index of its messages tells the subscriber how many it skipped.

Thus a slow disk does not stop the spectrum, and each loss in a recording has an annotation. The
buffer holds approximately 2 s of samples, from 16 MB to 1 GB.

The USRP backend uses the C API of UHD for all USRPs. When a USRP opens, wvt asks UHD for the model,
the number of channels, the antennas and the clock sources. It also asks for the ranges of the
frequency, rate, gain and bandwidth of the channel. The window uses these values for its lists and
tooltips. The receiver and the transmitter share one open device. The search for devices runs on a
background thread, because a network search takes approximately half a second. The self-check
`build/device_check` tests the device list entries and the rate rules.

The BB60 backend (`src/device/sdr_bb60.cpp`) uses `bb_api` from the Signal Hound SDK in I/Q
streaming mode with 16-bit samples. Its thread reads blocks of approximately 20 ms. For tests
without the SDK or a device, [WVT_BB_SIM](#wvt_bb_sim) builds the backend against a simulated
BB60D. The simulator makes a tone at 100.2 MHz and noise. It reports an overload when the tone is
above the reference level. The self-check `build/bb_check` uses it.

The Sidekiq backend (`src/device/sdr_skiq.cpp`) uses `libsidekiq` from the Sidekiq SDK.

- It receives on handle A1 with 16-bit samples in I/Q order. It shifts the samples of a 12-bit
  card by 4 bits, so that full scale is 0 dBFS.
- Its thread collects the blocks from `skiq_receive` into deliveries of approximately 20 ms.
- The RF timestamp of each block counts samples. Thus a jump is a loss of that many samples, and an
  overrun shows as a gap with the exact count. The RF timestamp of an AD9361 card jumps when it
  retunes. Thus the gap check pauses for 0.5 s after a retune.
- The SDK messages go to the wvt log (component `skiq`).
- `skiq_exit` stops the library for the whole process. Thus wvt opens one Sidekiq at a time.

For tests without the SDK or a card, [WVT_SKIQ_SIM](#wvt_skiq_sim) builds the backend against a
simulated card: an M.2, or an X4 with `WVT_SKIQ_SIM_PART=x4`. The simulated card keeps up with
245.76 MS/s. It reports an overrun when the host falls more than 64 MiB behind. The self-check
`build/skiq_check` uses it.

### Signal generator

The signal generator makes CW and pulsed waveforms: chirps, Barker codes, staggered or jittered
PRIs, bursts, sweeps and impairments. It also plays back recordings as canned waveforms, one at a
time or as a chain. The generator writes its waveforms to files, shows them in the live view
(loopback), or transmits them with a USRP.

#### Open the generator

Do one of these steps:

- Select **Tools → Signal generator**.
- Press **Ctrl+G**.
- Start wvt with `--siggen` or `--gen MODE`.

The window floats over the main window. It works in File mode and in Device mode.

#### Generator modes and settings

Select the mode at the top of the window: **CW**, **Pulsed**, **Playback** or **Chain**.

| Setting | Function |
|---|---|
| Carrier | The sample rate, the center frequency, the offset from the center, the peak amplitude (dBFS) and the start phase |
| CW | A steady tone, with an optional drift (Hz/s). When you tune to a new frequency, the phase stays continuous |
| Sweeps | CW and pulsed. Refer to [Sweeps](#sweeps) |
| Pulsed | The PRI or the PRF, and the pulse width or the duty cycle. The width includes the edges |
| Edges | Rectangular, linear, raised cosine or Gaussian, with separate rise and fall times |
| PRI pattern | Fixed, stagger (a list of PRIs in turn), or jitter (± %, repeatable with a seed) |
| Bursts | N pulses, then more dead time. A start delay before the first pulse |
| In-pulse modulation | An LFM chirp (up, down or triangle) with a set bandwidth, or a Barker code (2 to 13) |
| Playback | A SigMF or raw IQ recording as a canned waveform. Refer to [Playback](#playback) |
| Chain | Many recordings in turn. Refer to [Chain](#chains) |
| Impairments | White noise (SNR), DC offset, IQ gain and phase imbalance, carrier phase noise (linewidth) |

The pulses are slices of one continuous carrier. Thus the pulses are phase-coherent.

wvt checks the settings while you type. A setting with a problem shows in red, and wvt does not
apply it. These are examples of problems:

- A pulse that is wider than the PRI.
- A Barker chip that is shorter than one sample.
- An offset outside the band.

The **preview** shows one pulse or the pulse train in time, and the averaged spectrum. For a sweep,
it shows the frequency and the level over one period, and a max-hold spectrum of the swept band.

#### Sweeps

A sweep changes the frequency, the level, or the two, over one period. The sweep repeats until the
output stops.

- **Frequency sweep:** ramp up, ramp down, triangle, or stepped (a step size and a dwell time). The
  sweep goes between two offsets from the center.
- **Level sweep:** ramp up, ramp down or triangle, linear in dB, between two dBFS levels.
- The frequency sweep and the level sweep have one period.

While the frequency sweeps, the sweep replaces the **Offset** and the **Drift**. While the level
sweeps, the sweep replaces the **Amplitude**.

In **CW** mode, the tone follows the sweep continuously. Its phase stays continuous through the
wraps and the steps.

In **Pulsed** mode, use the **Sweep** section under **Pulse**. Each pulse takes the frequency and
the level of the sweep at the start of the pulse. The pulse keeps them for its full length. Use this
for pulse-to-pulse frequency agility, stepped-frequency trains, or a scan-like amplitude pattern. A
chirp or a Barker code has its center on the swept frequency.

- If you change a sweep setting while an output runs, the period starts again.
- The noise (SNR) refers to the peak of the level sweep.

#### Playback

Playback plays a recording. The recording can be a file from the generator, a wvt recording, or
any SigMF or raw IQ capture.

1. Type the path of the recording, or use a shortcut:
   - **Last generated** loads the last file from the generator.
   - **Viewer's file** loads the file that is open in the viewer, with the format settings of the
     viewer.
   - **Viewer's markers** plays only the part between the time markers of the viewer.
2. Click **Load**. wvt reads the format, the rate and the frequency from the SigMF metadata or the
   file name. It then sets the generator to the same values. Thus the recording plays at its
   original rate and frequency.

##### Playback rules

- The 0 Hz point of the recording goes to the center plus the offset.
- The peak of the recording plays at the set amplitude. **Keep its level** sets the amplitude to
  the recorded peak. The recording then plays without a change of level.
- If the recording has a different rate, wvt resamples it.
- wvt can play up to 32 Mi samples (2^25) at a time, at the recording rate or at the generator
  rate.
- The impairments apply on top of the recording.
- With **Loop** off, a file output has the start delay and one play.
- Changes apply immediately. A different recording starts again from its beginning.

##### How resampling works

wvt uses a windowed-sinc resampler with a stopband of approximately 80 dB. It changes the rate by
up to 64 times in each direction. When wvt lowers the rate, a filter removes all content outside
the new band. Thus this content does not alias.

#### Chains

A chain plays a list of canned signals in order. A signal can be a file from the generator, a
region that you exported from the viewer, or any SigMF or raw IQ recording.

##### Make a chain

1. Select the **Chain** mode.
2. Add recordings. Type a path and click **Add**, or use the **Last generated**, **Viewer's file**
   and **Viewer's markers** shortcuts.
3. Click a row to edit its item.
4. Use the **^**, **v** and **x** buttons on a row to move the item up, move it down, or remove
   it.

Each item has its own format, file rate, segment, **offset**, **level**, number of **plays** and
**gap after** each play.

- When you add a recording, wvt reads its metadata.
- The offset puts the recording at its recorded frequency, if that frequency is in the band.
- The first recording also sets the rate and the center frequency of the generator.
- The default level is **As recorded**: the samples play without a change. If you clear this
  checkbox, the peak of the recording plays at the set dBFS value.

##### Chain loops

- *Forever* plays the chain until the output stops.
- *Times* plays the chain N times. After the last pass, the output is silent. A file ends there,
  the loopback tells you, and a USRP stops the transmission.

##### Changes while the chain plays

- Offset and level changes apply immediately.
- If you add, remove or move an item, or change a segment, the chain starts again from the first
  item. This occurs after the current item ends.

##### Chain limits

- wvt loads all the items before the chain starts.
- All the items together can have up to 128 Mi samples (1 GiB as `cf32`). Each item can have up
  to 32 Mi samples.
- The noise (SNR) refers to the loudest item.

##### Chain files

**Save** writes the chain to a `.wvtchain.json` file. The file has the items, the loop count, the
start delay, and the rate and the center frequency of the generator. Recordings in the folder of
the chain file, or below it, have paths relative to that folder. Thus you can move the folder.

**Load** reads a chain file. `--chain FILE` also reads a chain file.

The preview shows one pass. Each item has a shaded area and a number. The preview also shows the
average spectrum.

#### Generator outputs

Select one output with *Output* at the top of the window: **File (SigMF / raw)**, **Loopback
(live view)** or **USRP transmit**.

##### File output

The generator writes SigMF or raw files in `cf32`, `ci16`, `ci8` or `cu8`. The file names have the
same pattern as recordings.

- The length is a duration. In pulsed mode, the length can also be a number of PRIs. With a sweep,
  it can be a number of sweep periods. With a chain, it can be a number of passes.
- The SigMF metadata has all the generator settings (`wvt:siggen`).
- As an option, the metadata has one annotation for each pulse, play, sweep or chain item.
- **Generate file** writes the file. **Open in viewer** opens the result.

##### Loopback

The loopback streams the waveform into the live spectrum and waterfall.

1. Select the **Loopback (live view)** output.
2. Click **Start loopback**. wvt starts the device *Signal generator loopback* in the main window,
   at the rate and the center frequency of the generator.
3. To stop, click **Stop loopback**.

You can also select the device *Signal generator loopback* in the **Device** mode of the main
window, or start wvt with `--device siggen`.

The live view shows the waveform as a receiver tuned to the center frequency shows it. Changes
apply while you make them. The main window can record the loopback stream.

##### USRP transmit

The generator transmits the waveform from a USRP, tuned to the center frequency. You can change
the frequency, the gain and all waveform settings while the USRP transmits.

| Setting | Function |
|---|---|
| Device args | The UHD device arguments, for example `serial=31A2B3C` or `addr=192.168.40.2`. Empty: the first USRP that UHD finds |
| Antenna | The transmit port. Empty: the first transmit port of the channel (`TX/RX` on the B2xx and N3xx, `TX/RX0` on the X410) |
| Channel | The transmit channel |
| Reference | The clock and time reference (*internal*, *external* or *gpsdo*). It applies to the full device |
| Bandwidth | The analog filter bandwidth. 0 sets it automatically |
| TX gain | The transmit gain. While the USRP transmits, the slider shows the range of the device |

If the live view receives from the same USRP, the receiver and the transmitter share it.

:::{warning}
Transmit only into a dummy load or an attenuated cable, or where you have a license.
:::

wvt transmits only after you do these steps:

1. Tick **Arm transmitter**.
2. Click **Transmit...**.
3. In the dialog, examine the frequency, the rate and the gain.
4. Click **Transmit**.

The arm setting resets after each transmission.

Safety and limits:

- The TX gain starts at 0 dB.
- While wvt transmits, the window title, the menu bar (**TX ON**) and a large **STOP
  TRANSMITTING** button show it.
- If you close the generator window or stop wvt, the transmission stops.
- You can change the center frequency during a transmission. If the USRP cannot tune to it, the
  transmitter stays on the last good frequency, and the field shows that frequency. If the USRP
  cannot go back to that frequency, the transmission stops.
- If a transmission stops because of an error (for example, the USRP is disconnected), wvt sends
  an end-of-burst to the USRP before it stops. This tells the USRP to stop transmitting.
- To monitor your own signal on a receiver, put a minimum of 30 dB of attenuation between the
  ports.
- On one USRP, wvt can receive and transmit at the same time (full duplex). On a B2xx, the two
  directions must use the same sample rate, because the B2xx uses one clock for the two. A network
  USRP can use a different rate for each direction.
- Between pulses, the USRP transmits zeros. Thus a small LO leakage stays at the center frequency.
  A non-zero offset keeps the signal away from this leakage.

##### Transmit limits

For a lab, you can set limits that wvt cannot go past. The command line and the window both obey
them.

| Limit | Option | Environment variable | [Configuration file](#configuration-file) |
|---|---|---|---|
| The maximum TX gain, in dB | `--tx-max-gain DB` | `WVT_TX_MAX_GAIN` | `tx.max_gain_db` |
| The frequency bands where wvt can transmit | `--tx-allow LO:HI[,LO:HI...]` | `WVT_TX_ALLOW` | `tx.allow` |

- The strictest limit applies. Each source only adds limits: wvt uses the lowest maximum gain, and
  only the frequencies that every source allows. An option cannot remove a limit of the
  configuration file or of an environment variable.
- If the allowed bands of two sources do not overlap, wvt cannot transmit at any frequency.
- The full transmit band is the center frequency ± half the sample rate. This band must be in one
  allowed band, because the generator can put a signal anywhere in it.
- wvt does not start a transmission, change the gain or change the frequency past a limit. The
  TX gain slider stops at the maximum gain. The **Transmit** dialog shows the limits, and why it
  cannot transmit.
- If the value of an environment variable is not correct, wvt does not start.

For example, put this in your shell profile:

```sh
export WVT_TX_MAX_GAIN=30
export WVT_TX_ALLOW=902M:928M,2.4G:2.4835G
```

#### Signal generator on the command line

`-c` sets the center frequency and `-r` sets the sample rate.

| Option | Function |
|---|---|
| `--siggen` | Open the signal generator window |
| `--gen cw\|pulsed\|playback` | The mode. This option also opens the window |
| `--offset HZ` | The carrier offset from the center (default 100 kHz) |
| `--amp DBFS` | The peak amplitude (default −6 dBFS) |
| `--drift HZ` | The CW frequency drift per second |
| `--sweep FROM:TO` | A frequency sweep between two offsets, for example `-400k:400k`. CW, or pulse to pulse with `--gen pulsed` |
| `--sweep-shape up\|down\|tri` | The sweep shape |
| `--sweep-step HZ:DWELL` | A stepped sweep |
| `--sweep-time S` | The sweep period |
| `--level-sweep FROM:TO` | A level sweep in dBFS over the same period |
| `--level-shape up\|down\|tri` | The level sweep shape |
| `--pri S` or `--prf HZ` | The pulse repetition interval or frequency |
| `--pw S` | The pulse width, with the edges |
| `--edge rect\|linear\|cos\|gauss` | The edge shape. `--rise S` and `--fall S` set the edge times |
| `--stagger S,S,..` | PRIs in turn |
| `--jitter PCT` | PRI jitter. `--seed N` makes it repeatable |
| `--burst N[:GAP]` | N pulses for each burst, and `GAP` seconds of silence after each burst |
| `--delay S` | Silence before the first pulse |
| `--lfm up\|down\|tri:BW` | A chirp in each pulse, for example `--lfm up:2M` |
| `--barker N` | A Barker phase code (2, 3, 4, 5, 7, 11 or 13) |
| `--play FILE` | Play a SigMF or raw IQ recording. Its rate and frequency become `-r` and `-c`, if you do not give them |
| `--play-rate HZ` | The rate of the recording |
| `--play-segment START:LEN` | Play a part of the recording. `LEN` 0 plays to the end |
| `--play-gap S` | Silence between the plays |
| `--play-once` | Do not repeat |
| `--chain FILE` | Play a chain file (`.wvtchain.json`) |
| `--chain-add PATH[@OFFSET][:N]` | Add a recording to the chain, played N times. You can use this option more than one time. The first recording sets `-r` and `-c`, if you do not give them |
| `--chain-loops N` | The number of passes through the chain. 0 (default) plays until you stop it |
| `--snr DB`, `--dc I:Q`, `--iq GAIN_DB:PHASE_DEG`, `--linewidth HZ` | Impairments |
| `--gen-out PREFIX` | Write to a file, then stop. `--record-format` sets the file type |
| `--gen-sample cf32\|ci16\|ci8\|cu8` | The file sample format (default `cf32`) |
| `--gen-duration T` or `--gen-pulses N` | The file length (default 1 s). N counts pulses, plays, sweeps or chain passes |
| `--gen-annotate` | SigMF: one annotation for each pulse, play, sweep or chain item |
| `--tx` | Transmit with a USRP until Ctrl+C, or for `--gen-duration` |
| `--tx-gain DB` | The transmit gain (default 0 dB) |
| `--tx-args ARGS` | The UHD device arguments. The default is the first USRP found |
| `--tx-antenna NAME` | The transmit port. The default is the first transmit port of the channel |
| `--tx-dry-run` | Show what `--tx` transmits, check the limits, then stop. wvt does not open the USRP |
| `--tx-confirm` | Transmit, and do not ask first. Use this in a script |
| `--tx-max-gain DB` | The maximum TX gain (see [Transmit limits](#transmit-limits)) |
| `--tx-allow LO:HI[,LO:HI...]` | The bands where wvt can transmit (see [Transmit limits](#transmit-limits)) |
| `--usrp-channel N`, `--ref SRC` | The USRP channel and reference, as for the receiver |

Before `--tx` transmits, wvt shows the signal, the frequency, the rate, the gain, the device, the
duration and the limits. Then it asks you to type `yes`. If the input is not a terminal (for
example, in a script), wvt cannot ask: it stops, unless you give `--tx-confirm`.

##### Signal generator examples

```sh
wvt --gen pulsed --pri 1m --pw 20u --lfm up:2M -r 10M -c 915M                     # open the window
wvt --gen pulsed --pri 1m --pw 20u --barker 13 -r 2M --gen-out ~/caps/b13 --gen-pulses 100 --gen-annotate
wvt --device siggen --gen cw --offset 250k -c 915M -r 2M                          # loopback view
wvt --device siggen --sweep -400k:400k --sweep-shape tri --sweep-time 2 -c 915M -r 2M  # sweep, loopback
wvt --sweep -1M:1M --sweep-time 10m --level-sweep -40:-6 -r 5M -c 915M --gen-out ~/caps/sw --gen-pulses 20
wvt --sweep -200k:200k --sweep-step 25k:5m -r 1M -c 915M --tx --tx-gain 10        # stepped, transmit
wvt --gen pulsed --pri 1m --pw 20u --sweep -2M:2M --sweep-step 500k:10m --level-sweep -30:-6 -r 10M -c 915M
wvt --gen cw --offset 100k -c 915M -r 1M --tx --tx-gain 10 --gen-duration 5         # transmit 5 s
wvt --gen cw --offset 100k -c 915M -r 1M --tx-gain 10 --tx-dry-run                   # only show the plan
wvt --gen cw -c 915M -r 1M --tx --tx-confirm --tx-max-gain 20 --gen-duration 5      # from a script
wvt --play ~/caps/b13_*.sigmf-meta --play-gap 5m --gen-out ~/caps/b13x10 --gen-pulses 10  # 10 plays
wvt --play capture.cs16 --play-rate 2.4M -r 10M -c 915M --play-segment 1.5:0.2 --tx    # replay a burst
wvt --chain-add ~/caps/b13.sigmf-meta:3 --chain-add ~/caps/lfm.sigmf-meta@250k --chain-loops 5 --tx
wvt --chain ~/caps/demo.wvtchain.json --gen-out ~/caps/demo --gen-pulses 10 --gen-annotate
```

#### How the generator works

- **Parameter changes:** the window sends a full copy of the settings to the generator. The
  generator applies the copy at the next block. In pulsed mode, it applies a change after the
  current pulse ends. Thus a change does not cut a pulse.
- **Sweeps:** a CW sweep uses a piecewise-linear phase rotator. Its step also rotates, so the phase
  is exactly quadratic. The rotator starts again at the end of each period, at the turn of a
  triangle, and at each step. The period is a whole number of samples.
- **Playback and chains:** wvt loads each recording once, resamples it, and keeps it in a cache.
  The cache keeps up to 128 Mi samples. When the cache is full, wvt removes the recordings that
  are not in use.
- **Outputs:** the file output, the loopback and the transmitter use the same generator. Thus the
  three outputs give the same samples.

The self-check `build/siggen_check` tests the generator without hardware.

## Part 3: Back end

### Logging

wvt writes a log file for each run. As an option, it also shows the log on the console. Use the log
to find the cause of a problem, or attach it to an issue report.

#### The log file

Each run writes `wvt_log_<YYYYMMDD>_<HHMMSS>.log` (local time). wvt puts the file in the first of
these folders that is set:

1. The `--log-dir DIR` option.
2. The `WVT_LOG_DIR` environment variable.
3. The `log.folder` key of the [configuration file](#configuration-file).
4. The `TMPDIR` environment variable.
5. `/tmp`.

If the folder does not exist, wvt makes it. If wvt cannot make the folder or the file, it writes a
warning and continues without a log file.

The file gets all messages, at all levels, also when you do not use `-v`. To write no file, use
`--no-log-file`.

The start of the file has:

- The start time and the process ID.
- The build: the git commit and version tag (refer to [Version information](#version-information)),
  and whether RTL-SDR, UHD and audio are on or off.
- The operating system.
- The full command line.

Each line has the date and time (to the millisecond), the level, the component and the message:

```text
# wvt log, started 2026-10-20 19:51:53.739, pid 91526
# build: git 0e5ae9d, RTL-SDR on, UHD on, audio on
# os: Darwin 25.6.0 x86_64
# command: ./build/wvt --list-devices
2026-10-20 19:51:53.739 [INFO ] [log] log file /tmp/wvt_log_20261020_195153.log
```

#### Console output

Without `-v`, the console shows only warnings and errors.

| Option | Function |
|---|---|
| `-v`, `--verbose` | Show messages at INFO level and above on the console, with a time stamp |
| `-v LEVEL`, `--verbose=LEVEL` | Show messages at `LEVEL` and above. `LEVEL` is `debug`, `info`, `warn` or `error` |
| `--log-dir DIR` | The folder for the log file |
| `--no-log-file` | Do not write a log file |

```sh
wvt big.sigmf-meta -v debug        # all messages on the console
wvt --device rtlsdr -v --log-dir ~/logs
```

#### Log levels

| Level | Use |
|---|---|
| DEBUG | Details for a developer, for example the time of each job |
| INFO | The actions of wvt, for example a file opens, a device starts, or a recording stops |
| WARN | A problem that wvt can continue after, for example a GPU fallback or dropped samples |
| ERROR | An action that failed |

#### Log components

The component tells you which part of wvt wrote the message:

| Component | Part |
|---|---|
| `main` | The command line and the programs that run without a window |
| `app` | The window and its actions |
| `config` | The configuration file and the config folder |
| `file` | Opening a file |
| `gpu` | The GPU backends and the fallback |
| `waterfall`, `engine` | The calculation of the file plots |
| `live`, `device`, `rtlsdr`, `uhd`, `bb`, `skiq` | Live streaming and the devices |
| `stream`, `zmq` | The network stream and the ZMQ message formats |
| `record` | Recording |
| `siggen`, `tx` | The signal generator and the transmitter |
| `audio` | The audio demodulator and the sound output |
| `extract` | The region export |
| `log` | The logger |

#### How the logger works

The logger is safe to use from all threads. It does not write from the real-time audio and SDR
callbacks, because a line write takes a lock and can write to the disk. The self-check
`build/log_check` tests the levels, the options, the log file and the threads.

### Configuration file

The configuration file sets the defaults of wvt: the folders, the file formats, the FFT values, the
GPU and a profile for each SDR. wvt reads the file when it starts.

#### Make the file

1. Write a file with all the keys and their defaults:

   ```sh
   wvt --write-config
   ```

   wvt writes `wvt.conf` in the [config folder](#the-config-folder). If there is no config folder,
   wvt makes `~/.config/wvt/` and writes the file there. To write a different file, give its name:
   `wvt --write-config FILE`.

2. Open the file in a text editor and change the values that you need.
3. Start wvt. The log shows `read <folder>/wvt.conf: N settings`.

wvt does not replace a file that exists. To replace it, add `--force`. To show the file on the
terminal, use `--write-config -`. In the repository, `config/wvt.conf.example` is a copy of the file
with the defaults.

#### The config folder

The config folder holds the user configuration files of wvt: `wvt.conf`, and the
[ZMQ message formats](#zmq-message-formats) (`zmq_*.ini`). wvt uses the first of these folders that
exists, and only that one:

1. The `--config-dir DIR` option. If the folder does not exist, wvt stops.
2. `config/` in the folder of the program.
3. `config/` in the folder above the program. When you run wvt from a build folder in the
   repository (for example `build/wvt`), this is the `config/` folder of the repository.
4. `~/.config/wvt/`.

wvt can use a folder in item 1, 2 or 3 while `~/.config/wvt/` also has configuration files. The log
and `--show-config` then tell you that wvt does not use the files in `~/.config/wvt/`.

#### Where wvt finds the file

wvt uses the first of these items:

1. The `--config FILE` option.
2. The `WVT_CONFIG` environment variable.
3. `wvt.conf` in the [config folder](#the-config-folder).

If you give the file with `--config` or `WVT_CONFIG` and the file does not exist, wvt stops. If
there is no config folder, or the folder has no `wvt.conf`, wvt uses the built-in defaults. To read
no file, use `--no-config`.

wvt does not read `$XDG_CONFIG_HOME/wvt/wvt.conf`. If you keep the file there, move it to
`~/.config/wvt/wvt.conf` or to a config folder.

#### Configuration file options

| Option | Function |
|---|---|
| `--config FILE` | Read this configuration file. If it does not exist, wvt stops |
| `--config-dir DIR` | Use this config folder. If it does not exist, wvt stops |
| `--no-config` | Read no configuration file. wvt uses the built-in defaults |
| `--write-config [FILE]` | Write a configuration file with all the keys, then stop. `-` writes to the terminal. Without FILE: `wvt.conf` in the config folder |
| `--force` | With `--write-config`: replace a file that exists |
| `--show-config` | Show each setting, its value and its source, then stop |

#### The order of the settings

A setting can come from four sources. A later source wins over an earlier source:

1. The built-in default.
2. The configuration file.
3. An environment variable, for example `WVT_GPU`.
4. A command-line option, for example `--gpu`.

A change in the window applies after all of them. A [file session](#file-sessions) also restores
its own values over the configuration file.

The transmit limits are different, because the strictest limit applies. Refer to
[Transmit limits](#transmit-limits).

To see each value and its source, use `--show-config`:

```text
$ WVT_GPU=off wvt --scale 1.5 --show-config
# config folder: /home/user/.config/wvt (home folder)
# configuration file: /home/user/.config/wvt/wvt.conf
fft.size              = 4096                            wvt.conf:14
gpu                   = off                             $WVT_GPU
ui.scale              = 1.5                             --scale
live.fft_size         = 2048                            default
```

#### The configuration file format

- Each line has one `key = value`.
- `#` starts a comment at the start of a line, or after a space or a tab. `skiq#0` is a value.
- Put a value in double quotes if it has a `#` after a space, or spaces at its start or end. In
  quotes, write `\"` for a quote and `\\` for a backslash.
- Keys can be in upper case or lower case.
- Numbers are in base units: Hz, S/s, seconds and dB. Write large numbers in scientific notation,
  for example `61.44e6` for 61.44 MS/s. wvt does not accept SI suffixes in the file. For `61.44M`,
  the warning tells you to use `61.44e6`.
- A folder starts with `/` or `~/`.

If a line is not correct, wvt writes a warning with the file name and the line number, and it does
not use that line. wvt uses all the other lines. For example:

```text
wvt: warning: wvt.conf:12: unknown key 'fft.sise' (did you mean 'fft.size'?)
wvt: warning: wvt.conf:20: sdr.0.rate: '61.44M' is not a number; use 61.44e6 (base units, no suffix); the line is not used
```

If a key is in the file two times, wvt uses the last line, with a warning.

#### Configuration keys

##### Folder keys

| Key | Default | Function |
|---|---|---|
| `recording.folder` | `~/wvt-recordings/` | The folder for recordings |
| `recording.name` | `wvt` | The start of a recording name: `<name>_<date>_<time>_<fc>_<rate>` |
| `snapshot.folder` | `~/wvt-recordings/` | The folder for [plot snapshots](#plot-snapshots) |
| `siggen.folder` | `~/wvt-recordings/` | The folder for signal generator files |
| `siggen.name` | `siggen` | The start of a signal generator file name |
| `audio.folder` | `~/wvt-recordings/` | The folder for audio WAV files |

##### File format keys

| Key | Default | Values |
|---|---|---|
| `recording.format` | `sigmf` | `sigmf`, `raw` |
| `recording.sample` | `native` | `native` (cu8 for RTL-SDR, ci16 for USRP, BB60 and Sidekiq), `cf32` |
| `recording.max_seconds` | `0` | The maximum length of a recording. `0` is no limit |
| `siggen.format` | `sigmf` | `sigmf`, `raw` |
| `siggen.sample` | `cf32` | `cf32`, `ci16`, `ci8`, `cu8` |
| `export.band_sample` | `cf32` | The format of a [region export](#export-a-region) with a band filter: `cf32`, `ci16` |

##### File waterfall keys

| Key | Default | Values |
|---|---|---|
| `fft.size` | `1024` | A power of two, 64 to 65536 |
| `fft.window` | `blackman-harris` | `rect`, `hann`, `hamming`, `blackman`, `blackman-harris`, `flat-top` |
| `fft.overlap_pct` | `50` | 0 to 95 |
| `display.colormap` | `viridis` | An ImPlot colormap, for example `viridis`, `plasma`, `hot`, `jet`, `greys` |
| `display.db_min`, `display.db_max` | `auto` | The dB range of the colors. A value turns off the automatic range |

##### Live spectrum keys

| Key | Default | Values |
|---|---|---|
| `live.fft_size` | `2048` | A power of two, 64 to 65536 |
| `live.window` | `blackman-harris` | The same values as `fft.window` |
| `live.avg` | `auto` | `auto` (all the new samples) or 1 to 64 FFTs for each row |
| `live.rows_per_sec` | `30` | 1 to 100 |
| `live.max_columns` | `2048` | 64 to 8192 |
| `live.history_rows` | `600` | 64 to 4096 |

##### General keys

| Key | Default | Function |
|---|---|---|
| `gpu` | `auto` | `auto`, `on` or `off`. Refer to [GPU processing](#gpu-processing) |
| `gpu.device` | The best GPU | The GPU number, as `--gpu-device` |
| `ui.scale` | `1` | The UI scale, 0.5 to 4 |
| `ui.geometry` | `1600x950` | The window size |
| `log.folder` | `$TMPDIR` or `/tmp` | The folder for the [log file](#the-log-file) |

##### Network stream keys

| Key | Default | Function |
|---|---|---|
| `zmq.address` | The address of this computer, port 5600 (else `127.0.0.1:5600`) | The default address of the [network stream](#stream-over-the-network) on this computer. The publisher (**Publish**) sends to it. The **ZMQ stream** device receives on it. `a.b.c.d[:port]`, or a multicast group `[iface;]group[:port]` |
| `zmq.ttl` | `1` | Multicast hops, 1 to 255. `1` keeps the stream on the local network. `--publish-ttl` changes it |
| `zmq.format` | `wvt`, the built-in format | The [message format](#zmq-message-formats) of the **ZMQ stream** device: `wvt`, or the `NAME` of `zmq_NAME.ini` in the config folder. `--zmq-format` changes it |
| `publish.format` | `wvt`, the built-in format | The [message format](#publish-with-a-format) of the publisher (**Publish**), as `zmq.format`. `--publish-format` changes it |

On the computer that publishes, set `zmq.address` to the address of the subscriber. On the
subscriber, leave it empty (the address of this computer) or set it to the address to receive on.
With a multicast group, set the same group on both computers. `--publish` always needs its
address. `zmq.address` is the address of the built-in format only. A format file has its own
address.

##### Transmit limit keys

| Key | Function |
|---|---|
| `tx.max_gain_db` | The maximum TX gain, in dB |
| `tx.allow` | The bands where wvt can transmit, for example `902e6:928e6, 2.4e9:2.5e9` |

These keys only add to the other [transmit limits](#transmit-limits). An option cannot remove a
limit of the file.

#### SDR profiles

A profile sets the values of one SDR type or of one unit. A profile has the keys `sdr.N.<field>`,
where `N` is a number from 0 to 99.

| Field | Devices | Function |
|---|---|---|
| `type` | All | The SDR type. This field is necessary |
| `serial` | All | Only the unit with this serial number |
| `args` | All | Only the unit with these device arguments, for example `"addr=192.168.40.2"` |
| `fc` | All | The center frequency, in Hz |
| `rate` | All | The sample rate, in S/s |
| `gain_db` | USRP, Sidekiq, RTL-SDR | The gain, in dB |
| `ref_level_dbm` | BB60 | The reference level, in dBm |
| `agc` | USRP, Sidekiq, RTL-SDR | `on` or `off` |
| `port` | USRP, Sidekiq | The antenna (`TX/RX`, `RX2`) or the RF port (`J1`) |
| `bandwidth` | USRP, BB60, Sidekiq | The bandwidth in Hz, or `auto` |
| `ref` | USRP | `internal`, `external` or `gpsdo` |
| `channel` | USRP | The receive channel |
| `atten` | BB60 | `auto`, `0`, `10`, `20` or `30` |
| `amp` | BB60 | `auto` or 0 to 3 |
| `ext_ref` | BB60, Sidekiq | `on` or `off` |
| `direct_sampling` | RTL-SDR | `off`, `i` or `q` |
| `bias_tee` | RTL-SDR | `on` or `off` |
| `ppm` | RTL-SDR | The frequency correction |

If a field does not apply to the type, wvt writes a warning and does not use the field.

##### SDR types

| Type | Devices |
|---|---|
| `b200`, `b205`, `b210`, `x310`, `n310`, `x410` | A USRP model. `b205` agrees with a B205mini |
| `usrp` | All USRPs |
| `bb60` | All Signal Hound BB60 devices |
| `mpcie`, `m2`, `m2-2280`, `z3u`, `x2`, `x4`, `nv100` | A Sidekiq part. The name must be the same: `x4` does not agree with an X40 |
| `sidekiq` | All Sidekiq cards |
| `rtlsdr` | All RTL-SDR devices |
| `sim` | The simulator |

##### How wvt selects a profile

A device uses the first profile that agrees with it:

1. The type agrees with the model of the device.
2. If the profile has a `serial`, the serial number of the device is the same.
3. If the profile has `args`, the device arguments include them.

Put the profiles for one unit before the profiles for a type. `wvt --list-devices` shows
`[config sdr.N]` after each device that has a profile.

A Sidekiq card that has a serial number shows with its serial number, not with its card number.
For that card, use `serial`, not `args = "#0"`.

##### The default SDR

`sdr.default` selects the device that wvt uses first:

- A type, for example `sdr.default = b205`: the first device of that type.
- A profile number, for example `sdr.default = 1`: the first device that profile 1 agrees with.

If the device is not connected, wvt writes a message in the log and uses the first device that it
finds.

##### Where the profiles apply

- **In the window:** when wvt selects a device, it uses the profile of that device.
- **With `--device`:** the profile sets the device values. The options win over the profile. For
  example, `--device uhd -r 10e6` uses the profile but a rate of 10 MS/s.
- **With `--no-gui --record` and no `--device`:** wvt records from the `sdr.default` device.

#### Configuration example

```ini
recording.folder  = ~/wvt/recordings/
snapshot.folder   = ~/wvt/snapshots/
siggen.folder     = ~/wvt/siggen/
fft.size          = 2048
gpu               = auto

sdr.default = b205

# Any B205mini
sdr.0.type    = b205
sdr.0.fc      = 100e6
sdr.0.rate    = 61.44e6
sdr.0.gain_db = 0
sdr.0.port    = TX/RX

# One X310, on its second channel
sdr.1.type    = x310
sdr.1.args    = "addr=192.168.40.2"
sdr.1.channel = 1
sdr.1.fc      = 2.45e9
sdr.1.rate    = 50e6

# Signal Hound BB60
sdr.2.type          = bb60
sdr.2.rate          = 40e6
sdr.2.ref_level_dbm = -20
```

For a file with all the keys, use `wvt --write-config -`.

### Command-line reference

```text
wvt [options] [file]
```

`wvt --help` shows all the options. These rules apply to the values:

- Values in Hz and seconds accept SI suffixes, for example `2.4M`, `250k`, `20u` or `5m`.
- A unit can follow the value, for example `2.4 MHz` or `10 MS/s`.
- wvt does not accept other text after the value (`1.5.2` or `12abc`).
- Whole-number options (for example `--seed`, `--gpu-device` or the `N` in `--chain-add PATH:N`)
  must be whole numbers in their range.

#### General options

| Option | Function |
|---|---|
| `--chan OFF:BW` | Start with the channel filter on, for example `--chan 300k:150k`. Refer to [The channel filter and the time plots](#the-channel-filter-and-the-time-plots) |
| `--view T0:T1` | Start with this time range, for example `--view 0.5:0.52` |
| `--software` | Use the software renderer. Refer to [Remote and virtual displays](#remote-and-virtual-displays) |
| `--scale S` | The UI scale. Use it on a high-resolution display |
| `--geometry WxH` | The initial window size (default 1600x950) |
| `--screenshot F` | Show one frame, write it to `F` as a PPM file, then stop |
| `--gpu auto\|on\|off` | GPU processing. Refer to [Select the GPU at run time](#select-the-gpu-at-run-time) |
| `--gpu-device N` | Use GPU `N` |
| `-h`, `--help` | Show the options, then stop |

#### Options of each function

The chapter of each function has the full table of its options:

| Function | Options | Table |
|---|---|---|
| File settings | `-f`, `-r`, `-c`, `-H`, `--swap-iq` | [How wvt finds the file settings](#how-wvt-finds-the-file-settings) |
| Segments | `--segment T0:T1` | [Set a segment](#set-a-segment) |
| Region export | `--extract`, `--extract-out` and related options | [Export on the command line](#export-on-the-command-line) |
| Audio | `--audio`, `--audio-out` and related options | [Audio on the command line](#audio-on-the-command-line) |
| Live devices, recording and the network stream | `--device`, `--record`, `--publish` and related options | [Live devices on the command line](#live-devices-on-the-command-line) |
| Signal generator and transmit | `--siggen`, `--gen`, `--tx` and related options | [Signal generator on the command line](#signal-generator-on-the-command-line) |
| Logging | `-v`, `--log-dir`, `--no-log-file` | [Console output](#console-output) |
| Configuration file | `--config`, `--write-config`, `--show-config` and related options | [Configuration file options](#configuration-file-options) |
| File sessions | `--session-out F` | [Save a session](#save-a-session) |
| Signal analysis | `--analyze`, `--analyze-out`, `--analyze-opt` | [Analysis on the command line](#analysis-on-the-command-line) |

#### Environment variables

| Variable | Function |
|---|---|
| `WVT_CONFIG` | The configuration file, as `--config` |
| `WVT_SCALE` | The UI scale, as `--scale` |
| `WVT_GPU` | `auto`, `on` or `off`, as `--gpu` |
| `WVT_LOG_DIR` | The folder for the log file, as `--log-dir` |
| `TMPDIR` | The folder for the log file, if `WVT_LOG_DIR` is not set |
| `WVT_TX_MAX_GAIN` | The maximum TX gain, as `--tx-max-gain` |
| `WVT_TX_ALLOW` | The bands where wvt can transmit, as `--tx-allow` |
| `WVT_GPU_FORCE_FAIL` | Set to `1` to make each GPU call fail. For tests of the CPU fallback |
| `DISPLAY` | The X server |

## Part 4: In the weeds

### Signal processing

This chapter tells you how wvt calculates each plot of the file viewer.

#### The waterfall calculation

- wvt calculates windowed FFTs with fftshift, in dBFS.
- The view fits in a maximum of *Rows* rows. When a row covers more time than one FFT, the row
  averages sufficient FFTs to cover its full time slice. Thus a short pulse cannot fall between
  two FFTs.
- *Avg* sets the minimum number of FFTs for each row.
- Above 256 M samples of FFT input (2 G on the GPU), wvt uses a sample of each slice. The settings
  row then shows the percentage of the view that the FFTs see.
- wvt max-pools adjacent bins to *Cols* columns. Thus narrow signals stay visible.

#### The spectrum calculation

The spectrum above the waterfall uses the linear power of each waterfall row:

- The average trace is the mean of the rows, in dB.
- The peak-hold trace is the maximum of the rows, in dB.

#### The channel filter calculation

The channel filter is optional. It does these steps:

1. It mixes the selected offset down to 0 Hz.
2. It applies a windowed-sinc FIR low-pass filter (Blackman window).
3. It decimates.

The automatic settings are: decimation = ⌊Fs / (2 · BW)⌋, and taps ≈ 12 · Fs / BW. With a GPU,
large full-resolution views do the mixer and the filter on the GPU. The mixer phase is a function
of the absolute sample number. Thus the results do not move when you pan.

#### The AM, FM and PM calculations

AM
: |x|, as linear, dB, or normalized m(t) = |x| / mean − 1. The modulation index is
  m = (max − min) / (max + min).

FM
: The instantaneous frequency arg(x[n] · x*[n−1]) · Fs / 2π. It is absolute (the offset from the
  capture center) or relative to the channel center. The readout shows the mean, the peak deviation
  and the RMS deviation.

PM
: The phase: wrapped, unwrapped, or unwrapped with the best-fit line removed. The last option
  removes a remaining carrier offset. The unit is radians or degrees.

Smoothing
: A moving average for each of AM, FM and PM.

Squelch
: Blanks FM and PM where the smoothed power is below the threshold.

#### Large views

- wvt memory-maps the files.
- wvt demodulates views up to *Max pts* samples (and approximately 32 M input samples) at full
  resolution.
- Wider views use **SPARSE** mode. This mode demodulates a short block for each screen pixel.
- All plots show the exact minimum and maximum envelope for each pixel.

#### Background work

Each calculation runs on its own worker thread. A new request replaces the old request. Thus a
setting change shows its effect immediately, and the UI does not stop.

### How the analysis works

This chapter tells you how the tabs of the Analysis window calculate their results. The code is in
`src/dsp/analysis.cpp`. It has no UI code. Thus the window, `--analyze-out` and the self-check use
the same code.

#### The channel samples

Each tool reads the samples through `ChannelReader` in `src/dsp/engine.h`. It gives the output of
the channel filter: the mixer, the FIR filter and the decimation, on the GPU or the CPU. When the
filter is off, it gives the samples of the file. Output `j` is at input sample `j · D`. Thus the
samples of a tool are at the same times as the time plots. A tool can also read each part of the
region separately.

The tools that read all the samples (measurements, spectrum and pulses) read blocks of 4 M
samples. The other tools read the first **Max samples** of the region into memory.

#### The measurement calculations

- The time-domain values come from the sums of I, Q, I², Q², I·Q and the power over all the
  samples.
- The gain imbalance is 10 log10(var(I) / var(Q)). The phase imbalance is
  asin(cov(I, Q) / √(var(I) · var(Q))).
- The spectrum is a Welch average with 50 % overlap. wvt scales each bin so that a full-scale
  tone in one bin reads 0 dBFS (the same scale as the waterfall).
- The carrier is the peak bin, with parabolic interpolation.
- The occupied bandwidth starts where the cumulative power is 0.5 % of the total. It ends where
  the cumulative power is 99.5 % of the total.

#### The spectrum and channel power calculations

The channel power is the sum of the bins in the band, divided by the equivalent noise bandwidth of
the window in bins. Thus the result is the total power in the band, for tones and for noise. The
RBW is the equivalent noise bandwidth times the bin width.

#### The CCDF calculation

wvt puts 10 log10(p / mean) of each sample into bins of 0.05 dB, and adds the bins from the top.
For complex Gaussian noise, the probability of a value over x dB is exp(−10^(x/10)) / exp(−1)
relative to the mean.

#### The pulse calculations

1. **Noise floor.** wvt takes the power of up to 64 blocks of 16384 samples across the region,
   with the smoothing. It uses the 20th percentile, divided by the 20th percentile of a gamma
   distribution with the same smoothing. Thus the result is the mean noise power.
2. **Detection.** A pulse starts where the smoothed power goes above the threshold. It ends
   where the power goes below the threshold minus the hysteresis.
3. **Measurement.** For each pulse, wvt reads the pulse again with a margin. The amplitude is the
   median of the envelope in the center 60 % of the pulse. The edges are the 10 %, 50 % and 90 %
   crossings of that amplitude, with linear interpolation.
4. **Frequency and chirp.** wvt calculates the instantaneous frequency in the center 80 % of the
   pulse. The frequency is its mean, weighted by the power. The chirp is the least-squares slope.

#### The correlation calculations

- The autocorrelation uses FFTs: R = IFFT(|FFT(x)|²), on up to 4 M samples, after wvt removes
  the mean.
- The template search uses overlap-save FFT correlation in blocks. The score is
  |Σ x[k+m] · t*[m]| / √(E_t · E_x[k]). E_t is the energy of the template, and E_x[k] is the
  energy of the samples under it. Thus the score is 1 for a perfect match at any level. A hit is
  the maximum of each run of scores over the threshold.

#### The symbol calculations

1. wvt makes a detector signal: the power (**Envelope**), |Δ|x|| (**Envelope changes**), or the
   size of the change of the instantaneous frequency (**Frequency changes**).
2. The symbol rate is the strongest line in the spectrum of the detector, with parabolic
   interpolation. Sharp symbol edges give harmonics that are as strong as the rate. Thus wvt
   selects the lowest sub-multiple of the strongest line that is within 6 dB of it.
3. The phase of the detector at the rate gives the time of the symbol centers.
4. The eye diagram folds the trace over two symbols, with linear interpolation between the
   samples, for the first 2 M samples.

#### The FAM calculation

1. Hamming-windowed FFTs of `Np` points (the channels) at a hop of `L = Np / 4`, with the phase
   correction for each block.
2. For each pair of channels, the product of one channel and the conjugate of the other, over the
   `P` blocks. Then a `P`-point FFT.
3. wvt keeps the center half of each FFT and puts each value at its alpha and frequency. The
   alpha resolution is `fs / (L · P)`.

#### The demodulation calculations

1. **Symbol rate.** OOK uses **Envelope changes**, 2-FSK uses **Frequency changes**, and BPSK and
   QPSK use the detector with the strongest line.
2. **Decision values.** OOK uses the magnitude. 2-FSK uses the instantaneous frequency, with a
   moving average of half a symbol. The threshold is the center between the two means of a
   two-means clustering. BPSK and QPSK first remove the carrier with the x^2 or x^4 peak.
3. **Timing.** A Gardner timing loop starts at the estimated symbol center. It samples with linear
   interpolation.
4. **Carrier phase (BPSK, QPSK).** wvt removes the phase of each block of 256 symbols, from the
   x^N phase of the block. QPSK uses Gray coding.

#### The analysis self-checks

- `build/analysis_check` tests each tool on signals with known values. Most of the signals come
  from the signal generator. The signals are tones with DC offset and I/Q imbalance, pulse trains
  with LFM and stagger, Barker codes, FSK, OOK and QPSK.
- The CTest `analysis_cli` runs each `--analyze` kind through wvt, and the refusals.
- The CTest `analysis_window` opens each tab in the window and takes a screenshot. It needs an X
  server. Without one, CTest skips it.

### GPU processing

wvt can calculate these items on the GPU:

- The file waterfall.
- The live spectrum.
- The channel filter of the time plots and of the region export.

wvt supports CUDA on an NVIDIA GPU under Linux, and Metal on an Apple GPU under macOS. With a GPU,
wide views can keep "FFT sees 100 %", and the live spectrum can analyze all samples at USRP rates.
The results are the same as on the CPU.

#### NVIDIA GPUs (CUDA, Linux)

For the build requirements, refer to [WVT_WITH_CUDA](#wvt_with_cuda). To run on the GPU, the
computer needs:

- An NVIDIA driver.
- A GPU with compute capability 7.5 or later (GeForce RTX 20xx and newer).
- A minimum of 1 GB of free GPU memory.

A CUDA build also runs on computers without a GPU or a driver. wvt then uses the CPU, and the log
tells you why.

#### Apple GPUs (Metal, macOS)

For the build requirements, refer to [WVT_WITH_METAL](#wvt_with_metal). You do not have to install
more software. To run on the GPU:

- The computer needs macOS 14 or later. Older versions use the CPU.
- The kernels compile when wvt uses the GPU for the first time (approximately 0.2 s).

On a Mac, the CPU path uses the Accelerate FFT (vDSP). Thus the CPU is also fast. On an M5, the
GPU gives these speeds, compared with the CPU:

| Job | Speed |
|---|---|
| Waterfall, N ≤ 4096 | Approximately 1.6 times |
| One live row | 2.5 to 3.4 times |
| Channel filter with many taps | Up to 6 times |

Small jobs stay on the CPU.

#### Select the GPU at run time

| Control | Function |
|---|---|
| `--gpu auto` | Use a GPU if one is usable. This is the default |
| `--gpu on` | Use the GPU. If no GPU is usable, wvt stops |
| `--gpu off` | Do not use the GPU. wvt does not load the driver |
| `WVT_GPU` | The environment variable for the same values |
| `--gpu-device N` | Use GPU `N` |
| `--list-devices` | Show the GPUs and the SDR devices |
| **Tools → Use the GPU** | Disable or enable the GPU in the window |

The status bar shows `GPU <model>` or `CPU`. The tooltip of each view tells you where wvt
calculated it.

#### GPU errors

If a GPU error occurs, that part of wvt uses the CPU for 1 s and then tries the GPU again. If the
errors continue, each wait is two times longer, up to a maximum of 60 s. When the GPU operates
correctly again, wvt uses the GPU again and writes `... is back on the GPU` in the log. The log and
the status-bar tooltip tell you about the error. To try the GPU again immediately, disable and
enable **Tools → Use the GPU**.

If the GPU has insufficient memory for a job, only that job uses the CPU.

wvt also uses the CPU, without an error, for jobs that are too small. Refer to the work limits in
[How the GPU code works](#how-the-gpu-code-works). On an M5, the CPU is as fast as the GPU for
these jobs. For example:

- A live row with fewer than 131072 samples. This occurs at low sample rates, with a manual
  **Avg** value, or for one row after a retune.
- A waterfall view with less than 16 M FFT input samples, for example a view that you zoomed in.

With `-v`, the log shows each change of the live spectrum between the GPU and the CPU, and the
reason.

To test the fallback without an error, set `WVT_GPU_FORCE_FAIL=1`. Each GPU call then fails, and
wvt uses the CPU.

#### Check the GPU

- `build/gpu_check` compares the GPU results with the CPU results.
- `build/gpu_check --bench` measures the speed of the GPU and the CPU.
- `build/gpu_check --require-gpu` (or `WVT_REQUIRE_GPU=1`) fails when no GPU is usable. Use it on a
  computer that has a GPU. Without it, `gpu_check` passes on a computer without a GPU, and it skips
  the GPU comparisons. The line `build: CUDA; GPU: ...` shows the GPU or the reason.

If the log shows `CUDA cannot start: unknown error (cudaErrorUnknown 999)`, the NVIDIA driver did
not start. wvt and all other CUDA programs then cannot use the GPU. Usually the cause is one of
these:

- The `nvidia_uvm` module is not loaded. `lsmod | grep nvidia_uvm` shows nothing and
  `/dev/nvidia-uvm` is missing. Load it with `sudo modprobe nvidia-uvm`. `nvidia-smi` works
  without it.
- The driver did not recover after a suspend. Load `nvidia_uvm` again, or restart the computer.
- The driver was updated and the computer was not restarted. `dmesg` shows an `NVRM: API mismatch`.
- You cannot open `/dev/nvidia*`.

#### How the GPU code works

`src/gpu/gpu.h` is the interface that the rest of wvt uses. `src/gpu/gpu_common.cpp` applies the
`--gpu` rules and the fallback to the CPU. Each backend implements the interface in
`src/gpu/gpu_backend.h`:

| Backend | File | FFT |
|---|---|---|
| CUDA | `src/gpu/gpu_cuda.cu` | cuFFT |
| Metal | `src/gpu/gpu_metal.mm` | FFTs up to 4096 points in one pass in the on-chip memory of the GPU. Larger FFTs use MPSGraph |
| None | `src/gpu/gpu_stub.cpp` | The CPU only |

On a Mac, the GPU reads the memory-mapped file in place, because the CPU and the GPU share the
memory. wvt sends a job to the GPU only when the job is sufficiently large. Each job type has its
own work limit.

### File sessions

A file session keeps the state of the file viewer in a small file. Thus you can go back to the
same work later. When you open the session, wvt opens the recording and shows it as it was.

A session file keeps these items:

| Item | Contents |
|---|---|
| The recording | Its path, the sample format, the header, I/Q swap, the sample rate, the center frequency and the segment |
| The view | The time span, the frequency span and relative frequency |
| The waterfall | The FFT size, the window, the overlap, the averages, the size limits, the dB range, Auto, the colormap and peak hold |
| The layout | The panel sizes, the overview strip, and which time plots show and fit their Y axis |
| The channel | On or off, the offset, the bandwidth, the taps, the decimation, and the AM, FM and PM settings and the squelch |
| Markers and bookmarks | The time and frequency markers, the point markers (P1 and P2), and all the bookmarks |
| The Analysis window | Open or closed, the open tab, the region, the channel filter switch, **Max samples**, and the settings of each tab. Not the results: wvt calculates them again |

A session does not keep the live mode, the signal generator, the audio window or the export
window.

#### Save a session

1. Open a recording, and set the view and the plots as you want them.
2. Select **File → Save file session...**.
3. Make sure that the path is correct. The default is the name of the recording with
   `.wvtsession.json`, in the folder of the recording.
4. Click **Save**.

wvt does not replace a file that exists. To replace it, tick **Replace an existing file**.

From the command line, `--session-out F` opens the recording with the other options, writes
the session to `F`, then stops:

```sh
wvt cap.sigmf-meta --view 10:10.5 --chan 300k:150k --session-out cap.wvtsession.json
```

#### Open a session

Type the path of the session file in the **File** box and press Enter, or give it on the command
line:

```sh
wvt cap.wvtsession.json
```

wvt looks at the contents of the file, not only its name. A session file is a small JSON file
with a `wvt_session` member. Thus wvt never opens a recording (for example a `.bin` file) or SigMF
metadata as a session.

The session keeps the path of the recording in two forms: the full path, and the path from the
folder of the session file. wvt tries the second form first. Thus, if you move a folder that has
the recording and its session, the session still finds the recording.

#### The session file format

The session file is JSON. You can read and change it with a text editor. This is a part of a
session file. `...` shows where members are not shown:

```text
{
  "wvt_session": 1,
  "recording": {"path": "/data/cap.sigmf-data", "relative_path": "cap.sigmf-data", "datatype": "ci16_le",
                "header_bytes": 0, "swap_iq": false, "sample_rate": 2400000, "center_freq": 100000000,
                "segment_s": [-1, -1]},
  "view": {"t0_s": 10, "t1_s": 10.5, "f0_hz": 98800000, "f1_hz": 101200000, "relative_freq": false},
  "waterfall": {"nfft": 1024, "window": "Blackman-Harris", "overlap_pct": 50, ...},
  "layout": {...},
  "channel": {"enabled": true, "offset_hz": 300000, "bandwidth_hz": 150000, ...},
  "markers": {...},
  "points": {"shown": true, "p1": {"plot": "am", "t_s": 10.12}, "p2": {"plot": "waterfall", "t_s": 10.15, "f_hz": 100300000}},
  "bookmarks": [{"name": "burst", "t0_s": 10.1, "t1_s": 10.2}],
  "analysis": {"open": true, "tab": "pulses", "region": "view", ...}
}
```

- `segment_s` -1 is the start or the end of the file.
- `plot` of a point marker is `waterfall`, `spectrum`, `i`, `q`, `am`, `fm` or `pm`. A marker on the
  waterfall has `t_s` and `f_hz`. A marker on the spectrum has `f_hz`. A marker on a time plot has
  `t_s`.
- A member that is not in the file gets its default value. Only `recording` is necessary.
- `analysis` keeps the Analysis window on one line. Its `tab` and `region` use the names of
  `--analyze` and of the region (`view`, `markers`, `bookmark` or `segment`).
- wvt refuses a session file from a newer wvt (a larger `wvt_session` number).
- The self-check `build/session_check` tests the write and the read, a moved folder, and the
  session files and recordings that wvt must tell apart.

### ZMQ message formats

The **ZMQ stream** device receives the stream of another wvt in the built-in format `wvt`. It can
also receive samples from other programs, for example GNU Radio. A format file tells wvt the
structure of the messages from such a program. The publisher (**Publish**) can send in the same
formats. This chapter is the reference for the format files.

For how to receive a stream, refer to [Receive other formats](#receive-other-formats).

#### Where wvt finds the format files

A format file is `zmq_NAME.ini` in the [config folder](#the-config-folder). `NAME` is the
name of the format: for `zmq_gnuradio_sub_cf32.ini`, it is `gnuradio_sub_cf32`. wvt reads only the
config folder that it uses. It does not read the files in the other folders.

The format `wvt` is always available, also without a config folder. Do not name a file `zmq_wvt.ini`:
the name `wvt` is the built-in format, and wvt shows an error for that file.

wvt reads the files again each time that you open the **Format** list. You do not have to start wvt
again after you add or change a file.

#### Check a format file

1. Put the file in the config folder.
2. Run this command:

   ```sh
   wvt --list-zmq-formats
   ```

wvt shows each format with its file, its socket and its samples. A file that has a problem shows the
error. Most errors include the line number. A key that wvt does not know gives a warning, with the key that you
possibly mean. The command stops with exit code 1 if a file has an error, so that a script can use
it.

```text
ZMQ message formats (/home/user/wvt/config):
* wvt                    the wvt stream (built-in)
      dish, udp bind, the wvt header
  gnuradio_sub_cf32      GNU Radio PUB Sink (cf32)
      file:  /home/user/wvt/config/zmq_gnuradio_sub_cf32.ini
      sub, tcp connect 127.0.0.1:5555, cf32 (complex float32)
* = zmq.format, the default of --device zmq
```

In the **Format** list, a file with an error shows, but you cannot select it. Its tooltip shows the
error.

#### The format file

The file is an INI file:

- A `[section]` line starts a section. The sections are `[format]`, `[socket]`, `[header]`,
  `[samples]` and `[stream]`.
- Each setting is `key = value`.
- `#` or `;` starts a comment, at the start of a line or after a space.
- Write numbers as in `wvt.conf`, for example `2.4e6`. Do not use SI suffixes such as `2.4M`.

This example is `config/zmq_example_header.ini` in the repository. Copy it to start a new file:

```ini
[format]
name        = Example header (ci16 BE)
description = 32-byte header, then big-endian ci16 samples

[socket]
type      = sub
transport = tcp
mode      = connect
address   = 127.0.0.1:5557
topic     =
frames    = last
max_bytes = 1048576

[header]
bytes       = 32
match.magic = "ABCD" @ 0         # a message that does not match is ignored
match.type  = u8 @ 4 == 1
seq         = u32be @ 8          # message counter
fc          = f64be @ 12
rate        = u32be @ 20 * 1e3   # the field is in kS/s
time_ns     = u64be @ 24

[samples]
format = ci16
endian = big
order  = iq

[stream]
gain = 0        # the header has no gain
```

##### The [format] section

| Key | Default | Function |
|---|---|---|
| `name` | The `NAME` of the file | The text in the **Format** list |
| `description` | Empty | The tooltip in the **Format** list |

##### The [socket] section

| Key | Default | Function |
|---|---|---|
| `type` | Necessary | The ZMQ socket that wvt opens: `dish`, `sub` or `pull`. The source sends with `radio`, `pub` or `push` |
| `transport` | `udp` for `dish`, else `tcp` | `dish` works only on `udp`. `sub` and `pull` work only on `tcp` |
| `mode` | `bind` for `dish`, else `connect` | `bind`: wvt listens and the source sends to wvt. `connect`: wvt connects to the source. `dish` works only with `bind` |
| `address` | Empty | The address when the **Address** field is empty. `host:port`. With `mode = bind`, `*:port` listens on all interfaces |
| `topic` | Empty (all messages) | `sub` only: wvt receives only the messages that start with this text |
| `group` | `wvt` | `dish` only: the group to join, 1 to 255 characters |
| `frames` | `last` | A message with more than one frame (for example a topic frame, then the data): the frame with the data. `last`, or 1 to 16 |
| `max_bytes` | `1048576` | The largest frame, 64 bytes to 64 MiB. libzmq drops a larger frame. On `tcp`, libzmq also closes the connection to the source |

The address of a `dish` format is as for the built-in format: an address of this computer, or a
multicast group. Refer to [The address](#the-address).

wvt limits the queue of messages to approximately 160 MB. A larger `max_bytes` gives a shorter
queue. Set `max_bytes` near the size of the largest message from the source.

##### The [header] section

The header is the bytes at the start of each message. The samples start after it.

| Key | Function |
|---|---|
| `bytes` | The size of the header, 0 to 65536 bytes. `0`: the message has only samples |
| `match.NAME` | Bytes that each message must have. wvt ignores a message that does not match. You can have more than one match, each with its own `NAME` |
| `fc` | The center frequency, in Hz |
| `rate` | The sample rate, in S/s |
| `gain` | The gain, in dB |
| `time_ns` | The time of the first sample, in ns since 1970. wvt reads it, but does not use it now |
| `index` | The index of the first sample: a counter of samples since the start of the stream |
| `seq` | A message counter: 1 more in each message |
| `count` | The number of samples in the message. Without it, all the bytes after the header are samples |

A field is `TYPE @ OFFSET`, for example `u32be @ 8`. `OFFSET` is the position of the first byte in
the message. The first byte of the message has the offset 0. The field must be in the header.

`fc`, `rate`, `gain` and `time_ns` can have a scale: `TYPE @ OFFSET * SCALE`. wvt multiplies the value
by the scale. For example, `rate = u32be @ 20 * 1e3` reads a rate in kS/s. `index`, `seq` and `count`
must have an integer type and no scale.

The types are:

| Type | Size | Value |
|---|---|---|
| `u8`, `i8` | 1 byte | Unsigned or signed integer |
| `u16le`, `u16be`, `i16le`, `i16be` | 2 bytes | Integer, little endian (`le`) or big endian (`be`) |
| `u32le`, `u32be`, `i32le`, `i32be` | 4 bytes | Integer |
| `u64le`, `u64be`, `i64le`, `i64be` | 8 bytes | Integer |
| `f32le`, `f32be` | 4 bytes | Floating point |
| `f64le`, `f64be` | 8 bytes | Floating point |

A match has one of these forms:

- `TYPE @ OFFSET == VALUE`: a number. For example, `match.type = u8 @ 4 == 1`.
- `"TEXT" @ OFFSET`: the bytes of a text. For example, `match.magic = "ABCD" @ 0`.

Use a match when the source sends more than one type of message. Then wvt uses only the messages
with samples.

##### The [samples] section

| Key | Default | Function |
|---|---|---|
| `format` | Necessary | The samples: `cu8`, `ci8`, `ci16`, `ci32`, `cf32` or `cf64`. Each sample is an I value and a Q value |
| `endian` | `little` | The byte order of each value: `little` or `big` |
| `order` | `iq` | `iq`: I first. `qi`: Q first |

wvt keeps `cu8`, `ci16` and `cf32` as they are. It changes the other formats to `cf32`.

##### The [stream] section

The values that the header does not give.

| Key | Function |
|---|---|
| `rate` | The sample rate in S/s. It is necessary if the header has no `rate` field |
| `fc` | The center frequency in Hz (default 0). wvt shows it on the frequency axis |
| `gain` | The gain in dB (default 0) |

A field in the header wins over a value in `[stream]`.

#### Lost messages

wvt finds lost messages with a counter in the header:

- With `index`, wvt knows the exact number of lost samples. If the index goes back by more than the
  samples of 1 s, wvt shows that the source started again.
- With `seq`, wvt counts the lost messages. The number of lost samples is the lost messages
  multiplied by the samples of one message, so it is an estimate. The counter can go back to 0 when it
  is at its maximum value.
- Without `index` and `seq`, wvt does not find lost messages. TCP does not lose messages, but a
  `pub` socket drops messages when the subscriber is too slow.

The status bar shows the lost samples as *network loss*, and a recording has an annotation for each
gap. Set `index` or `seq`, not both.

If the sample rate in the header changes, wvt stops the device. Click **Start** again. If the center
frequency in the header changes, wvt retunes at the first sample of that message.

#### Publish with a format

wvt can send the live stream in a format file, so that another program receives it. One file
describes both sides: the publisher sends the messages that a subscriber with the file receives.

1. Start a device.
2. Click the ⚙ button next to **Publish**.
3. In **Format**, select the format file.
4. In **Address**, type the address, or leave it empty to use the address of the file.
5. Click **Publish**.

From the command line, use `--publish ADDR --publish-format NAME`. To use the address of the file,
type `--publish ""`. To use a format each time, set
[`publish.format`](#network-stream-keys) in the configuration file.

wvt uses the socket that fits the socket of the file, and the other mode:

| The file (the subscriber) | wvt sends with |
|---|---|
| `type = dish` (`bind`) | RADIO, to the address of the subscriber or a multicast group |
| `type = sub`, `mode = connect` | PUB that binds on the address. `*:port` listens on all interfaces |
| `type = sub`, `mode = bind` | PUB that connects to the address of the subscriber |
| `type = pull`, `mode = connect` | PUSH that binds on the address |
| `type = pull`, `mode = bind` | PUSH that connects to the address of the subscriber |

wvt fills the header from the device:

- `fc`, `rate` and `gain`: the values of the device. If a value does not fit in its field, wvt does
  not start, or it stops publishing with a message. For example, 2.4 GHz does not fit in a `u16`
  field without a scale.
- `index`: the samples since the start of publishing. `seq`: the messages since the start.
- `time_ns`: the time of the first sample of the message.
- `count`: the samples in the message.
- The matches: their bytes. Other header bytes are 0.

wvt changes the samples from the format of the device to the format of the file. A message has at
most 8064 bytes on `udp`. On `tcp`, it has at most `max_bytes` and 256 KiB.

Some files cannot publish, and the **Format** list shows them disabled:

- A `sub` format with a `topic` needs `frames = last` or `frames = 2`. wvt sends the topic in frame
  1 and the data in frame 2.
- The other formats need `frames = last` or `frames = 1`.
- The header must leave space for one sample in `max_bytes`.

If the file has no `rate` field, the subscriber takes the rate from `[stream]`. If the device has
another rate, wvt writes a warning in the log. Set `rate` in `[stream]` to the rate of the device.

```{warning}
A PUSH socket without a subscriber, and a PUB or RADIO socket on a slow network, drop messages.
wvt shows the dropped samples as *skipped*.
```

Only the built-in format sends the info message and the end message. A subscriber of a format file
does not get the description of the device, and it does not see when the publisher stops.

#### The example format files

The repository has these examples in `config/`:

| File | Source |
|---|---|
| `zmq_gnuradio_sub_cf32.ini` | A GNU Radio *ZMQ PUB Sink* with the type complex and *Pass Tags* off. The rate and the frequency come from `[stream]`: set them to the values of the flowgraph |
| `zmq_raw_pull_ci16.ini` | Raw `ci16` samples from a ZMQ PUSH socket. The rate and the frequency come from `[stream]` |
| `zmq_example_header.ini` | A template for a message with a binary header. Copy it and change it for your source |
| `zmq_wvt_ci16.ini` | The built-in format as a file, for `ci16` data messages only. It is a template, and the self-checks use it |

A GNU Radio *ZMQ PUB Sink* with the address `tcp://*:5555` binds on port 5555. wvt connects to it on
`127.0.0.1:5555`, or on the address of the computer that runs GNU Radio. With tags on, the sink adds
a header that these files do not read. Set *Pass Tags* to off.
