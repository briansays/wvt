// CUDA GPU backend: device discovery and the file waterfall (spectrogram).
// See docs/cuda-gpu-plan.md.
#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp.h"
#include "gpu_backend.h"
#include "log.h"

// Evaluate a CUDA or cuFFT call. On an error they set `err` (call, file,
// line, message) and `oom`, which the enclosing function must have in scope,
// and return GpuStatus::Failed. gpu_common.cpp logs it and falls back.
#define CUDA_TRY(call)                                                                                \
    do {                                                                                              \
        const cudaError_t e_ = (call);                                                                \
        if (e_ != cudaSuccess) {                                                                      \
            cudaGetLastError();                                                                       \
            err = callError(#call, __FILE__, __LINE__, cudaGetErrorString(e_));                       \
            oom = e_ == cudaErrorMemoryAllocation;                                                    \
            return GpuStatus::Failed;                                                                 \
        }                                                                                             \
    } while (0)
#define CUFFT_TRY(call)                                                                               \
    do {                                                                                              \
        const cufftResult r_ = (call);                                                                \
        if (r_ != CUFFT_SUCCESS) {                                                                    \
            err = callError(#call, __FILE__, __LINE__, ("cuFFT error " + std::to_string((int)r_)).c_str()); \
            oom = r_ == CUFFT_ALLOC_FAILED;                                                           \
            return GpuStatus::Failed;                                                                 \
        }                                                                                             \
    } while (0)

namespace {
std::string callError(const char* call, const char* file, int line, const char* msg) {
    const char* base = strrchr(file, '/');
    char b[512];
    snprintf(b, sizeof b, "%s at %s:%d: %s", call, base ? base + 1 : file, line, msg);
    return b;
}

std::string versionText(int v) {  // 12040 -> "12.4"
    char b[32];
    snprintf(b, sizeof b, "%d.%d", v / 1000, (v % 1000) / 10);
    return b;
}

std::string describe(const cudaDeviceProp& p) {
    char b[320];
    snprintf(b, sizeof b, "%s (compute %d.%d, %.1f GB)", p.name, p.major, p.minor, p.totalGlobalMem / 1e9);
    return b;
}

// Why no device can be used, from the cudaGetDeviceCount error.
std::string noDeviceReason(cudaError_t e) {
    int driver = 0;
    cudaDriverGetVersion(&driver);
    if (e == cudaErrorNoDevice) return "no NVIDIA GPU found";
    if (driver == 0) return "no NVIDIA driver";
    if (e == cudaErrorInsufficientDriver)
        return "the NVIDIA driver (CUDA " + versionText(driver) + ") is older than this build (CUDA " +
               versionText(CUDART_VERSION) + ")";
    return std::string("CUDA cannot start: ") + cudaGetErrorString(e);
}

int deviceCount(std::string& reason) {
    int n = 0;
    const cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess) {
        cudaGetLastError();
        reason = noDeviceReason(e);
        return 0;
    }
    if (n == 0) reason = "no NVIDIA GPU found";
    return n;
}

// Checks one device against the `auto` rules; reason says why it fails.
// This creates the CUDA context on that device.
bool usable(int d, const cudaDeviceProp& p, std::string& reason) {
    if (p.major * 10 + p.minor < kGpuMinCompute) {
        reason = "GPU " + std::to_string(d) + " " + describe(p) + " is older than compute 7.5";
        return false;
    }
    size_t freeB = 0, totalB = 0;
    cudaError_t e = cudaSetDevice(d);
    if (e == cudaSuccess) e = cudaMemGetInfo(&freeB, &totalB);
    if (e != cudaSuccess) {
        cudaGetLastError();
        reason = "GPU " + std::to_string(d) + " cannot start: " + cudaGetErrorString(e);
        return false;
    }
    if ((double)freeB < kGpuMinFreeBytes) {
        char b[96];
        snprintf(b, sizeof b, " has only %.2f GB of free memory (needs 1 GB)", freeB / 1e9);
        reason = "GPU " + std::to_string(d) + " " + describe(p) + b;
        return false;
    }
    return true;
}
}  // namespace

bool gpuBackendBuilt() { return true; }

void gpuBackendProbe(int device, GpuInfo& info) {
    info.built = true;
    info.available = false;
    const int n = deviceCount(info.reason);
    if (n == 0) return;
    std::vector<cudaDeviceProp> props(n);
    for (int d = 0; d < n; ++d)
        if (cudaGetDeviceProperties(&props[d], d) != cudaSuccess) {
            cudaGetLastError();
            props[d] = cudaDeviceProp{};
        }

    std::vector<int> order;
    if (device >= 0) {
        if (device >= n) {
            info.reason = "--gpu-device " + std::to_string(device) + ": there " + (n == 1 ? "is 1 GPU" : "are " + std::to_string(n) + " GPUs");
            return;
        }
        order.push_back(device);
    } else {
        // Best first: newest architecture, then the most multiprocessors.
        for (int d = 0; d < n; ++d) order.push_back(d);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const int ca = props[a].major * 10 + props[a].minor, cb = props[b].major * 10 + props[b].minor;
            return ca != cb ? ca > cb : props[a].multiProcessorCount > props[b].multiProcessorCount;
        });
    }
    std::string firstReason;
    for (int d : order) {
        std::string why;
        if (!usable(d, props[d], why)) {
            if (firstReason.empty()) firstReason = why;
            continue;
        }
        int runtime = 0, driver = 0;
        cudaRuntimeGetVersion(&runtime);
        cudaDriverGetVersion(&driver);
        info.available = true;
        info.device = d;
        info.name = describe(props[d]);
        info.model = props[d].name;
        if (info.model.rfind("NVIDIA ", 0) == 0) info.model.erase(0, 7);
        info.version = "CUDA runtime " + versionText(runtime) + ", driver " + versionText(driver);
        info.reason.clear();
        return;
    }
    info.reason = firstReason;
}

