# Plan: CUDA GPU acceleration for wvt

Branch base: `wvt-sh-dev` (commit `72f9d8a`)
Status: **Phases 0–4 implemented** on branch `wvt-gpu-dev`. The CPU-only build is
tested on macOS. The CUDA code is **not yet compiled or run**: it needs a Linux computer with
the CUDA toolkit and an RTX card. See §9.
Target hardware: **desktop NVIDIA GeForce RTX cards** (RTX 20 / 30 / 40 / 50 series) on x86-64
Linux. A large, statically linked binary is acceptable.

## 1. Goal

Use an NVIDIA GPU, when the computer has one, for the heavy DSP in wvt. The result must
look the same as the CPU result. On every computer without a supported GPU (macOS, AMD/Intel
graphics, no driver), wvt must build, start and work exactly as it does now.

What the user gets:

- **File view:** wide zoom levels keep 100 % FFT coverage ("FFT sees 100%") much further out,
  and the waterfall updates faster when the user pans or zooms.
- **Live view:** the spectrum thread analyzes all samples at the full rate of the BB60
  (40 MS/s) and the USRP B2xx (up to 56 MS/s). The CPU path stops at 20 MS/s
  (`kMaxAnalyzeRate` in `src/live.cpp`).
- **Channel filter / demod:** long FIR filters (up to 8191 taps) on dense views run at
  interactive speed.

Out of scope: rendering through CUDA–OpenGL interop (the UI uses ImGui's OpenGL 2 backend
over GLX, often with indirect rendering on XQuartz or remote X11, and the images are small);
OpenCL, Metal, ROCm/HIP backends (the design below leaves room for them); GPU use in the
signal generator's waveform synthesis.

## 2. Where the time goes today

| Computation | Code | Work per request | GPU value |
|---|---|---|---|
| File waterfall + spectrum | `computeWaterfall`, `src/engine.cpp` | Up to 256 M input samples (`kMaxInputSamples`): rows × avg FFTs of size N, then \|X\|², fftshift, average, column max-pool, log10 | **High.** Batched FFT; output is only rows × cols floats |
| Live spectrum rows | `LiveEngine::spectrumLoop`, `src/live.cpp` | Up to 20 MS/s of FFTs (CPU cap), single thread | **High.** Removes the 20 MS/s cap |
| Channelizer + demod | `processRange`, `src/engine.cpp` | NCO mix + direct-form FIR (taps × outputs MACs, up to 32 M inputs), then atan2 / unwrap / smoothing on the decimated output | **Medium–high** for the NCO + FIR; low for the post-processing |
| Playback resampler | `resample`, `src/siggen.cpp` | 64+ tap sinc per output sample, one time per recording load | Low–medium; one-off load |
| Overview strip | `computeOverview` | 4 × 2048-sample probes per bucket | None (I/O bound) |
| Siggen preview spectrum | `src/siggen_out.cpp` | ≤ 2 M samples | None (too small) |

