# Plan: Apple GPU support (issue #21)

Branch: `21-apple-gpu`, from `main` at `dea9c20` (after the CUDA work, PR #20, and the
region export, PR #22).
Status: **steps 0–6 implemented** (2026-10-20), PR #25; step 7 (macOS CI) dropped. The user
tested the GUI on screen (2026-10-20). The numbers in §2 come from throwaway prototypes in the session
scratch folder; they are not in the repository.

## 1. Goal

Issue #21: "extend support to the Apple GPU hardware set to allow GPU processing on non-CUDA
devices."

- On a Mac, run the jobs that use the GPU on NVIDIA (#3) on the Apple GPU: the file waterfall,
  the live spectrum, and the channel filter of the dense demodulator and of the region export.
- The same rules as for CUDA:
  - the GPU result looks the same as the CPU result;
  - any GPU error falls back to the CPU;
  - `--gpu auto|on|off`, the **GPU** checkbox and the status bar tag work the same way;
  - `-DWVT_WITH_METAL=OFF` builds what `main` builds now.
- Other non-NVIDIA GPUs (AMD, Intel) are covered in §8, as a direction for later issues.

## 2. Findings on the development Mac

Apple M5, 10-core GPU, Metal 4, 16 GB of unified memory, macOS 26.6.2. Only the Command Line
Tools are installed: **no Xcode, so there is no offline Metal shader compiler** (`xcrun metal`
is missing). The SDK has Metal, Metal Performance Shaders (MPS), MPSGraph and Accelerate.

The test job is the inner loop of the waterfall and the live spectrum: complex samples, window,
FFT, |X|², summed over frames.

### Accelerate (vDSP) on the CPU, against wvt's radix-2 FFT

| Size | wvt FFT, 1 core | vDSP, 1 core | Gain | Max difference (bins 20 dB above the floor) |
|---|---|---|---|---|
| N 2048 × 1000 | 11.1 ms | 2.2 ms | 5.0× | 0.0004 dB |
| N 8192 × 250 | 10.1 ms | 3.1 ms | 3.2× | 0.012 dB |
| N 65536 × 32 | 16.4 ms | 2.8 ms | 5.9× | 0.060 dB |

All 10 cores, 33 M samples:

| N | wvt FFT | vDSP |
|---|---|---|
| 2048 | 1.1 GS/s | 2.7 GS/s |
| 8192 | 1.0 GS/s | 2.1 GS/s |
| 65536 | 0.54 GS/s | 2.2 GS/s |

### MPSGraph FFT on the GPU (the same job, cf32 in shared buffers)

| Size | First run (graph compile) | Then | Rate |
|---|---|---|---|
| N 2048 × 1000 (2 MS) | 11.5 ms | 1.08 ms | 1.9 GS/s |
| N 8192 × 250 (2 MS) | 97 ms | 1.30 ms | 1.6 GS/s |
| N 65536 × 32 (2 MS) | 75 ms | 1.37 ms | 1.5 GS/s |
| N 2048 × 16000 (33 MS) | 15 ms | 4.83 ms | **6.8 GS/s** |
| N 8192 × 4000 (33 MS) | 18 ms | 9.44 ms | **3.5 GS/s** |

Small jobs cost about 1 ms whatever their size. A new shape costs a graph compile of 11–97 ms
the first time.

### Metal compute kernel compiled at run time, reading the file in place

- `newLibraryWithSource` compiles Metal Shading Language source at run time with only the
  Command Line Tools: **184 ms**, once.
- `newBufferWithBytesNoCopy` wraps a **read-only `mmap` of the file**: the GPU reads the file
  pages directly, with no copy.
- A kernel that decodes ci16 and sums |x|² over 256 MB: 28.6 ms the first time (page faults),
  then **6.7–7.3 ms (36–40 GB/s)**. The sum matches the CPU.
- `maxBufferLength` is 9.5 GB, and the recommended working set is 12.7 GB.

### What this means

1. **vDSP alone** makes every FFT on every Mac 2–6× faster, GPU or not, for little work.
2. **The GPU** adds another 1.6–2.5× over vDSP on large jobs: the wide file waterfall and the
   channel filter. The live spectrum (2 MS per row at 61 MS/s) is about 1 ms either way.
3. **Unified memory** removes the copies that the CUDA backend needs: no gather into pinned
   buffers, no upload. The GPU reads the mapped file.
4. **Apple GPUs have no 64-bit floating point.** Two CUDA kernels use `double`:
   - the mixer phase (`frac * index`);
   - the cf64 decode.

   Both need another approach (§3).

## 3. Design decisions

| Topic | Decision | Reason |
|---|---|---|
| API | **Metal**, in Objective-C++ (`src/gpu/gpu_metal.mm`), behind the existing backend interface (`src/gpu/gpu_backend.h`): `gpuBackendProbe`, `gpuBackendSpectrogram`, `gpuBackendLive*`, `gpuBackendChannelize`. | The rest of wvt does not change. Metal is the only supported GPU compute API on macOS (OpenCL is deprecated). |
| Kernels | Metal Shading Language source kept as a C++ string in `gpu_metal.mm`, compiled at run time when the backend first starts (about 0.2 s, on a background thread). | It builds without Xcode. A precompiled `.metallib` would need Xcode on every build machine. |
| FFT | **MPSGraph** `fastFourierTransform` (macOS 14 and later), between our own kernels. Cached graphs per (N, batch), with a power-of-two batch, as the cuFFT plan cache does. | Fast (§2), maintained by Apple, no extra dependency. The alternatives are below. |
| FFT alternatives | (a) Our own Stockham FFT kernel: no graph compile, full control, but more work and tuning. (b) VkFFT with its Metal backend: one more third-party library, but it is also a route to Vulkan, HIP and OpenCL (§8). | Can replace MPSGraph later behind the same code, if the graph compile time matters. |
| File input | `newBufferWithBytesNoCopy` over the page-aligned mapping, one buffer per chunk (offset rounded down to a page, at most 1 GB), so files larger than `maxBufferLength` work. `IqFile` exposes the mapping. | No gather and no copy (§2). |
| Mixer phase | A **64-bit fixed-point phase**: the host computes the increment `inc = round(frac · 2⁶⁴)` and the chunk's start phase `in0 · inc` (unsigned 64-bit, wrapping). The kernel adds `k · inc` (`ulong`) and takes the top 32 bits as the phase in cycles. | Metal has `ulong` but no `double`. The phase stays exact for any file length; the error is far below the float rounding of the samples. The CUDA and CPU mixers can use the same method later. |
| cf64 files | Decoded on the CPU to cf32 for that chunk (or the GPU path reports `Unavailable` for cf64 at first). | No `double` in Metal. cf64 is rare. |
| CPU FFT | On macOS, `FFT::forward` (`src/dsp/dsp.cpp`) uses **vDSP** (`vDSP_fft_zip`, with a split-complex buffer per thread). Other systems keep the radix-2 code. | 2–6× for every FFT on every Mac (§2), with or without the GPU. Separate commit, separate test. |
| Build | `WVT_WITH_METAL` = `AUTO` / `ON` / `OFF`, default `AUTO`, on Apple only. CMake enables `OBJCXX` and links Metal, Foundation, MetalPerformanceShaders and MetalPerformanceShadersGraph. CUDA stays Linux-only, so a build has at most one GPU backend. | Same pattern as `WVT_WITH_CUDA`. No new dependency to install. |
| Run time | `gpuInit` finds the default Metal device. It needs macOS 14 or later (MPSGraph complex FFT); on older macOS the reason is logged and the CPU is used. Intel Macs with AMD or Intel graphics also have Metal; they are not tested. | The same `--gpu` rules and log lines as CUDA. |
| Names | Messages that say "CUDA" become neutral: "built without GPU support", `GpuInfo::backend` = "CUDA" or "Metal", `gpu_check` prints the backend. The status bar shows `GPU Apple M5`. | Two backends now. |
| Budgets | The GPU budgets (2 G samples for the waterfall, 100 MS/s live) apply to Metal too. Set them again from `gpu_check --bench` on this Mac. | The same knobs. |
| Concurrency | One `MTLDevice` and its compiled pipelines, shared. Each user thread has its own command queue, buffers and graph cache (`thread_local`), as with CUDA streams. | Same structure as the CUDA backend. |
| Live spectrum | Uses the GPU only when frames × N is large enough to pay for the about 1 ms per call (measure; else the vDSP CPU path). | §2: small jobs are not faster on the GPU. |

## 4. Interface changes

- `src/gpu/gpu_backend.h`: no new functions. `GpuInfo` gets `backend` ("CUDA" / "Metal").
- `src/io/iq_file.h`: `mappedBase()` and `mappedSize()`. The mapping is already page-aligned
  (`mmap`). `data()` (the raw bytes) stays.
- `src/dsp/dsp.cpp`: `FFT` holds a vDSP setup on Apple. `forward()` keeps its interface (in-place,
  interleaved `cf`), and converts to split complex inside.

## 5. Implementation steps

Each step builds and passes the tests, then gets its own commit.

0. **Benchmark first.** `gpu_check --bench` on this Mac, before any change. These are the CPU
   numbers to beat.
1. **vDSP CPU FFT** (`src/dsp/dsp.cpp`, Apple only).
   - All self-tests must pass.
   - `gpu_check` compares the vDSP FFT with the radix-2 one (strong bins within 0.05 dB) and
     times it.
   - The waterfall checksums change (a different FFT); the 0.05 dB rule is the test now.
2. **Build switch and Metal probe.**
   - `WVT_WITH_METAL` in CMake, and `src/gpu/gpu_metal.mm` with `gpuBackendProbe` and
     `gpuBackendList`.
   - Neutral names (§3), `GpuInfo::backend`.
   - `--list-devices` shows the Metal device.
   - `--gpu on` works, and the status bar shows the tag.
3. **Kernels and the waterfall.**
   - The MSL source: decode with window, accumulate, pool.
   - Zero-copy chunks of the mapping.
   - MPSGraph FFT in the same command buffer.
   - `gpu_check` runs its GPU vs CPU comparisons, which **can run on this Mac**.
4. **Live spectrum.** `GpuLiveSpectrum` on Metal (the ring slice as a shared buffer), with the
   size threshold of §3.
5. **Channel filter.**
   - Mix with the fixed-point phase (§3), and the direct FIR with the taps in threadgroup
     memory.
   - `gpu_check` and `extract_check` compare with the CPU.
   - The region export (#18) uses it with no change.
6. **Budgets and README.**
   - Set the budgets from `gpu_check --bench`.
   - README: "Optional: GPU acceleration" covers Metal; the file table.
7. **CI.**
   - A `macos-14` (Apple Silicon) job: install XQuartz (`brew install --cask xquartz`), build,
     run the self-tests.
   - GitHub's Mac runners are virtual machines. If they have no Metal device, the GPU checks
     are skipped, as on Linux.

## 6. Testing

| Where | What |
|---|---|
| This Mac (M5) | Everything: the build with no warnings, all self-tests, `gpu_check` comparisons on the real GPU (waterfall and live within 0.05 dB, channel filter within 1e-4), `gpu_check --bench`, `WVT_GPU_FORCE_FAIL=1`, the GUI (tag, checkbox, `(GPU)` in live mode, an export with a band). |
| `-DWVT_WITH_METAL=OFF` | The same as `main`, apart from the vDSP FFT (step 1). |
| Linux CI | No change: the CPU and CUDA jobs still pass. |
| macOS CI (step 7) | The build and the self-tests on Apple Silicon. |

## 7. Acceptance criteria

1. On this Mac, `gpu_check` passes on the Metal GPU.
2. The file waterfall at N 8192 over 100× the zoom-1 span: at least 3× faster than the CPU
   (vDSP) path, and "FFT sees 100 %" for views at least 4× wider than the CPU budget allows.
3. The channel filter with 8191 taps: at least 5× faster than the CPU.
4. A forced GPU error falls back to the CPU and logs a WARN.
5. `-DWVT_WITH_METAL=OFF` and Linux builds behave as before.

## 8. Other non-NVIDIA GPUs (AMD, Intel): options for later

The backend interface in `gpu_backend.h` keeps this open. Options, from the least work for
one vendor to the widest reach:

| Route | GPUs | FFT | Notes |
|---|---|---|---|
| **HIP** (ROCm) | AMD on Linux | hipFFT / rocFFT | `hipify` turns `gpu_cuda.cu` into HIP almost line for line. ROCm supports a limited list of AMD cards (mostly RDNA2/3 and Instinct). The cheapest route for AMD. |
| **SYCL / oneAPI** | Intel (also AMD, NVIDIA through plugins) | oneMKL DFT | A large toolchain. Mainly useful for Intel Arc and data-center GPUs. |
| **Vulkan compute** | AMD, Intel, NVIDIA on Linux and Windows; Apple through MoltenVK | **VkFFT** (MIT, header-only) | One backend for all vendors. More code than CUDA (descriptors, SPIR-V shaders at build or run time). VkFFT also has CUDA, HIP, OpenCL, Level Zero and Metal backends. |
| **OpenCL** | Most GPUs | VkFFT (OpenCL backend) or clFFT | Wide reach, but old: deprecated on macOS, and uneven drivers on Linux. |

Recommendation: do the Metal backend for #21 now. For AMD and Intel, open a new issue when
there is a computer to test on. HIP is the shortest route for AMD on Linux, and Vulkan +
VkFFT is the route to cover every vendor with one backend.

## 9. Decisions needed

1. **Scope:** Metal backend plus the vDSP CPU FFT (recommended), or Metal only? vDSP helps
   every Mac and is low risk.
2. **FFT on the GPU:** MPSGraph (recommended: fast, no dependency, needs macOS 14 or later), our
   own Metal FFT kernel (no graph compile, more work), or VkFFT's Metal backend (one more
   library, a route to other vendors)?
3. **Minimum macOS** for the GPU path: 14 (Sonoma), as MPSGraph's complex FFT needs? Older
   macOS uses the CPU.
4. **cf64 on the GPU:** decode on the CPU for that chunk, or no GPU for cf64 files at first?
5. **macOS CI job** (step 7): add it? On a private repository, macOS minutes cost 10× Linux
   minutes.
6. **AMD / Intel:** a separate issue later (recommended), or part of #21?

## 10. Decisions (2026-10-20)

1. **Scope:** the Metal backend and the vDSP CPU FFT.
2. **GPU FFT:** MPSGraph.
3. **Minimum macOS** for the GPU path: 14. Older macOS uses the CPU.
4. **cf64:** decoded on the CPU, chunk by chunk.
5. **macOS CI job:** skipped (step 7 is dropped).
6. **AMD / Intel:** a separate issue, #24.

## 11. Implementation notes

### Change of plan: a hybrid FFT

With MPSGraph for every FFT, the GPU waterfall was only **1.0–1.4×** the CPU per sample on the
M5. The CPU had just become 2–2.4× faster with vDSP (step 1), and the MPSGraph pipeline moves
each sample through memory four times: decode, FFT in, FFT out, sum. A prototype kernel that
decodes, windows, transforms and squares one FFT in threadgroup memory ran at
**4.1–4.5 GS/s** (2.5–3× the CPU), but only up to N 4096 (32 KB of threadgroup memory). The
user chose the hybrid (2026-10-20):

- **N ≤ 4096:** our one-pass kernel `fftPower` (radix-2 in threadgroup memory; the fftshifted
  powers of each FFT go out), then `accumulatePower` sums them per row.
- **N 8192–65536:** `decodeWindow`, the MPSGraph FFT and `accumulate`, in one command buffer.

### Files

| File | Change |
|---|---|
| `src/dsp/dsp.h`, `src/dsp/dsp.cpp` | `FFT` uses vDSP on macOS (a split-complex scratch per thread). |
| `src/gpu/gpu_metal.mm` | The Metal backend: device probe and list, the kernels (MSL source compiled at run time, safe math mode), no-copy buffers over the file mapping (up to 1 GB per chunk), the waterfall (hybrid), the live spectrum, the channel filter (64-bit fixed-point phase, taps in threadgroup memory up to 8192). cf64 is decoded on the CPU. |
| `src/gpu/gpu.h`, `src/gpu/gpu_backend.h`, `src/gpu/gpu_common.cpp` | `GpuInfo::backend`; `gpuBackendName`, `gpuBackendImplements` (the jobs a backend runs), `GpuLimits` / `gpuBackendLimits` (budgets and the job sizes below which the CPU is faster). |
| `src/gpu/gpu_cuda.cu`, `src/gpu/gpu_stub.cpp` | The new backend functions; CUDA keeps its limits. Neutral "built without GPU support". |
| `src/dsp/engine.cpp`, `src/device/live.cpp` | The budgets and minimums come from `gpuLimits()`. |
| `CMakeLists.txt` | `WVT_WITH_METAL`, `OBJCXX`, the frameworks; Accelerate for every target on macOS. |
| `tools/gpu_check.cpp` | The FFT against a direct DFT; the backend name; why a comparison is skipped; the bench shows the GPU budget and minimum. |
| `README.md` | The GPU section covers Metal; options and the file table. |

### Limits for Metal (`gpuBackendLimits`)

| Limit | Metal | CUDA | Reason |
|---|---|---|---|
| Waterfall budget | 512 MS | 2 GS | At about 2–3.6 GS/s, 512 MS takes about the time the CPU needs for its 256 MS. |
| Waterfall minimum | 16 MS of FFT input | 0 | Below it the GPU is no faster (0.9–1.1×). |
| Live rate | 100 MS/s | 100 MS/s | |
| Live minimum | 128 k samples per row | 0 | A small row is as fast on one CPU core. |
| Channel filter minimum | 2·10⁷ multiply-adds | 2·10⁷ | |

### Results on the M5 (`gpu_check`, `gpu_check --bench`)

- **Correctness**, GPU against CPU:
  - waterfall: every format, big-endian, swapped I/Q, header, edges and long rows, within
    0.0002 dB;
  - live: within 0.0002 dB;
  - channel filter: within 1.1e-6 of the peak (the limit is 1e-4).

  Cancel and the forced failure work on all three paths.
- **vDSP CPU FFT:** waterfall 2.0–2.4× faster than before, live row 2.3×.
- **GPU waterfall:**
  - N 1024 at 100× zoom: 39 ms against 61 ms on the CPU (1.6×), the same FFTs.
  - N 8192 and 65536 (MPSGraph): 1.2–1.4× per sample. Wide views get more coverage for
    about the same time.
- **Live row**, N 2048: 0.72 / 1.76 / 2.86 ms on the GPU against 2.43 / 4.88 / 7.26 ms on one
  CPU core, at 20 / 40 / 61.44 MS/s.
- **Channel filter** (8 MS in, decimation 8, with the demodulation on the CPU): 1.8× (255
  taps), 3.7× (2047), **6.2×** (8191).
- **Acceptance (§7):**
  - item 1 (`gpu_check` on the GPU), item 4 (fallback) and item 5 (OFF and Linux builds): met;
  - item 3 (≥ 5× for 8191 taps): met;
  - item 2 (≥ 3× at N 8192): **not met**. N 8192 uses MPSGraph, about 1.2×. A multi-pass FFT
    of our own (the third option of decision 2) would be needed for that.

### Checks

- The macOS build has no warnings; all 6 self-tests pass.
- `-DWVT_WITH_METAL=OFF`: builds the stub ("built without GPU support").
- The CUDA 12.8 container build still compiles with no warnings, and its self-tests pass.

### GUI

The user tested the GUI on screen (2026-10-20). All five checks passed:
- the status bar showed `GPU Apple M5`;
- the GPU checkbox switched the GPU off and on;
- live mode showed `(GPU)`;
- a region export with a band worked;
- `WVT_GPU_FORCE_FAIL=1` gave the `CPU` tag with the reason in the tooltip.