std::vector<std::string> gpuBackendList() {
    std::string reason;
    const int n = deviceCount(reason);
    if (n == 0) return {"none (" + reason + ")"};
    std::vector<std::string> out;
    for (int d = 0; d < n; ++d) {
        cudaDeviceProp p{};
        if (cudaGetDeviceProperties(&p, d) != cudaSuccess) {
            cudaGetLastError();
            out.push_back(std::to_string(d) + "  (cannot read the device properties)");
            continue;
        }
        std::string line = std::to_string(d) + "  " + describe(p);
        if (p.major * 10 + p.minor < kGpuMinCompute) line += "  - not used: older than compute 7.5";
        out.push_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Spectrogram (file waterfall)
// ---------------------------------------------------------------------------
// The host gathers the raw bytes of each FFT from the mapped file into a
// pinned buffer (two of them, so the gather of chunk k+1 overlaps the GPU
// work on chunk k). The GPU decodes, windows, transforms (batched cuFFT),
// sums |X|^2 per row and max-pools to columns. Only rows * cols floats come
// back. Rows longer than one chunk are summed over several chunks.
namespace {
constexpr int64_t kChunkSamples = int64_t(1) << 23;  // samples per chunk (64 MB as cf32)

template <class T>
__device__ inline T loadSample(const uint8_t* p, bool swap) {
    if (!swap) return *reinterpret_cast<const T*>(p);  // aligned: each FFT starts at a multiple of n * bps
    union {
        T v;
        uint8_t b[sizeof(T)];
    } u;
    for (int k = 0; k < (int)sizeof(T); ++k) u.b[k] = p[sizeof(T) - 1 - k];
    return u.v;
}

// Same rules as decodeSamples() in iq_file.cpp, then the window. valid[j]
// is the part [x, y) of FFT j inside the file; the rest is zero. With valid
// null, every sample is used.
__global__ void decodeWindowKernel(const uint8_t* raw, const int2* valid, const float* win, int m, int log2n, int fmt,
                                   int bps, bool bigEndian, bool swapIQ, cufftComplex* out) {
    const int64_t idx = (int64_t)blockIdx.x * blockDim.x + threadIdx.x;
    const int n = 1 << log2n;
    if (idx >= (int64_t)m * n) return;
    const int j = (int)(idx >> log2n), i = (int)(idx & (n - 1));
    const int2 v = valid ? valid[j] : make_int2(0, n);
    float re = 0.f, im = 0.f;
    if (i >= v.x && i < v.y) {
        const uint8_t* p = raw + idx * bps;
        const int half = bps / 2;
        switch ((SampleFormat)fmt) {
            case SampleFormat::CF32:
                re = loadSample<float>(p, bigEndian);
                im = loadSample<float>(p + half, bigEndian);
                break;
            case SampleFormat::CF64:
                re = (float)loadSample<double>(p, bigEndian);
                im = (float)loadSample<double>(p + half, bigEndian);
                break;
            case SampleFormat::CI16:
                re = loadSample<int16_t>(p, bigEndian) * (1.f / 32768.f);
                im = loadSample<int16_t>(p + half, bigEndian) * (1.f / 32768.f);
                break;
            case SampleFormat::CI32:
                re = (float)(loadSample<int32_t>(p, bigEndian) * (1.0 / 2147483648.0));
                im = (float)(loadSample<int32_t>(p + half, bigEndian) * (1.0 / 2147483648.0));
                break;
            case SampleFormat::CI8:
                re = (int8_t)p[0] * (1.f / 128.f);
                im = (int8_t)p[1] * (1.f / 128.f);
                break;
            case SampleFormat::CU8:
                re = (p[0] - 127.5f) * (1.f / 127.5f);
                im = (p[1] - 127.5f) * (1.f / 127.5f);
                break;
            default: break;
        }
        if (swapIQ) {
            const float t = re;
            re = im;
            im = t;
        }
    }
    const float w = win[i];
    out[idx] = make_cuComplex(re * w, im * w);
}

// acc[rl * n + i] += sum of |X|^2 at fftshifted bin i over the FFTs of row
// r0 + rl that are in this chunk (global FFT indices [f0, f1), row * avg + a).
__global__ void accumulateKernel(const cufftComplex* x, int64_t f0, int64_t f1, int r0, int rowsHere, int avg, int log2n,
                                 float* acc) {
    const int64_t idx = (int64_t)blockIdx.x * blockDim.x + threadIdx.x;
    const int n = 1 << log2n;
    if (idx >= (int64_t)rowsHere * n) return;
    const int rl = (int)(idx >> log2n), i = (int)(idx & (n - 1));
    const int64_t row = r0 + rl;
    const int64_t fa = max(f0, row * avg), fb = min(f1, (row + 1) * avg);
    const int k = (i + n / 2) & (n - 1);  // fftshift
    float s = 0.f;
    for (int64_t f = fa; f < fb; ++f) {
        const cufftComplex c = x[((f - f0) << log2n) + k];
        s += c.x * c.x + c.y * c.y;
    }
    acc[idx] += s;
}

// lin[(r0 + rl) * cols + c] = scale * max over the bins of column c.
__global__ void poolKernel(const float* acc, int rowsHere, int n, int cols, float scale, float* lin) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= rowsHere * cols) return;
    const int rl = idx / cols, c = idx - rl * cols, per = n / cols;
    const float* a = acc + (size_t)rl * n + (size_t)c * per;
    float mx = 0.f;
    for (int b = 0; b < per; ++b) mx = fmaxf(mx, a[b]);
    lin[idx] = mx * scale;
}

inline unsigned blocksFor(int64_t threads) { return (unsigned)((threads + 255) / 256); }

// A few batched C2C plans on one stream, least recently used replaced first.
struct PlanCache {
    struct Plan {
        int n = 0, batch = 0;
        cufftHandle h = 0;
        uint64_t used = 0;
    };
    Plan plans[4];
    uint64_t tick = 0;

    void release() {
        for (auto& p : plans)
            if (p.batch) cufftDestroy(p.h);
        *this = PlanCache{};
    }
    GpuStatus get(int n, int batch, cudaStream_t stream, cufftHandle& out, std::string& err, bool& oom) {
        ++tick;
        for (auto& p : plans)
            if (p.batch && p.n == n && p.batch == batch) {
                p.used = tick;
                out = p.h;
                return GpuStatus::Ok;
            }
        Plan* victim = &plans[0];
        for (auto& p : plans)
            if (!p.batch || p.used < victim->used) victim = &p;
        if (victim->batch) {
            cufftDestroy(victim->h);
            *victim = Plan{};
        }
        cufftHandle h;
        CUFFT_TRY(cufftPlan1d(&h, n, CUFFT_C2C, batch));
        if (const cufftResult r = cufftSetStream(h, stream); r != CUFFT_SUCCESS) {
            cufftDestroy(h);
            CUFFT_TRY(r);
        }
        *victim = Plan{n, batch, h, tick};
        out = h;
        return GpuStatus::Ok;
    }
};

// The smallest power of two >= count, at most cap (itself a power of two),
// so few distinct plans are made.
int batchFor(int64_t count, int64_t cap) {
    int64_t b = 1;
    while (b < count) b <<= 1;
    return (int)std::min(b, cap);
}

// log2(n), or -1 when n is not a power of two.
int log2Of(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return n > 0 && (1 << k) == n ? k : -1;
}

// Per-thread state: the stream, the buffers (kept between calls, grown as
// needed) and a few cuFFT plans. Each worker thread gets its own.
struct SpecState {
    int device = -1;
    cudaStream_t stream = nullptr;
    cufftComplex* dX = nullptr;
    uint8_t* dRaw = nullptr;
    int2* dValid = nullptr;
    float *dWin = nullptr, *dAcc = nullptr, *dLin = nullptr;
    size_t capX = 0, capRaw = 0, capValid = 0, capWin = 0, capAcc = 0, capLin = 0;
    uint8_t* hRaw[2] = {nullptr, nullptr};
    int2* hValid[2] = {nullptr, nullptr};
    size_t capHRaw = 0, capHValid = 0;
    cudaEvent_t slotFree[2] = {nullptr, nullptr};
    PlanCache plans;
};
struct SpecContext : SpecState {
    ~SpecContext() { release(); }
    void release() {
        // Errors are ignored: at exit the runtime may already be gone.
        plans.release();
        for (void* d : {(void*)dX, (void*)dRaw, (void*)dValid, (void*)dWin, (void*)dAcc, (void*)dLin}) cudaFree(d);
        for (int k = 0; k < 2; ++k) {
            cudaFreeHost(hRaw[k]);
            cudaFreeHost(hValid[k]);
            if (slotFree[k]) cudaEventDestroy(slotFree[k]);
        }
        if (stream) cudaStreamDestroy(stream);
        static_cast<SpecState&>(*this) = SpecState{};
        cudaGetLastError();
    }
};
thread_local SpecContext t_spec;

template <class T>
cudaError_t growDevice(T*& p, size_t& cap, size_t count) {
    if (count <= cap) return cudaSuccess;
    cudaFree(p);
    p = nullptr;
    cap = 0;
    const cudaError_t e = cudaMalloc((void**)&p, count * sizeof(T));
    if (e == cudaSuccess) cap = count;
    return e;
}

template <class T>
cudaError_t growHost(T* (&p)[2], size_t& cap, size_t count) {
    if (count <= cap) return cudaSuccess;
    for (auto& q : p) {
        cudaFreeHost(q);
        q = nullptr;
    }
    cap = 0;
    for (auto& q : p)
        if (const cudaError_t e = cudaMallocHost((void**)&q, count * sizeof(T)); e != cudaSuccess) return e;
    cap = count;
    return cudaSuccess;
}

// Copies FFTs [f0, f0 + m) (global index row * avg + a) into dst, on the CPU
// threads (reading the mapped file is the slow part). Each FFT takes
// n * bps bytes; valid[k] is its part inside the file.
void gather(const SpecJob& j, int64_t f0, int m, int bps, int64_t fileSamples, uint8_t* dst, int2* valid) {
    const uint8_t* base = j.file->data() + j.fp.headerBytes;
    parallelFor(m, [&](int kb, int ke) {
        for (int k = kb; k < ke; ++k) {
            const int64_t f = f0 + k;
            const int row = (int)(f / j.avg), a = (int)(f % j.avg);
            const int64_t start = specFftStart(j, row, a);
            const int64_t lo = std::max<int64_t>(0, -start), hi = std::min<int64_t>(j.n, fileSamples - start);
            if (hi <= lo) {
                valid[k] = make_int2(0, 0);
                continue;
            }
            valid[k] = make_int2((int)lo, (int)hi);
            memcpy(dst + ((size_t)k * j.n + lo) * bps, base + (start + lo) * bps, (size_t)(hi - lo) * bps);
        }
    }, std::max(1, 65536 / j.n));
}
}  // namespace