The FFT in `src/dsp.cpp` is a scalar radix-2 FFT. Its call sites are the two "High" rows above.

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| Build switch | `WVT_WITH_CUDA` = `AUTO` / `ON` / `OFF`, default `AUTO` | Same pattern as `WVT_WITH_RTLSDR`, `WVT_WITH_UHD`, `WVT_WITH_BB` |
| CMake | Use `check_language(CUDA)`, then `enable_language(CUDA)` and `find_package(CUDAToolkit)` only inside the CUDA block. Keep `cmake_minimum_required(3.16)` for the project; the CUDA block needs 3.18+ and is skipped with a status line on older CMake | Non-CUDA builds (macOS, most Linux) must not change |
| Toolkit / architectures | CUDA 12.x or later; CUDA ≥ 12.8 recommended. Default `CMAKE_CUDA_ARCHITECTURES` = `75-real;86-real;89-real;120` with CUDA ≥ 12.8, else `75-real;86-real;89` (PTX for sm_89 is JIT-compiled on RTX 50). User can override | One cubin per desktop RTX generation: RTX 20 = sm_75 (Turing), RTX 30 = sm_86 (Ampere), RTX 40 = sm_89 (Ada), RTX 50 = sm_120 (Blackwell, needs CUDA 12.8). Data-center archs (sm_80, sm_90) are left out to keep build time and size down |
| Linking | Static only: `CUDA::cudart_static` and `CUDA::cufft_static` (plus `CUDA::culibos`). No shared-library option | Decided: a large binary (expect 100–200 MB, mostly cuFFT kernels) is acceptable. The binary needs only the NVIDIA driver (`libcuda.so`) at run time, not the toolkit. `cudart_static` loads the driver lazily, so the binary starts on a computer with no driver and reports "no GPU" |
| FFT library | cuFFT, C2C single precision, batched plans | Standard; batched plans fit "rows × avg" FFTs |
| Code layout | `src/gpu.h` (plain C++ interface, no CUDA headers), `src/gpu_cuda.cu` (kernels + cuFFT), `src/gpu_stub.cpp` (CPU-only build: `gpuAvailable()` returns false) | Callers stay plain C++; same "driver in its own file" style as `sdr_*.cpp` |
| Runtime choice | `--gpu auto\|on\|off` and env `WVT_GPU`. `auto` uses device 0 when it has compute capability ≥ 7.5 and ≥ 1 GB free memory. `--gpu-device N` picks another device | User can force CPU to compare or to work around a driver problem |
| Fallback | Each GPU entry point returns `false` on any CUDA error. The caller then runs the existing CPU code for that request. After the first error, that path stays on CPU for the session and the status bar shows a note | Never lose a result because of the GPU |
| Data transfer | Copy **raw** samples (cu8 / ci16 / ci8 / cf32 …) to the GPU and decode there, through pinned staging buffers | 2–4× less PCIe traffic than copying decoded cf32 |
| Concurrency | One CUDA context per process. Each worker (waterfall, demod, live) owns its own `cudaStream_t`, device buffers and cuFFT plan cache | cuFFT plans and buffers are not shared across threads; streams let the waterfall and live paths overlap |
| Cancellation | Keep the `CancelFn` contract: process in chunks and check `cancelled()` between chunks | Same "latest request wins" behavior as now (`Worker` in `src/engine.h`) |
| Numerical match | GPU results must match CPU within 0.05 dB on waterfall / spectrum bins above the noise floor, and within 1e-4 relative on demod I/Q | cuFFT and the radix-2 FFT round differently; both are float |

## 4. Interface sketch (`src/gpu.h`)

```cpp
#pragma once
// Optional NVIDIA GPU acceleration. All functions are safe to call in a build
// without CUDA: they report "not available" and the caller uses the CPU path.
#include <string>
#include "engine.h"
#include "live.h"

struct GpuInfo {
    bool available = false;
    std::string name;       // e.g. "NVIDIA GeForce RTX 4070"
    int ccMajor = 0, ccMinor = 0;
    size_t memFree = 0, memTotal = 0;
    std::string reason;     // why not available ("no CUDA driver", "built without CUDA", ...)
};

enum class GpuMode : int { Auto = 0, On, Off };
bool gpuInit(GpuMode mode, int device, std::string& err);  // called once from main()
const GpuInfo& gpuInfo();
bool gpuEnabled();

// Each returns false if the GPU cannot do the request; the caller then runs the CPU code.
bool gpuWaterfall(const WaterfallReq& r, int avg, double hop, WaterfallRes& res, const CancelFn& cancelled);
bool gpuChannelize(const DemodReq& r, const ChanDesign& cd, const std::vector<float>& h,
                   int64_t inStart, int64_t inLen, int64_t M, std::vector<cf>& y, const CancelFn& cancelled);

// Live spectrum: one object per LiveEngine, holds its stream, buffers and plan.
class GpuLiveSpectrum {
public:
    static std::unique_ptr<GpuLiveSpectrum> create(int n, SampleFormat fmt);
    // raw: `frames * n` packed samples. Writes n fftshifted, summed powers to pw.
    virtual bool run(const uint8_t* raw, int frames, const float* win, float* pw) = 0;
    virtual ~GpuLiveSpectrum() = default;
};
```

The engine keeps ownership of geometry (rows, hop, avg, cols, f0/f1, percentiles). The GPU
code only replaces the inner loops, so the CPU and GPU paths cannot drift apart on layout.

## 5. Work breakdown

