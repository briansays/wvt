# Build reference

This page gives all the CMake settings of wvt, what each setting enables or disables, and the
programs that the build makes.

## Configure and compile

```sh
cmake -S . -B build [OPTIONS]
cmake --build build -j
```

Give each option as `-DNAME=VALUE`. CMake keeps the values in `build/CMakeCache.txt`. Thus, when
you configure the same build folder again, you do not have to give the options again. To go back
to the default of an option, delete `build/CMakeCache.txt`, or give the default value.

## Options

### Summary

| Option | Values | Default | Platform |
|---|---|---|---|
| [`CMAKE_BUILD_TYPE`](#cmake_build_type) | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel` | `Release` | All |
| [`X11_ROOT`](#x11_root) | A folder | `/opt/X11` | macOS |
| [`WVT_WITH_RTLSDR`](#wvt_with_rtlsdr) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_UHD`](#wvt_with_uhd) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_BB`](#wvt_with_bb) | `AUTO`, `ON`, `OFF` | `AUTO` | Linux, macOS ARM |
| [`SIGNALHOUND_SDK_DIR`](#signalhound_sdk_dir) | A folder | Empty | Linux, macOS ARM |
| [`WVT_BB_SIM`](#wvt_bb_sim) | `ON`, `OFF` | `OFF` | All |
| [`WVT_WITH_AUDIO`](#wvt_with_audio) | `AUTO`, `ON`, `OFF` | `AUTO` | All |
| [`WVT_WITH_CUDA`](#wvt_with_cuda) | `AUTO`, `ON`, `OFF` | `AUTO` | Linux |
| [`WVT_CUDA_STATIC`](#wvt_cuda_static) | `ON`, `OFF` | `ON` | Linux |
| [`CMAKE_CUDA_ARCHITECTURES`](#cmake_cuda_architectures) | A list of GPU architectures | Refer to the section | Linux |
| [`WVT_WITH_METAL`](#wvt_with_metal) | `AUTO`, `ON`, `OFF` | `AUTO` | macOS |
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

### CMAKE_BUILD_TYPE

Sets the compiler optimization. If you do not give a value, wvt uses `Release`.

- `Release` compiles with `-O3 -DNDEBUG`. Use this value for normal use. The waterfall and the
  demodulators are much slower without optimization.
- `Debug` compiles with `-g` and no optimization. Use it with a debugger.
- `RelWithDebInfo` compiles with optimization and debug symbols.

### X11_ROOT

macOS only. Sets the folder of XQuartz. The default is `/opt/X11`. The build links
`${X11_ROOT}/lib/libX11.dylib` and `${X11_ROOT}/lib/libGL.dylib`, and uses the headers in
`${X11_ROOT}/include`. If `libX11.dylib` is not in this folder, the configure step stops.

On Linux, CMake finds X11 and OpenGL with its standard `FindX11` and `FindOpenGL` modules. The
build uses the legacy `libGL.so`, which has the GL and the GLX functions.

### WVT_WITH_RTLSDR

Enables or disables the RTL-SDR device backend (`src/sdr_rtlsdr.cpp`).

- CMake finds librtlsdr with `pkg-config` (the `librtlsdr` module).
- CMake also checks for `rtlsdr_set_bias_tee()`. This function is in librtlsdr builds from 2018
  and later. Without it, the **bias-tee** option is not available.
- Without this backend, the device list does not show RTL-SDR devices.

Output: `wvt: RTL-SDR support enabled (librtlsdr <version>)`.

### WVT_WITH_UHD

Enables or disables the USRP backend (`src/sdr_uhd.cpp`). This backend receives and transmits, with
all USRPs that UHD supports: the USB B200 family and the network N3xx, X3xx and X4xx.

- CMake finds UHD with `pkg-config` (the `uhd` module). If `pkg-config` does not find it, CMake
  uses the `UHDConfig.cmake` file of UHD.
- wvt uses the C API of UHD (`uhd.h`). Thus the build does not need the Boost headers.
- Without this backend, wvt cannot receive from a USRP, and the signal generator cannot transmit.

Output: `wvt: UHD support enabled (UHD <version>)`.

### WVT_WITH_BB

Enables or disables the Signal Hound BB60 backend (`src/sdr_bb60.cpp`). It needs `bb_api` from the
Signal Hound SDK. Refer to [Live devices](live.md#signal-hound-bb60).

- CMake looks for `bb_api.h` and the library in `SIGNALHOUND_SDK_DIR`. It uses the library for the
  CPU of the build: Linux x86-64 (the newest version 5 library), Linux ARM64, or macOS ARM.
- CMake copies the library into `build/bb_api/` under its run-time name. wvt finds it there
  through its rpath. On Linux x86-64, CMake also copies `libftd2xx.so`.
- An x86-64 macOS build cannot use the ARM library. CMake then tells you to configure with
  `-DCMAKE_OSX_ARCHITECTURES=arm64`.
- Without this backend, the device list does not show BB60 devices.

Output: `wvt: Signal Hound BB60 support enabled (bb_api 5.0.x, <library>)`.

### SIGNALHOUND_SDK_DIR

The folder of the extracted Signal Hound SDK, for example `$HOME/signal_hound_sdk`. A prefix
with `include/bb_api.h` and `lib/libbb_api.*` also works. If the library is installed in a system
folder, such as `/usr/local/lib`, CMake finds it without this option.

### WVT_BB_SIM

`ON` builds the BB60 backend against a simulated BB60D (`tools/bb_sim/`) instead of the SDK. Use
it for CI and tests. The build then also makes `bb_check`, which tests the backend. Do not use it
for normal use: the device list then shows the simulated BB60.

### WVT_WITH_AUDIO

Enables or disables audio playback through the bundled miniaudio library.

- On macOS, the build links CoreAudio, AudioToolbox and CoreFoundation.
- On Linux, the build does not need development packages. At run time, miniaudio loads
  PipeWire, PulseAudio or ALSA, whichever is available.
- `AUTO` and `ON` have the same result, because the library is in the repository.
- With `OFF`, the audio window cannot play sound. The demodulator and the WAV export still work.

Output: `wvt: audio playback enabled (miniaudio)`.

### WVT_WITH_CUDA

Enables or disables the CUDA GPU backend (`src/gpu_cuda.cu`) for NVIDIA GPUs. Refer to
[GPU processing](gpu.md).

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

### WVT_CUDA_STATIC

Sets how the build links the CUDA runtime and cuFFT. Applies only when the CUDA backend builds.

- `ON` (default) links the CUDA runtime and cuFFT into the binary. The binary is approximately
  270 MB larger. It needs only the NVIDIA driver at run time.
- `OFF` links the shared libraries. The binary is small, but the computer that runs it must have
  the CUDA runtime and cuFFT libraries.

### CMAKE_CUDA_ARCHITECTURES

Sets the GPU architectures that the CUDA code is compiled for. Applies only when the CUDA backend
builds. If you do not give a value, wvt uses:

| CUDA version | Architectures | GPUs |
|---|---|---|
| 12.0 to 12.7 | `75-real 80-real 86-real 89-real 90` | RTX 20xx to 40xx, A100, H100, plus PTX for newer GPUs |
| 12.8 and later | `75-real 80-real 86-real 89-real 90-real 120` | The same, plus RTX 50xx |

A shorter list makes a smaller binary and a faster build. For example, for an RTX 4090 only, use
`-DCMAKE_CUDA_ARCHITECTURES=89`. The GPU must have compute capability 7.5 or later.

### WVT_WITH_METAL

macOS only. Enables or disables the Metal GPU backend (`src/gpu_metal.mm`) for Apple GPUs. Refer
to [GPU processing](gpu.md).

- The backend needs an Objective-C++ compiler and the Metal and MetalPerformanceShadersGraph
  frameworks. The Command Line Tools have all of them. You do not need Xcode.
- The GPU kernels compile when wvt uses the GPU for the first time (approximately 0.2 s).
- The GPU path needs macOS 14 or later at run time. Older versions use the CPU.
- On Linux, `ON` causes an error, and `AUTO` disables the backend.

Output: `wvt: Metal support enabled (MPSGraph FFT; the GPU path needs macOS 14 at run time)`.

### WVT_SANITIZE

Builds all the programs with compiler sanitizers. Use it to test, not for normal use: the
programs are slower and use more memory.

- `address`: AddressSanitizer and UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`). They
  find out-of-bounds access, use after free, memory leaks (Linux) and undefined behavior. An
  undefined-behavior error stops the program.
- `thread`: ThreadSanitizer (`-fsanitize=thread`). It finds data races between threads. You cannot
  use it together with `address`.
- Empty (the default): no sanitizers.

The CUDA code (`gpu_cuda.cu`) does not get the sanitizer flags. Use a separate build folder for
each value. Refer to [Self-checks](self-checks.md#sanitizer-builds).

Output: `wvt: sanitizers enabled (address: -fsanitize=address,undefined;...)`.

### WVT_FUZZ

`ON` also builds the libFuzzer targets `fuzz_json`, `fuzz_chain`, `fuzz_meta` and `fuzz_parse_si`,
with AddressSanitizer and UndefinedBehaviorSanitizer. CTest then also runs each target on its seed
inputs. The option needs clang: with another compiler, the configure step stops with an error. On
macOS, use the clang of the Homebrew `llvm` package, because Apple clang has no libFuzzer. Refer
to [Fuzz tests](self-checks.md#fuzz-tests).

Output: `wvt: libFuzzer targets enabled (fuzz_json, fuzz_chain, fuzz_meta, fuzz_parse_si)`.

## The CPU FFT

The CPU FFT is not an option. On macOS, the build always links the Accelerate framework and uses
its vDSP FFT. On Linux, wvt uses its own radix-2 FFT.

## Examples

A Linux build with the two device backends, and the configure step stops if one is missing:

```sh
cmake -S . -B build -DWVT_WITH_RTLSDR=ON -DWVT_WITH_UHD=ON
```

A minimum build: the file viewer, the simulator and the signal generator file output only:

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

## Build targets

`cmake --build build` makes all these programs. To make one program only, add `--target NAME`.

| Target | Contents |
|---|---|
| `wvt` | The application |
| `iqgen` | Writes a synthetic test recording |
| `siggen_check` | Self-check for the signal generator |
| `audio_check` | Self-check for the audio demodulator |
| `log_check` | Self-check for the logger |
| `segment_check` | Self-check for the time segment rules |
| `extract_check` | Self-check for the region export |
| `snapshot_check` | Self-check for the plot snapshots |
| `device_check` | Self-check for the device list and the sample rate rules |
| `bb_check` | Self-check for the BB60 backend, with the simulated device. Only with `WVT_BB_SIM=ON` |
| `gpu_check` | Compares the GPU results with the CPU results, and measures the speed |
| `fuzz_json`, `fuzz_chain`, `fuzz_meta`, `fuzz_parse_si` | The fuzz targets. Only with `WVT_FUZZ=ON` |

`ctest --test-dir build --output-on-failure` runs all the self-checks.

For the self-checks, refer to [Self-checks and test signals](self-checks.md).

## Version information

At configure time, CMake reads the git revision with `git describe --always --dirty`. wvt writes
this revision to the start of each log file. If you change the source, configure again to update
the revision.

## Build the documentation

The documentation uses Sphinx with the MyST Markdown parser. To make the HTML pages:

```sh
pip install -r docs/requirements.txt
sphinx-build -W -b html docs build/docs
```

Open `build/docs/index.html` in a browser. The `-W` option makes each warning an error. CI uses
the same command. For the writing rules, refer to [Writing the documentation](writing-docs.md).
