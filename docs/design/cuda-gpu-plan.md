# Plan: CUDA GPU acceleration (issue #3)

Branch: `3-cuda-gpu`, from `main` at `29dca41` (after the logging merge, PR #15).
Status: **steps 0–8 implemented** (2026-10-19). The CPU-only build is tested on
macOS. The CUDA build is compiled and tested in CUDA 12.6 and 12.8 containers with no GPU.
**The GPU code has not run on a GPU yet** (see §9).

Earlier GPU work (`wvt-gpu-dev`, PR #5) is out of date. This plan starts again from `main`
and does not use that branch.

## 1. Goal

Issue #3 asks for these items:

1. Do the heavy DSP on an NVIDIA GPU when the computer has one. This reduces the time
   between the device and the display.
2. Render images on the GPU where it is useful.
3. Add CMake flags that turn the CUDA features on or off, based on the GPU or its drivers.

Hard rules:

- The GPU result must look the same as the CPU result.
- Every computer without a usable GPU must build, start and work exactly as `main` does
  now. This includes macOS, AMD or Intel graphics, and computers with no NVIDIA driver.

## 2. Starting point (`main`)

### Where the CPU time goes

| Computation | Code | Work | GPU value |
|---|---|---|---|
| File waterfall + spectrum | `computeWaterfall`, `src/engine.cpp` | rows × avg FFTs of size N, \|X\|², fftshift, average, column max-pool, log10. Input budget 256 M samples (`kMaxInputSamples`). Past it, "FFT sees" drops below 100 %. | **High** |
| Live spectrum | `LiveEngine::spectrumLoop`, `src/live.cpp` | Same math on the newest ring samples. One thread. Auto averaging stops at 20 MS/s (`kMaxAnalyzeRate`). A USRP B2xx at 56 MS/s is not fully analyzed. | **High** |
| Channel filter for the plots | `processRange`, `src/engine.cpp` | NCO mix + direct FIR with up to 8191 taps over up to 32 M input samples, then atan2 / unwrap / smoothing on the decimated output | **Medium**. Only the NCO + FIR are worth moving. |
| Audio demod | `src/audio_dsp.cpp` | Streaming FIR + resampler on small real-time blocks | **None**. The blocks are too small, and a kernel launch would add latency. |
| Overview strip | `computeOverview` | 4 × 2048-sample probes per bucket | None (disk bound) |
| Siggen preview, playback resampler | `src/siggen_out.cpp`, `src/siggen.cpp` | Small or one-time work | None |

The FFT (`src/dsp.cpp`) is a scalar radix-2 FFT. The waterfall uses `parallelFor` over
rows. Each FFT reads and decodes its own samples through `IqFile::read`. The file is
memory-mapped.

### Rendering today

- The UI is ImGui + ImPlot with the OpenGL 2 backend over GLX. When GLX works, the GPU
  already draws the frame.
- When GLX does not work, `src/sw_renderer.cpp` draws on the CPU. This happens with some
  XQuartz or remote X11 setups.
- The waterfall image is at most 1024 × 2048. The CPU changes dB values to colors with a
  256-entry lookup table (`src/app.cpp`, `src/app_live.cpp`), then sends the image with
  `glTexImage2D`.

That is a small part of the frame time. CUDA–OpenGL interop would need a direct GLX
context on the NVIDIA driver. It cannot work over remote X11, which is a main use of wvt.
**Recommendation: no GPU work for rendering** (see §8). Measure this first (step 0).

### Build

- `CMakeLists.txt` needs CMake 3.16. Optional parts use `WVT_WITH_<X>` = `AUTO` / `ON` /
  `OFF` (RTL-SDR, UHD, audio).
- Self-test tools: `siggen_check`, `audio_check` and `log_check`.
- The repository has no CI.
- The development computer is a Mac. It has no CUDA.

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Build switch | `WVT_WITH_CUDA` = `AUTO` / `ON` / `OFF`, default `AUTO`. `AUTO` builds the CUDA code when the CUDA toolkit is found. `ON` stops the build if the toolkit is not found. | Same pattern as the other optional parts. |
| "Based on presence of a GPU" | **Build time:** CMake checks for the CUDA toolkit (nvcc + cuFFT). **Run time:** wvt checks for the GPU and the driver. | The computer that builds wvt is often not the one that runs it. A CUDA build must still run on a computer with no GPU. |
| CMake details | `include(CheckLanguage)`, `check_language(CUDA)`, then `enable_language(CUDA)` and `find_package(CUDAToolkit)`, all inside the CUDA block. The CUDA block needs CMake 3.18 or later; with an older CMake, skip it with a status line. On Apple, skip it, and stop the build if `ON`. | Non-CUDA builds do not change. |
| Code layout | `src/gpu.h`: plain C++ interface, no CUDA headers. `src/gpu_cuda.cu`: kernels + cuFFT. `src/gpu_stub.cpp`: build without CUDA, "not available". `src/gpu_common.cpp`: code shared by both builds (option parsing, status text). | Callers stay plain C++. Same "one file per backend" style as `sdr_*.cpp`. The interface can later take a Vulkan or HIP backend. |
| FFT | cuFFT, single-precision C2C, batched plans | Fits "rows × avg" FFTs. |
| Data transfer | Copy **raw** samples (cu8, ci16, ...) to the GPU through pinned buffers. Decode, window and FFT on the GPU. Copy back only rows × cols floats. | Two to eight times less PCIe traffic than decoded cf32. The output is small. |
| Geometry stays in C++ | The engine still computes rows, hop, avg, cols, f0/f1, the spectrum and the percentiles. The GPU code replaces only the inner loop. | The two paths cannot drift apart on layout. |
| Concurrency | One CUDA context. Each user thread (the waterfall worker, the live spectrum thread) owns its own stream, buffers and plan cache. | cuFFT plans are not thread-safe to share. Streams let the live and file paths overlap. |
| Cancel | Process in chunks. Check `cancelled()` between chunks. | Keeps the "latest request wins" behavior of `Worker` (`src/engine.h`). |
| Fallback | Each GPU function returns `false` on any error. The caller then runs the CPU code for that request. After an error other than "out of memory", that path stays on the CPU for the rest of the session. | Never lose a result because of the GPU. |
| Run-time switch | `--gpu auto\|on\|off`, `--gpu-device N`, env `WVT_GPU`. `auto` uses a device with compute capability 7.5 or later and at least 1 GB of free memory. `on` stops at startup, with the reason, if no GPU is usable. | Lets the user compare with the CPU, or avoid a driver problem. |
| Work budget | `kMaxInputSamples` and `kMaxAnalyzeRate` depend on the backend. The CPU keeps 256 M and 20 MS/s. The GPU values come from the benchmark (start at 2 G and 100 MS/s). | This is where the user sees the gain: wider views at 100 %, and full-rate live analysis. |
| Logging (#14) | Component `gpu`. The `gpuInit` result is INFO: device, compute capability, memory, CUDA and driver versions, or why the GPU is not used. Each fallback is WARN, one time for each path. Request times are DEBUG, through the existing `Worker` "job took" line plus the GPU kernel time. | Follows the logging rules in `docs/design/verbose-logging-plan.md`. |
| Match tolerance | Waterfall and spectrum: 0.05 dB on bins more than 20 dB above the noise floor. Channel filter I/Q: 1e-4 relative. | cuFFT and the radix-2 FFT round differently. Both are float. |

## 4. Interface sketch (`src/gpu.h`)

```cpp
#pragma once
// Optional NVIDIA GPU acceleration. In a build without CUDA every call reports
// "not available" and the caller uses its CPU code.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "iq_file.h"

enum class GpuMode : int { Auto = 0, On, Off };

struct GpuInfo {
    bool available = false;
    std::string name;    // "NVIDIA GeForce RTX 4070 (compute 8.9, 12.0 GB)"
    std::string reason;  // why not available: "built without CUDA", "no NVIDIA driver", ...
};

bool gpuInit(GpuMode mode, int device, std::string& err);  // once, from main()
const GpuInfo& gpuInfo();
bool gpuEnabled();            // available and not switched off
void gpuSetEnabled(bool on);  // UI switch

// File spectrogram: `rows` rows of `avg` FFTs of size n. FFT a of row r starts at
// sample llround(s0 + r*hop + (a+0.5)*hop/avg) - n/2 (zero outside the file).
// Writes rows*cols linear max-pooled powers (scaled by norm/avg) to lin.
struct GpuSpecReq {
    const IqFile* file; FormatParams fp;
    double s0, hop; int rows, avg, n, cols; const float* win; float norm;
};
bool gpuSpectrogram(const GpuSpecReq& r, float* lin, const std::function<bool()>& cancelled);

// Live spectrum: one per LiveEngine thread; owns its stream, buffers and plan.
class GpuLiveSpectrum {
public:
    static std::unique_ptr<GpuLiveSpectrum> create(int n, SampleFormat fmt);
    virtual ~GpuLiveSpectrum() = default;
    // raw: frames*n packed samples. pw: n fftshifted powers, summed over frames.
    virtual bool run(const uint8_t* raw, int frames, const float* win, float* pw) = 0;
};
```

## 5. Implementation steps

Each step builds and passes the tests on macOS (CPU only) before the next one. Each step
is one commit.

0. **Baseline benchmark (no GPU code).** Add `tools/gpu_check.cpp`:
   - It makes test signals with `SigGen` (tones, chirps, short pulses, noise) in each
     `SampleFormat`, and writes temp files.
   - It runs `computeWaterfall`, the live row math and `computeDemod`, and prints the times:
     - waterfall at N = 1024 / 8192 / 65536 and 1× / 10× / 100× zoom-out;
     - live rows at 20 / 40 / 61.44 MS/s;
     - channel filter with 255 / 2047 / 8191 taps.
   - It also measures the color lookup + texture upload time, to settle the rendering
     question.

   Record the CPU numbers on the Linux computer that has the GPU.
1. **Build switch and stub.**
   - Add `WVT_WITH_CUDA` to `CMakeLists.txt`, with the CUDA block from §3 and the default
     `CMAKE_CUDA_ARCHITECTURES` (§8).
   - Add `gpu.h`, `gpu_stub.cpp` and `gpu_common.cpp`, and a `wvt_gpu` library that both
     `wvt` and `gpu_check` link.
   - Status line: `wvt: CUDA support enabled (CUDA 12.x, archs ..., cuFFT)`, or the reason
     it is disabled.
2. **Run-time setup.**
   - `gpuInit` in `gpu_cuda.cu`. It treats `cudaErrorNoDevice` and
     `cudaErrorInsufficientDriver` as "not available", and checks compute capability and
     free memory.
   - One `CUDA_TRY` macro that logs the first error (file and line) and returns `false`.
   - `--gpu`, `--gpu-device` and `WVT_GPU` in `main.cpp`, with usage text.
   - `--list-devices` also lists the GPUs.
3. **CI.**
   - Add `.github/workflows/build.yml` with two jobs:
     - CPU-only Linux build that runs the self-tests;
     - build in an `nvidia/cuda:12.x-devel-ubuntu24.04` container with
       `-DWVT_WITH_CUDA=ON`, compile only.
   - Without this step, nvcc errors show up only on the GPU computer. Free GitHub runners
     have no GPU, so `gpu_check` cannot run there.
4. **File waterfall on the GPU.**
   - Split `computeWaterfall` into a geometry part and an inner loop. Call `gpuSpectrogram`
     when `gpuEnabled()`; otherwise run the existing `parallelFor` loop.
   - GPU side:
     - At 100 % coverage, copy the contiguous raw span in chunks of rows. Otherwise, gather
       only the FFT segments into the pinned buffer.
     - Kernel `decodeWindow`: same rules as `decodeSamples` (`swapIQ`, `bigEndian`,
       scale), times the window, zero outside the file.
     - Batched `cufftExecC2C`.
     - Kernel `powerPool`: \|X\|² summed over `avg` with fftshift, scaled, max-pooled to
       `cols`.
     - Two staging slots, so the copy of chunk k+1 overlaps the work on chunk k.
   - Raise the input budget on the GPU. `gpu_check` compares GPU and CPU results.
5. **Live spectrum on the GPU.**
   - In `spectrumLoop`, make a `GpuLiveSpectrum`. Make it again when N or the format
     changes.
   - Send the `raw` slice that the loop already reads. Get back `n` powers. Column pooling
     stays in C++.
   - Make `kMaxAnalyzeRate` depend on the backend. Update the "up to 20 MS/s" tooltip in
     `src/app_live.cpp`.
   - Target: less than 2 ms of GPU time per row at 40 MS/s and 30 rows/s.
6. **UI.**
   - Status bar tag `GPU <name>` or `CPU`. A tooltip with `GpuInfo` and the fallback notes.
   - A `GPU` checkbox in the view settings. It calls `gpuSetEnabled` and invalidates the
     workers, so the view computes again.
   - The live status bar shows `(GPU)` after "FFT sees".
7. **Channel filter on the GPU (optional, after step 5 is measured).**
   - Move the NCO + FIR + decimate of dense `processRange` to the GPU. Keep the NCO phase
     from the absolute sample index in double precision.
   - Use a direct polyphase kernel for a small number of taps, and overlap-save FFT
     convolution for many taps.
   - The atan2 / unwrap / smoothing stays on the CPU.
   - Do this step only if step 0 shows that the channel filter is slow. It can also go to a
     new issue.
8. **README.**
   - Add the section "Optional: GPU acceleration": requirements, `WVT_WITH_CUDA`,
     `CMAKE_CUDA_ARCHITECTURES`, `--gpu`, and how to check with `--list-devices`.
   - Add the new files to the file table.

## 6. Testing

| Where | What |
|---|---|
| macOS (each step) | Build with no warnings. `siggen_check`, `audio_check`, `log_check` and `gpu_check` (CPU only) pass. Output and log with no `--gpu` flag match `main`, except the new INFO line. `--gpu on` stops with the reason. |
| CI (step 3 on) | The CPU-only job and the CUDA compile job pass. |
| Linux + NVIDIA GPU (steps 4–7, **you must do this**) | Build with `-DWVT_WITH_CUDA=ON`. `gpu_check` passes the tolerances in §3. Record the `gpu_check --bench` times. Run the GUI with a file and with `--device sim`: GPU checkbox, status bar, and a forced error to check the fallback. |
| Linux, CUDA build, no GPU or driver | wvt starts, shows `CPU`, works, and the log gives the reason. |

## 7. Acceptance criteria

1. A build with `WVT_WITH_CUDA=OFF`, and every macOS build, has no change in behavior.
2. A CUDA build on a computer without an NVIDIA driver starts and works on the CPU.
3. `gpu_check` passes on at least one RTX card.
4. File waterfall: at least 5× faster than the CPU at N = 8192 and 100× zoom-out. 100 %
   coverage for a view at least 4× wider than the CPU budget allows.
5. Live: at 40 MS/s with auto averaging, "FFT sees" stays at 100 % at 30 rows/s. The
   spectrum thread causes no dropped samples.
6. A forced CUDA error moves that path to the CPU, logs a WARN, and the view still
   updates.

## 8. Decisions needed

1. **GPU rendering.** Recommendation: leave it out (§2). OpenGL already draws on the GPU,
   and CUDA–GL interop fails over remote X11. Step 0 measures it. Do you agree?
2. **Target hardware.** Desktop GeForce RTX (sm_75 / 86 / 89 / 120), data-center cards
   (sm_80 / 90), or Jetson (aarch64)? This sets `CMAKE_CUDA_ARCHITECTURES` and the
   build time.
3. **Linking.** Static cuFFT gives a binary of about 100–200 MB that needs only the
   driver. Shared cuFFT gives a small binary that needs the CUDA toolkit libraries at run
   time. Recommendation: static, because it runs with only the driver. The CPU build
   does not change.
4. **Minimum CUDA version.** Recommendation: 12.0, with 12.8 or later recommended (needed
   for RTX 50 / sm_120).
5. **CI.** Can step 3 add GitHub Actions to the repository?
6. **Test computer.** Is there a Linux computer with an NVIDIA GPU for steps 4–7, and
   which card?
7. **Scope for closing #3.** Close #3 after steps 0–6 and 8, and move the channel filter
   (step 7) to a new issue?

## 9. Implementation notes

The decisions in §8 are still open. Until they are answered, the code uses the
recommendations: static cuFFT, CUDA 12.0 or later, and desktop RTX + data-center targets.

### Files

| File | Change |
|---|---|
| `tools/gpu_check.cpp` | Step 0: `--bench` CPU times. Step 1–2: checks of the setup rules (`--gpu` parsing, `auto` / `on` / `off`, the switch, the device list). |
| `src/gpu.h` | `GpuMode`, `GpuInfo`, `gpuInit`, `gpuInfo`, `gpuEnabled`, `gpuSetEnabled`, `gpuListDevices`. `gpuSpectrogram` and `GpuLiveSpectrum` come with steps 4 and 5. |
| `src/gpu_backend.h` | New, not in §3: the three functions a backend provides (`gpuBackendBuilt`, `gpuBackendProbe`, `gpuBackendList`). The rules and the log line stay in `gpu_common.cpp`, so both builds share them. |
| `src/gpu_common.cpp` | Option parsing, the init rules, the INFO line, the switch. |
| `src/gpu_stub.cpp` | "built without CUDA". |
| `src/gpu_cuda.cu` | Device discovery: no device, no driver, driver too old, compute below 7.5, less than 1 GB free. `auto` picks the newest architecture, then the most multiprocessors. `CUDA_TRY` is ready for step 4. |
| `src/main.cpp` | `--gpu`, `--gpu-device`, `WVT_GPU`, usage text, the GPU list in `--list-devices`. |
| `CMakeLists.txt` | `WVT_WITH_CUDA`, `WVT_CUDA_STATIC` (default `ON`), the `wvt_gpu` library, `gpu_check`. |

### Differences from the plan

- `gpuInit` runs only on the GUI path. The headless paths (`--audio-out`, `--no-gui`,
  `--gen-out`, `--tx`) do not use the waterfall, so they do not start CUDA, and their
  output is the same as `main`.
- `gpu_check` makes its own test signal (two tones, a chirp, 1 µs pulses, noise) as one
  ci16 file. It does not use `SigGen`. The other formats come with the GPU decode checks in
  step 4.
- `--gpu off` never calls the CUDA driver.
- Default `CMAKE_CUDA_ARCHITECTURES`: `75-real 80-real 86-real 89-real 90` (PTX for newer
  GPUs), plus `120` with CUDA 12.8 or later. A value given on the command line wins.

### CPU baseline (step 0)

Development Mac, 10 cores, `gpu_check --bench` (64 M samples ci16 at 20 MS/s, 256 MB).
Wider views need a larger file (`--samples 1G`); the 64 M file clips them.

| Job | Case | Time |
|---|---|---|
| File waterfall | N 1024, 100× (52 MS, 100 FFTs/row) | 124 ms |
| | N 8192, 10× (42 MS) | 126 ms |
| | N 65536, 1× (34 MS) | 188 ms |
| Live row (N 2048, 30 rows/s, no 20 MS/s cap) | 20 / 40 / 61.44 MS/s | 5.4 / 10.7 / 15.8 ms per row (16 / 32 / 47 % of a core) |
| Channel filter + demod (8 MS in, decimation 8) | 255 / 2047 / 8191 taps | 67 / 192 / 660 ms |
| Waterfall colouring (1024 × 2048) | | 0.8 ms |

The colouring is under 1 ms, so it does not justify GPU rendering (§8, item 1). The
texture upload needs a GL context and is not measured. The channel filter with many taps
is slower than the waterfall, so step 7 is worth doing.

### Verification (macOS)

- The build has no warnings. CMake prints `CUDA support disabled (CUDA is not available on
  macOS)`.
- `gpu_check`, `siggen_check`, `audio_check`, `log_check` and `segment_check` pass.
- `--gpu on` exits with status 1 and "built without CUDA". `--gpu maybe` and
  `--gpu-device x` exit with status 2. A bad `WVT_GPU` gives a WARN and uses `auto`.

### Verification (CUDA container, no GPU)

Docker on the Mac (arm64), images `nvidia/cuda:12.6.3-devel-ubuntu24.04` and
`nvidia/cuda:12.8.1-devel-ubuntu24.04`, `-DWVT_WITH_CUDA=ON`, RTL-SDR and UHD off:

| | CUDA 12.6 | CUDA 12.8 |
|---|---|---|
| CMake line | `CUDA 12.6.85, archs 75-real;80-real;86-real;89-real;90, static cuFFT` | `CUDA 12.8.93, archs ...;90-real;120, static cuFFT` |
| Build warnings | none | none |
| Self-tests | all pass | all pass |
| `gpu_check` | "build: CUDA; GPU: none, no NVIDIA driver", all pass | same |
| `--gpu on` | "no NVIDIA driver", status 1 | same |
| CUDA shared libraries in `ldd wvt` | none | none |

The binary is 5.5 MB: no cuFFT function is called yet, so the static library adds
nothing. It grows in step 4.

**Not done:** running on a real GPU (steps 4–7 need the Linux computer, §8 item 6).

### Step 4: file waterfall on the GPU

| File | Change |
|---|---|
| `src/gpu.h` | `SpecJob` (the plan's `GpuSpecReq`, shared by the CPU and the GPU), `specFftStart`, `GpuStatus` (`Ok`, `Cancelled`, `Unavailable`, `Failed`), `gpuSpectrogram`, `gpuSpectrogramUsable`. |
| `src/gpu_common.cpp` | The fallback rules: after a failure other than "out of memory", the waterfall stays on the CPU until the next `gpuInit`; one WARN. Out of memory: WARN once, the path stays on. `WVT_GPU_FORCE_FAIL=1` forces a failure. |
| `src/gpu_cuda.cu` | `gpuBackendSpectrogram`, below. `CUDA_TRY` / `CUFFT_TRY` now return `Failed` with the message instead of logging. |
| `src/engine.h`, `src/engine.cpp` | `computeWaterfall` keeps the geometry and tries the GPU first (budget 2 G samples); on any result other than `Ok` or `Cancelled` it computes `avg` again with the CPU budget and runs `spectrogramCpu`, the old loop moved out unchanged. `WaterfallRes::gpu` says which one ran. |
| `src/iq_file.h` | `IqFile::data()`: the mapped bytes, for the upload. |
| `CMakeLists.txt` | CUDA C++20 (the headers use defaulted `operator==`). Static cuFFT needs `CUDA_SEPARABLE_COMPILATION` and `CUDA_RESOLVE_DEVICE_SYMBOLS` on `wvt_gpu` (device link). `segment_check` links `wvt_gpu`. |
| `tools/gpu_check.cpp` | GPU against CPU in all 6 formats, big-endian, swapped I/Q, a 3-byte header, FFTs before and after the file, rows longer than one chunk, `computeWaterfall`, cancel, and the forced failure. `--bench` adds GPU columns. |

How the GPU path works:

- The work goes in chunks of up to 8 M samples (`kChunkSamples`). A chunk holds whole
  rows, or, when one row needs more, part of one row; the row sums go on in a device
  buffer.
- The CPU threads copy the raw bytes of each FFT from the mapped file into one of two
  pinned buffers. The copy of chunk k+1 runs while the GPU works on chunk k (a CUDA event
  per buffer). The plan's "contiguous span at 100 % coverage" shortcut is not done: each
  FFT is copied on its own, so 50 % overlap copies each sample twice. Measure first.
- Kernels: `decodeWindowKernel` (the `decodeSamples` rules, the window, zero outside the
  file), batched in-place `cufftExecC2C`, `accumulateKernel` (|X|² with fftshift, summed
  per row, no atomics), `poolKernel` (scale and max over the bins of each column).
- Each thread has its own stream, buffers and up to 4 cached cuFFT plans. The batch is a
  power of two, so few plans are made. The buffers stay allocated between calls.
- Cancel is checked before each chunk.

Checks:

- macOS: the CPU waterfall is **bit-identical** to before the change (checksums of 24
  waterfalls: 6 formats × 4 views, compared with a build from before the split).
  All self-tests pass, with no warnings.
- CUDA 12.8 container, static cuFFT: builds with no warnings, the self-tests pass, and
  `gpu_check` skips the comparison ("no NVIDIA driver").
- CUDA 12.6 container, `-DWVT_CUDA_STATIC=OFF`: the same; `wvt` loads `libcudart.so.12`
  and `libcufft.so.11`.
- Binary size: **272 MB** with static cuFFT and the 6 default GPU targets, 4.7 MB with
  shared cuFFT. This is more than the 100–200 MB in §8 item 3. Fewer targets
  (`-DCMAKE_CUDA_ARCHITECTURES=89`) make it smaller.

**Still to do on the GPU computer:** `gpu_check` (the comparisons above must pass at
0.05 dB) and `gpu_check --bench --samples 1G`. Then set the GPU budget (2 G samples now)
from the times. The budget is wall time, not only FFT work: a wide view reads up to 8× more
of the file than on the CPU.

### Step 5: live spectrum on the GPU

| File | Change |
|---|---|
| `src/gpu.h` | `GpuLiveSpectrum` (a class with a hidden backend object, not the virtual `create` of §4, so the fallback rules stay in `gpu_common.cpp`) and `gpuLiveUsable`. `run` takes the format, N and the frame count on each call, so the loop does not make a new object when N or the format changes. |
| `src/gpu_common.cpp` | The fallback rules are now per path (`PathState`): a live failure turns off only the live path, and a waterfall failure only the waterfall. `gpuInit` resets both. |
| `src/gpu_cuda.cu` | `gpuBackendLiveRun`: copy the raw row up, then `decodeWindowKernel` (no "inside the file" table), batched cuFFT, and `accumulateKernel` as one row of all the frames. Rows over 8 M samples go in chunks. The plan cache is shared (`PlanCache`). |
| `src/live.cpp` | `spectrumLoop` tries the GPU first. Auto averaging analyzes up to 100 MS/s on the GPU (`kGpuMaxAnalyzeRate`) and 20 MS/s on the CPU. The column pooling and the "FFT sees" figure are unchanged. On a GPU failure the CPU does that row; the next rows use the CPU cap. |
| `src/live.h` | `Stats::gpu`: the last row came from the GPU (for the status bar in step 6). |
| `src/app_live.cpp` | The Avg tooltip: "up to 20 MS/s, or 100 MS/s on the GPU". |
| `tools/gpu_check.cpp` | GPU against CPU for cu8, ci16 at 20 and 61.44 MS/s, one frame, big-endian cf32 with swapped I/Q, and N 65536 over two chunks. A forced live failure turns off only the live path. `--bench` adds the GPU time per row. |

The row is copied from ordinary (pageable) memory: at 100 MS/s and 30 rows/s that is
13 MB per row for ci16. If the benchmark shows the copy matters, read the ring straight into
a pinned buffer.

Checks: macOS (no warnings, all self-tests pass, `--device sim` records with the live
loop running) and the CUDA 12.8 container (no warnings, self-tests pass, comparisons
skipped: "no NVIDIA driver").

**Still to do on the GPU computer:** `gpu_check` and `gpu_check --bench`; then the GUI with
a USRP at 40 MS/s or more (acceptance §7 item 5: "FFT sees" stays at 100 % at 30 rows/s,
no dropped samples).

### Step 6: UI

| File | Change |
|---|---|
| `src/app.cpp` | `gpuTag`: the status bar shows `GPU <model>` or `CPU` before the renderer, by where the shown waterfall was computed (`WaterfallRes::gpu`). Its tooltip has the device, the CUDA and driver versions, "switched off", and the fallback notes. `drawGpuCheckbox`: **GPU** after **Peak**; it calls `gpuSetEnabled` and invalidates the waterfall worker, so the view is computed again. `drawStatusRight` draws the right side of both status bars. |
| `src/app_live.cpp` | The live status bar: `FFT sees 100% (GPU)` and the same tag (from `Stats::gpu`). The GPU checkbox after **Cols**; the live loop checks it on each row. |
| `src/gpu.h`, `src/gpu_common.cpp`, `src/gpu_cuda.cu` | `GpuInfo::model` (the name without "NVIDIA "), `gpuFallbackNotes()` (one line per path that fell back, with the error). |

Differences from the plan:

- **Builds without CUDA show nothing new**: no tag, no checkbox. This keeps the
  "macOS builds do not change" rule (§1). A CUDA build always shows the tag: `CPU` when no
  GPU can be used, with the reason in the tooltip. The checkbox appears only when a GPU can
  be used.
- The tag uses the short model name ("GPU GeForce RTX 4070"); the full name is in the tooltip.

Checks:

- macOS: no warnings, the self-tests pass. The X display could not be opened from this
  session, so the Mac GUI was not looked at; by the code, a build without CUDA draws the
  same status bar as before.
- CUDA 12.8 container with Xvfb and `--software`, no driver: the screenshots show
  `CPU | software | 39 fps` (file) and `FFT sees 99% | CPU | software | 56 fps` (live
  `--device sim`). No GPU checkbox, as expected.

**Still to do on the GPU computer:** the tag shows `GPU <model>`; turning the checkbox off
and on computes the view again on the other side (the log has "GPU processing off / on");
the live status shows `(GPU)`; with `WVT_GPU_FORCE_FAIL=1` the tag shows `CPU` and its
tooltip has the fallback note.

### Step 8: README

"Optional: GPU acceleration" after "Optional: device support" (requirements, the CMake
options, `--gpu`, the checkbox, the fallback, `gpu_check`). The options list, the live
*Avg* text (100 MS/s on the GPU), the waterfall budget (2 G on the GPU), the status bar row
and the source table are updated.

### Step 7: channel filter on the GPU

Done after step 6, without a GPU measurement first: the CPU baseline already shows the
filter as the slowest job (660 ms for 8191 taps on 8 MS).

| File | Change |
|---|---|
| `src/gpu.h` | `ChanJob` (mix, FIR, decimate; shared by the CPU and the GPU), `gpuChannelize`, `gpuChannelUsable`. |
| `src/gpu_common.cpp` | A third fallback path, "channel filter". |
| `src/gpu_cuda.cu` | `decodeOne` (the decode rules, now shared by all kernels), `decodeMixKernel` (decode, zero outside the file, mix with the phase from the absolute index: `frac * index` in double, reduced to [0, 1), then `sincospif` in float), `firKernel` (direct FIR, only the kept outputs, taps in shared memory up to 12000), `gpuBackendChannelize` (chunks of up to 8 M input samples; the raw bytes go up straight from the mapping). |
| `src/engine.h`, `src/engine.cpp` | `channelizeCpu`: the old mix + FIR code, moved unchanged. `processRange` tries the GPU in dense mode when the job has at least 2·10⁷ multiply-adds (`kGpuMinChanMacs`, a few ms of CPU work); sparse mode (short blocks per pixel) stays on the CPU. The atan2 / unwrap / smoothing stays on the CPU. |
| `tools/gpu_check.cpp` | GPU against CPU (1e-4 of the peak): 255 to 13001 taps, decimation 1 to 10, no mix, before and after the file, 3 chunks, cu8 / ci16 / cf32; cancel; a forced failure turns off only this path. `--bench` adds the GPU column. |

Differences from the plan: no overlap-save FFT convolution for many taps. The direct
kernel does N multiply-adds per kept output with the taps in shared memory; the benchmark
on the GPU will show if FFT convolution is needed.

Checks: the CPU demodulator is **bit-identical** to before (checksums of 18 `computeDemod`
results: cf32 / ci16 / cu8, dense and sparse, decimation 1, 8191 taps, mix on and off,
views before and past the file). macOS and the CUDA 12.8 container: no warnings, all
self-tests pass, the GPU comparisons are skipped there.

**Still to do on the GPU computer:** `gpu_check` and the channel filter rows of
`gpu_check --bench`.

### Step 3: CI

`.github/workflows/build.yml`, on pushes to `main` and on pull requests, or by hand (a
newer push to the same branch cancels the running one). Pushes to other branches build only
through their pull request, so each commit builds once. `actions/checkout@v5` (Node.js 24).

| Job | What it does |
|---|---|
| Linux (CPU, RTL-SDR, UHD) | Ubuntu 24.04 with `librtlsdr-dev` and `libuhd-dev`, `WVT_WITH_CUDA=OFF`. Builds, runs the 5 self-tests, `--list-devices`, and a headless WAV export with `--segment`. |
| Linux CUDA 12.0 (compile only) | `nvidia/cuda:12.0.1-devel-ubuntu22.04` (CMake 3.22): the minimum CUDA version. |
| Linux CUDA 12.8 (compile only) | `nvidia/cuda:12.8.1-devel-ubuntu24.04`: adds sm_120. |

The CUDA jobs build with `-DWVT_WITH_CUDA=ON` and run the self-tests with no GPU, so they
also check the "CUDA build on a computer without a GPU" rule; `--gpu on` must fail.

Found while adding it: CMake before 3.25 does not know nvcc's C++20 flag, so the CUDA 12.0
build on Ubuntu 22.04 failed to configure. `CMakeLists.txt` now defines
`CMAKE_CUDA20_STANDARD_COMPILE_OPTION` when CMake lacks it.

Checked before pushing: the CPU job's steps in an `ubuntu:24.04` container (RTL-SDR and UHD
on, no warnings, all pass), and both CUDA images (no warnings, all pass). The containers
were arm64; the GitHub runners are x86-64.