### Phase 0 — Baseline and test harness
- [x] Add `tools/gpu_check.cpp` (like `tools/siggen_check.cpp`). It makes test signals (tones,
      chirps, pulses, noise; reuse `SigGen`) in each `SampleFormat`, writes temp files, and runs
      `computeWaterfall` / `computeDemod` with `--gpu off` and `--gpu on`. It compares the
      results with the tolerances in §3, and prints timings. Exit code ≠ 0 on a mismatch.
- [x] Add `--bench` to `gpu_check`: waterfall at 1×, 10×, 100× zoom-out for N = 1024 / 8192 /
      65536; demod with 255 / 2047 / 8191 taps; live spectrum at 20 / 40 / 61.44 MS/s.
- [ ] Record CPU numbers on the reference computers before any GPU code (gives the targets in §7).

### Phase 1 — Build system (`CMakeLists.txt`)
- [x] `WVT_WITH_CUDA` cache variable (`AUTO` / `ON` / `OFF`).
- [x] On `APPLE`: skip, status line "CUDA support disabled (not available on macOS)";
      `ON` is a fatal error.
- [x] Else: `include(CheckLanguage)`, `check_language(CUDA)`. If found and CMake ≥ 3.18:
      `enable_language(CUDA)`, `find_package(CUDAToolkit 12 REQUIRED)`, set default
      `CMAKE_CUDA_ARCHITECTURES` when the user did not set it (choose the list from
      `CUDAToolkit_VERSION`, see §3). On a non-x86-64 host, `AUTO` skips CUDA with a status
      line; `ON` is a fatal error (target hardware is desktop x86-64 only).
- [x] Add `src/gpu_cuda.cu` to `wvt` and `gpu_check`, link `CUDA::cudart_static CUDA::cufft_static
      CUDA::culibos`, define `WVT_HAVE_CUDA`. Otherwise add `src/gpu_stub.cpp`.
- [x] `CMAKE_CUDA_STANDARD 20`. `gpu.h` includes `iq_file.h`, which uses C++20 defaulted
      comparisons. CUDA 12 supports C++20 with GCC 10 or later.
- [x] Status line: `wvt: CUDA support enabled (CUDA 12.x, archs 75;86;89;120, static cuFFT)`.
- [ ] Check that a CPU-only build and its binary are byte-for-byte unchanged in behavior
      (`gpu_stub.cpp` only).

### Phase 2 — Runtime and device management (`src/gpu_cuda.cu`, `src/main.cpp`)
- [x] `gpuInit`: `cudaGetDeviceCount`; handle `cudaErrorNoDevice` and
      `cudaErrorInsufficientDriver` as "not available" (fill `GpuInfo::reason`); check compute
      capability and `cudaMemGetInfo`.
- [x] One `CUDA_TRY(expr)` macro that logs the first error with file/line and returns `false`.
- [x] Pinned host staging buffer pool (`cudaHostAlloc`) and device buffer pool, sized from free
      memory (default: at most 25 % of free VRAM, at least 64 MB chunks).
- [x] cuFFT plan cache keyed by (n, batch), one cache per stream owner.
- [x] CLI: `--gpu auto|on|off`, `--gpu-device N`, env `WVT_GPU`. `--gpu on` with no usable GPU
      is a startup error with the reason. `--list-devices` also prints GPUs.
- [x] UI: status-bar tag `GPU: <name>` or `CPU`, tooltip with `GpuInfo`, and a note when a path
      fell back after an error. A checkbox in the view settings to switch GPU use on or off at
      run time (invalidates the workers so the view recomputes).

### Phase 3 — File waterfall on the GPU (largest gain)
- [x] Split `computeWaterfall` so the geometry (rows, hop, avg, cols, f0/f1) is computed first,
      then calls `gpuWaterfall`, else the existing `parallelFor` loop.
- [x] Input: for each chunk of rows, find the sample span it covers. When coverage = 1, copy
      the contiguous raw span (it is a memory-mapped file; copy through a pinned buffer).
      When coverage < 1, gather only the FFT segments on the host into the pinned buffer.
      Double-buffer: copy chunk k+1 while chunk k computes (2 streams or 2 events).
- [x] Kernel `decodeWindow`: raw bytes → cf32 with the `FormatParams` rules (`swapIQ`,
      `bigEndian`, scale) from `decodeSamples` in `src/iq_file.cpp`, × window, written into the
      batched FFT layout (one row of `n` per FFT, start offsets from `llround(center) - n/2`).
      Zero-fill outside the file, as `IqFile::read` does.