GpuStatus gpuBackendSpectrogram(const SpecJob& j, float* lin, const std::function<bool()>& cancelled, std::string& err,
                                bool& oom) {
    SpecContext& c = t_spec;
    const int dev = gpuInfo().device;
    if (c.device != dev) {
        c.release();
        CUDA_TRY(cudaSetDevice(dev));
        CUDA_TRY(cudaStreamCreateWithFlags(&c.stream, cudaStreamNonBlocking));
        for (auto& e : c.slotFree) CUDA_TRY(cudaEventCreateWithFlags(&e, cudaEventDisableTiming));
        c.device = dev;
    } else {
        CUDA_TRY(cudaSetDevice(dev));
    }

    const int n = j.n, bps = formatBytesPerSample(j.fp.fmt), log2n = log2Of(n);
    const int64_t totalFfts = (int64_t)j.rows * j.avg;
    const int64_t fileSamples = j.file->numSamples(j.fp);
    if (j.rows <= 0 || j.cols <= 0 || log2n < 0) {
        err = "bad spectrogram job";
        return GpuStatus::Failed;
    }

    // FFTs per chunk: whole rows when a row fits, else one row in pieces.
    const int64_t maxFfts = std::max<int64_t>(1, kChunkSamples / n);
    const int64_t chunkFfts = std::min(maxFfts, totalFfts);
    const int rowsPerGroup = j.avg <= chunkFfts ? (int)(chunkFfts / j.avg) : 1;
    const int batch = batchFor(chunkFfts, maxFfts);

    CUDA_TRY(growDevice(c.dX, c.capX, (size_t)batch * n));
    CUDA_TRY(growDevice(c.dRaw, c.capRaw, (size_t)batch * n * bps));
    CUDA_TRY(growDevice(c.dValid, c.capValid, (size_t)batch));
    CUDA_TRY(growDevice(c.dWin, c.capWin, (size_t)n));
    CUDA_TRY(growDevice(c.dAcc, c.capAcc, (size_t)rowsPerGroup * n));
    CUDA_TRY(growDevice(c.dLin, c.capLin, (size_t)j.rows * j.cols));
    CUDA_TRY(growHost(c.hRaw, c.capHRaw, (size_t)batch * n * bps));
    CUDA_TRY(growHost(c.hValid, c.capHValid, (size_t)batch));
    cufftHandle plan;
    if (const GpuStatus st = c.plans.get(n, batch, c.stream, plan, err, oom); st != GpuStatus::Ok) return st;

    CUDA_TRY(cudaMemcpyAsync(c.dWin, j.win, (size_t)n * sizeof(float), cudaMemcpyHostToDevice, c.stream));
    const float scale = j.norm / j.avg;
    int slot = 0;
    for (int r0 = 0; r0 < j.rows; r0 += rowsPerGroup) {
        const int rowsHere = std::min(rowsPerGroup, j.rows - r0);
        const int64_t gEnd = (int64_t)(r0 + rowsHere) * j.avg;
        CUDA_TRY(cudaMemsetAsync(c.dAcc, 0, (size_t)rowsHere * n * sizeof(float), c.stream));
        for (int64_t f0 = (int64_t)r0 * j.avg; f0 < gEnd; f0 += chunkFfts) {
            const int m = (int)std::min(chunkFfts, gEnd - f0);
            // Wait until the copy that last used this staging slot is done.
            CUDA_TRY(cudaEventSynchronize(c.slotFree[slot]));
            if (cancelled()) {
                cudaStreamSynchronize(c.stream);
                return GpuStatus::Cancelled;
            }
            gather(j, f0, m, bps, fileSamples, c.hRaw[slot], c.hValid[slot]);
            CUDA_TRY(cudaMemcpyAsync(c.dRaw, c.hRaw[slot], (size_t)m * n * bps, cudaMemcpyHostToDevice, c.stream));
            CUDA_TRY(cudaMemcpyAsync(c.dValid, c.hValid[slot], (size_t)m * sizeof(int2), cudaMemcpyHostToDevice, c.stream));
            CUDA_TRY(cudaEventRecord(c.slotFree[slot], c.stream));
            slot ^= 1;
            decodeWindowKernel<<<blocksFor((int64_t)m * n), 256, 0, c.stream>>>(
                c.dRaw, c.dValid, c.dWin, m, log2n, (int)j.fp.fmt, bps, j.fp.bigEndian, j.fp.swapIQ, c.dX);
            CUDA_TRY(cudaGetLastError());
            // The batch may be larger than m; the extra transforms are ignored.
            CUFFT_TRY(cufftExecC2C(plan, c.dX, c.dX, CUFFT_FORWARD));
            accumulateKernel<<<blocksFor((int64_t)rowsHere * n), 256, 0, c.stream>>>(c.dX, f0, f0 + m, r0, rowsHere, j.avg,
                                                                                    log2n, c.dAcc);
            CUDA_TRY(cudaGetLastError());
        }
        poolKernel<<<blocksFor((int64_t)rowsHere * j.cols), 256, 0, c.stream>>>(c.dAcc, rowsHere, n, j.cols, scale,
                                                                               c.dLin + (size_t)r0 * j.cols);
        CUDA_TRY(cudaGetLastError());
    }
    CUDA_TRY(cudaMemcpyAsync(lin, c.dLin, (size_t)j.rows * j.cols * sizeof(float), cudaMemcpyDeviceToHost, c.stream));
    CUDA_TRY(cudaStreamSynchronize(c.stream));
    return GpuStatus::Ok;
}

