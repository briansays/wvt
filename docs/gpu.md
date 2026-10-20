# GPU processing

wvt can calculate these items on the GPU:

- The file waterfall.
- The live spectrum.
- The channel filter of the time plots and of the region export.

wvt supports CUDA on an NVIDIA GPU under Linux, and Metal on an Apple GPU under macOS. With a GPU,
wide views can keep "FFT sees 100 %", and the live spectrum can analyze all samples at USRP rates.
The results are the same as on the CPU.

## NVIDIA GPUs (CUDA, Linux)

**Build:**

- The CUDA toolkit 12.0 or later. For RTX 50-series GPUs, use 12.8 or later.
- CMake 3.18 or later.
- CMake finds `nvcc` and shows `wvt: CUDA support enabled (CUDA 12.x, archs ..., static cuFFT)`.

**Run:**

- An NVIDIA driver.
- A GPU with compute capability 7.5 or later (GeForce RTX 20xx and newer).
- A minimum of 1 GB of free GPU memory.

A CUDA build also runs on computers without a GPU or a driver. wvt then uses the CPU, and the log
tells you why.

## Apple GPUs (Metal, macOS)

**Build:** you do not have to install more software. The Command Line Tools are sufficient. You do
not need Xcode. CMake shows `wvt: Metal support enabled`.

**Run:**

- macOS 14 or later. Older versions use the CPU.
- The kernels compile when wvt uses the GPU for the first time (approximately 0.2 s).

On a Mac, the CPU path uses the Accelerate FFT (vDSP). Thus the CPU is also fast. On an M5, the
GPU gives these speeds, compared with the CPU:

| Job | Speed |
|---|---|
| Waterfall, N ≤ 4096 | Approximately 1.6 times |
| One live row | 2.5 to 3.4 times |
| Channel filter with many taps | Up to 6 times |

Small jobs stay on the CPU.

For the CMake options, refer to [Build reference](build.md#wvt_with_cuda).

## Select the GPU at run time

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

## Errors

If a GPU error occurs, that part of wvt goes back to the CPU for the rest of the session. The log
and the status-bar tooltip tell you why.

To test the fallback without an error, set `WVT_GPU_FORCE_FAIL=1`. Each GPU call then fails, and
wvt uses the CPU.

## Check the GPU

- `build/gpu_check` compares the GPU results with the CPU results.
- `build/gpu_check --bench` measures the speed of the GPU and the CPU.

## How it works

`src/gpu.h` is the interface that the rest of wvt uses. `src/gpu_common.cpp` applies the `--gpu`
rules and the fallback to the CPU. Each backend implements the interface in `src/gpu_backend.h`:

| Backend | File | FFT |
|---|---|---|
| CUDA | `src/gpu_cuda.cu` | cuFFT |
| Metal | `src/gpu_metal.mm` | FFTs up to 4096 points in one pass in the on-chip memory of the GPU. Larger FFTs use MPSGraph |
| None | `src/gpu_stub.cpp` | The CPU only |

On a Mac, the GPU reads the memory-mapped file in place, because the CPU and the GPU share the
memory. wvt sends a job to the GPU only when the job is sufficiently large. Each job type has its
own work limit.

For the designs, refer to [the CUDA design record](design/cuda-gpu-plan.md) and
[the Apple GPU design record](design/apple-gpu-plan.md).