- [x] `cufftExecC2C` forward, batch = FFTs in the chunk.
- [x] Kernel `powerReduce`: per row, sum \|X\|² over its `avg` FFTs with fftshift, scale by
      `norm / avg`, max-pool `binsPerCol` bins, write `lin` and `db` (rows × cols). Only this
      comes back to the host.
- [x] Spectrum average / peak and the percentiles stay on the CPU (they use only rows × cols).
- [x] Make the work budget per backend: `kMaxInputSamples` = 256 M on CPU, larger on GPU
      (start at 2 G, tune with the benchmark). The "FFT sees N%" readout then shows the gain.
- [x] Check `cancelled()` between chunks; on cancel, return `nullptr` as now.

### Phase 4 — Live spectrum on the GPU
- [x] In `spectrumLoop`, create a `GpuLiveSpectrum` when GPU is enabled; recreate on FFT size
      or format change (the loop already tracks `winFor` / `winType`).
- [x] Copy the raw ring slice (already read into `raw`) to the GPU, decode + window + batched
      FFT + summed fftshifted power, copy back `n` floats. Column pooling stays on the CPU.
- [x] Make `kMaxAnalyzeRate` depend on the backend (CPU 20 MS/s, GPU 100 MS/s) so "auto"
      averaging analyzes all samples at BB60 / USRP rates. Update the tooltip in
      `src/app_live.cpp` (it says "up to 20 MS/s").
- [x] Keep latency low: reuse buffers and the plan; one `cudaStreamSynchronize` per row.
      Target < 2 ms GPU time per row at 40 MS/s / 30 rows/s.

### Phase 5 — Channelizer on the GPU
- [ ] `gpuChannelize` replaces the NCO and FIR stages in `processRange` (dense mode only; the
      sparse mode works on many short blocks and stays on the CPU at first).
- [ ] NCO: phase from the **absolute** sample index in double precision per block (same as the
      CPU code, so results do not shift when the view pans).
- [ ] FIR + decimate: for taps ≤ ~512, a direct polyphase kernel with the taps in shared /
      constant memory, computing only the kept outputs. For more taps, overlap-save fast
      convolution with cuFFT, then decimate. Choose the method from a benchmark crossover.
- [ ] Return the decimated `y`; atan2, unwrap, smoothing, squelch and stats stay on the CPU
      (they work on the decimated output and are cheap).
- [ ] Later, if the benchmark shows a need: move the post-processing too (unwrap = prefix sum,
      `cub::DeviceScan`).

### Phase 6 — Optional: playback resampler
- [ ] Only if Phase 0 shows recording loads are slow: port `resample` in `src/siggen.cpp`
      (table-interpolated sinc, one output per thread). Low priority.

### Phase 7 — Docs and CI
- [x] `README.md`: "Optional: GPU acceleration" section (requirements, `WVT_WITH_CUDA`,
      `CMAKE_CUDA_ARCHITECTURES`, `--gpu`, how to check with `--list-devices`), new source files
      in the file table, roadmap row. Write it in Simplified Technical English as the rest of the
      README.
- [ ] CI: a Linux job that builds in an `nvidia/cuda:12.x-devel` container (build only, no GPU)
      and a CPU-only job, so a CUDA change cannot break non-CUDA builds. Run `gpu_check` on
      a GPU runner when one is available.

## 6. Risks and how to handle them