// ---------------------------------------------------------------------------
// Live spectrum
// ---------------------------------------------------------------------------
// One row: frames * n raw samples go up, n summed powers come back. Large
// rows go in chunks of kChunkSamples.
struct GpuLiveImpl {
    int device = -1;
    cudaStream_t stream = nullptr;
    cufftComplex* dX = nullptr;
    uint8_t* dRaw = nullptr;
    float *dWin = nullptr, *dPw = nullptr;
    size_t capX = 0, capRaw = 0, capWin = 0, capPw = 0;
    PlanCache plans;

    ~GpuLiveImpl() {
        plans.release();
        for (void* d : {(void*)dX, (void*)dRaw, (void*)dWin, (void*)dPw}) cudaFree(d);
        if (stream) cudaStreamDestroy(stream);
        cudaGetLastError();
    }
};

GpuLiveImpl* gpuBackendLiveCreate() { return new GpuLiveImpl; }
void gpuBackendLiveDestroy(GpuLiveImpl* impl) { delete impl; }

GpuStatus gpuBackendLiveRun(GpuLiveImpl* L, const uint8_t* raw, const FormatParams& fp, int n, int frames,
                            const float* win, float* pw, std::string& err, bool& oom) {
    const int dev = gpuInfo().device;
    CUDA_TRY(cudaSetDevice(dev));
    if (L->device != dev) {
        if (L->stream) cudaStreamDestroy(L->stream);
        L->stream = nullptr;
        CUDA_TRY(cudaStreamCreateWithFlags(&L->stream, cudaStreamNonBlocking));
        L->device = dev;
    }
    const int log2n = log2Of(n), bps = formatBytesPerSample(fp.fmt);
    if (log2n < 0 || frames < 1) {
        err = "bad live spectrum job";
        return GpuStatus::Failed;
    }
    const int64_t maxFrames = std::max<int64_t>(1, kChunkSamples / n);
    const int64_t chunk = std::min<int64_t>(frames, maxFrames);
    const int batch = batchFor(chunk, maxFrames);
    CUDA_TRY(growDevice(L->dX, L->capX, (size_t)batch * n));
    CUDA_TRY(growDevice(L->dRaw, L->capRaw, (size_t)chunk * n * bps));
    CUDA_TRY(growDevice(L->dWin, L->capWin, (size_t)n));
    CUDA_TRY(growDevice(L->dPw, L->capPw, (size_t)n));
    cufftHandle plan;
    if (const GpuStatus st = L->plans.get(n, batch, L->stream, plan, err, oom); st != GpuStatus::Ok) return st;

    CUDA_TRY(cudaMemcpyAsync(L->dWin, win, (size_t)n * sizeof(float), cudaMemcpyHostToDevice, L->stream));
    CUDA_TRY(cudaMemsetAsync(L->dPw, 0, (size_t)n * sizeof(float), L->stream));
    for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
        const int m = (int)std::min<int64_t>(chunk, frames - f0);
        CUDA_TRY(cudaMemcpyAsync(L->dRaw, raw + (size_t)f0 * n * bps, (size_t)m * n * bps, cudaMemcpyHostToDevice,
                                 L->stream));
        decodeWindowKernel<<<blocksFor((int64_t)m * n), 256, 0, L->stream>>>(L->dRaw, nullptr, L->dWin, m, log2n,
                                                                            (int)fp.fmt, bps, fp.bigEndian, fp.swapIQ, L->dX);
        CUDA_TRY(cudaGetLastError());
        CUFFT_TRY(cufftExecC2C(plan, L->dX, L->dX, CUFFT_FORWARD));
        // One "row" holding all the frames; this chunk adds frames [f0, f0 + m).
        accumulateKernel<<<blocksFor(n), 256, 0, L->stream>>>(L->dX, f0, f0 + m, 0, 1, frames, log2n, L->dPw);
        CUDA_TRY(cudaGetLastError());
    }
    CUDA_TRY(cudaMemcpyAsync(pw, L->dPw, (size_t)n * sizeof(float), cudaMemcpyDeviceToHost, L->stream));
    CUDA_TRY(cudaStreamSynchronize(L->stream));
    return GpuStatus::Ok;
}