| Risk | Handling |
|---|---|
| PCIe bandwidth limits the waterfall, not the FFT | Copy raw (narrow) samples; overlap copy and compute; measure in Phase 0 / 3 |
| Memory-mapped file pages are not resident; GPU waits on disk | Same as CPU today; the pinned copy reads the pages. Optional `madvise(MADV_SEQUENTIAL)` on the span |
| VRAM is low or shared with the display | Size pools from `cudaMemGetInfo`; on `cudaErrorMemoryAllocation`, halve the chunk and retry, then fall back to CPU |
| Driver older than the toolkit | `cudaErrorInsufficientDriver` → "not available" with a clear reason. README states the minimum driver for the chosen toolkit (CUDA 12.8 needs driver ≥ 570 on Linux) |
| RTX 50 with a toolkit older than 12.8 | PTX for sm_89 is JIT-compiled on first run (a few seconds, then cached in `~/.nv/ComputeCache`). Status line recommends CUDA ≥ 12.8 |
| GPU and CPU results differ visibly | Tolerances checked by `gpu_check`; same window, same norm, same fftshift and pooling rules |
| Static cuFFT makes the binary large (100–200 MB) and the link slow | Accepted (decided). Only `wvt` and `gpu_check` link cuFFT; `iqgen` and `siggen_check` stay small |
| Desktop RTX cards have less VRAM (8–16 GB typical, 6 GB on RTX 20 low end) and also drive the display | Default pool ≤ 25 % of free VRAM; the chunked design never needs the whole view in VRAM |
| Consumer cards have slow FP64 (1/64 rate) | Keep kernels in FP32. Use double only for per-block NCO start phase (one value per block, as the CPU code does) |
| Several workers share one GPU | Separate streams; the live path has priority (`cudaStreamCreateWithPriority`) |

## 7. Acceptance criteria

1. A build with `WVT_WITH_CUDA=OFF`, and every macOS build, has no change in behavior.
2. A CUDA build on a computer without an NVIDIA driver starts, shows `CPU`, and works.
3. `gpu_check` passes on desktop RTX cards from at least two generations (for example an
   RTX 3060 and an RTX 4070), and on an RTX 50 card when one is available.
4. File waterfall: on a mid-range RTX card (RTX 3060 / 4060 class, PCIe 4 x16), ≥ 5× faster
   than CPU (8-thread reference) at N = 8192, 100× zoom-out,
   and 100 % coverage at a 4× wider view than the CPU budget allows.
5. Live: at 40 MS/s (BB60) with auto averaging, "FFT sees" stays at 100 %, at 30 rows/s, with
   no dropped samples caused by the spectrum thread.
6. Demod: 8191-tap channel filter on a 32 M-sample dense view finishes in < 200 ms.
7. Forcing a CUDA error (test hook) makes that path fall back to CPU with a note, and the
   view still updates.

## 8. Open questions

Decided (2026-10-18): target hardware is desktop GeForce RTX cards; a large statically
linked binary is acceptable.

- Should the GPU on/off setting persist between sessions (wvt has no settings file today)?
- Is an alternative backend (Vulkan compute, or HIP for AMD) wanted later? If so, keep
  `gpu.h` free of CUDA names (it is in the sketch above).

## 9. Implementation notes (branch `wvt-gpu-dev`)

### Files

| File | Content |
|---|---|
| `src/gpu.h` | Interface: `gpuInit`, `gpuEnabled`, `gpuSpectrogram`, `GpuLiveSpectrum` |
| `src/gpu_cuda.cu` | CUDA build: device setup, kernels, cuFFT plan cache, file spectrogram, live spectrum |
| `src/gpu_stub.cpp` | Build without CUDA: no GPU is ever available |
| `src/gpu_common.cpp` | Code for both builds: `--gpu` parsing, status-bar text |
| `tools/gpu_check.cpp` | Compares GPU and CPU results. With `--bench`, it also measures speed |

### Differences from the plan

- The file spectrogram uses three kernels, not two. `kDecodeWindow` decodes and windows the
  samples. `kAccumulate` adds the power of each row's FFTs. `kFinalize` scales, pools and
  converts to dB.
- Each thread that uses the GPU has its own buffers, which grow when a request needs more.
  There is no shared pool.
- Copies use two staging slots on one stream. The CPU fills one slot while the GPU processes
  the other batch. The H2D copy does not yet overlap the FFT of the previous batch.
- A batch of FFTs is padded to a multiple of 64, so few cuFFT plans are necessary. The extra
  FFTs get zero input and add nothing to the result.
- An out-of-memory error does not turn off the file waterfall path. The next request can be
  smaller. Every other CUDA error turns off that path for the session.
- `WaterfallReq::maxInput` overrides the input budget. `gpu_check` uses it to test the
  gathered-FFT path, because a small test file cannot reach the GPU budget.
- The status bar shows `GPU <name>`, `GPU off`, or nothing (no GPU). A tooltip shows the
  fallback notes. The file view toolbar has a `GPU` checkbox.

### Not done yet

- Compile `src/gpu_cuda.cu` with nvcc, then run `gpu_check` and `gpu_check --bench` on an
  RTX card. Fix what fails.
- Phase 0: record CPU numbers on the Linux reference computer. The CPU numbers on macOS are
  not a reference for RTX speedups.
- `--bench` uses a 64 M-sample file, which is below the CPU budget (256 M). Add a case
  above 256 M samples to show the coverage gain.
- Phase 1: check that the CPU-only build has no change in behavior. `siggen_check` passes,
  but the GUI is not checked (no X display in this session).
- Phases 5 (channelizer), 6 (resampler) and the CI part of Phase 7.


## 10. Completion notes (2026-10-18)

Branch `wvt-gpu-dev`, from `wvt-sh-dev` (commit `72f9d8a`).

### Completed

- **Phase 0.** `tools/gpu_check.cpp` compares GPU and CPU results. It tests all 6 sample
  formats, big-endian ci16 and cf32, and swapped I/Q with a file header. It also tests zero
  fill at the start and the end of the file, and the gathered-FFT path. With `--bench`, it
  measures the file waterfall (N = 1024, 8192 and 65536) and the live row (20, 40 and
  61.44 MS/s).
- **Phase 1.** `WVT_WITH_CUDA` (`AUTO`, `ON`, `OFF`) is in `CMakeLists.txt`. The library
  `wvt_gpu` contains `gpu_cuda.cu` or `gpu_stub.cpp`. `wvt` and `gpu_check` link it.
- **Phase 2.** Device detection needs compute 7.5 or later and 1 GB of free memory. Batch
  size comes from the free memory. Each thread has its own buffers and cuFFT plan cache. The
  cached plans share one work area. The CLI has `--gpu`, `--gpu-device` and `WVT_GPU`.
  `--list-devices` shows the GPUs. The UI has a `GPU` checkbox, a status-bar tag and a
  tooltip with the fallback notes.
- **Phase 3.** `computeWaterfall` calls `gpuSpectrogram`. If the GPU fails, the CPU computes
  the request again with the CPU budget. The GPU budget is 2 G input samples. The CPU budget
  stays at 256 M.
- **Phase 4.** `spectrumLoop` uses `GpuLiveSpectrum`. The auto-averaging limit is 100 MS/s
  on the GPU and 20 MS/s on the CPU. The live status bar shows `(GPU)` after "FFT sees".
- **Phase 7 (README only).** The README has the section "Optional: GPU acceleration", the
  new CLI options and the new source files.

### Verification on macOS (CPU-only build)

- The build has no warnings. CMake shows `CUDA support disabled (not available on macOS)`.
- `gpu_check` and `gpu_check --bench` exit with status 0. They run the CPU timings only.
- `siggen_check` passes.
- `--gpu on` stops with exit status 1 and gives the reason. The program rejects incorrect
  `--gpu` and `WVT_GPU` values.

CPU reference times on the macOS development computer (not the Linux reference):

| Test | CPU time |
|---|---|
| File waterfall, N = 1024, 64 M samples | 168 ms |
| File waterfall, N = 8192, 64 M samples | 198 ms |
| File waterfall, N = 65536, 64 M samples | 386 ms |
| Live row, N = 2048, 40 MS/s (651 FFTs) | 11.0 ms |
| Live row, N = 2048, 61.44 MS/s (1000 FFTs) | 16.3 ms |

### Not verified

- `src/gpu_cuda.cu` is not compiled. The macOS computer has no CUDA toolkit. Expect some
  compile errors on the first Linux build.
- No test has compared a GPU result with the CPU result yet.
- The UI changes are not checked on screen. This session had no X display.

### Next steps

1. On Linux with an RTX card, run
   `cmake -S . -B build -DWVT_WITH_CUDA=ON && cmake --build build -j`. Fix the compile errors.
2. Run `build/gpu_check`. All checks must pass with a difference of 0.05 dB or less.
3. Run `build/gpu_check --bench`. Record the CPU and GPU times on the Linux reference
   computer. Compare them with the targets in §7.
4. Add a benchmark case with more than 256 M samples to show the coverage gain.
5. Open wvt with a file and with a live device. Check the `GPU` checkbox and the status bar.
6. Start Phase 5 (channelizer), then the CI part of Phase 7.
