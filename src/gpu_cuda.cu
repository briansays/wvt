// CUDA implementation of gpu.h: device setup, the file spectrogram and the
// live spectrum. Raw samples go to the device in their file format and are
// decoded there, so only the narrow format crosses PCIe, and only the pooled
// rows x cols result comes back.
#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>

#include "gpu.h"

namespace {

// ---------------------------------------------------------------------------
// State and error handling
// ---------------------------------------------------------------------------
GpuInfo gInfo;
std::atomic<bool> gUserOn{true};
std::atomic<bool> gWfFailed{false}, gLiveFailed{false};
int64_t gBatch = 1 << 22;  // complex samples per FFT batch, set from free memory in gpuInit
std::mutex gNoteM;
std::string gNotes;
thread_local std::string tErr;  // first error of the current call

void addNote(const std::string& s) {
    std::lock_guard<std::mutex> lk(gNoteM);
    if (gNotes.find(s) != std::string::npos) return;
    if (!gNotes.empty()) gNotes += "\n";
    gNotes += s;
    fprintf(stderr, "wvt: GPU: %s\n", s.c_str());
}

const char* cufftName(cufftResult r) {
    switch (r) {
        case CUFFT_SUCCESS: return "success";
        case CUFFT_INVALID_PLAN: return "invalid plan";
        case CUFFT_ALLOC_FAILED: return "out of memory";
        case CUFFT_INVALID_VALUE: return "invalid value";
        case CUFFT_INTERNAL_ERROR: return "internal error";
        case CUFFT_EXEC_FAILED: return "exec failed";
        case CUFFT_SETUP_FAILED: return "setup failed";
        case CUFFT_INVALID_SIZE: return "invalid size";
        default: return "error";
    }
}

bool cuOk(cudaError_t e, const char* what, int line) {
    if (e == cudaSuccess) return true;
    if (tErr.empty()) {
        char b[256];
        snprintf(b, sizeof b, "%s failed: %s (gpu_cuda.cu:%d)", what, cudaGetErrorString(e), line);
        tErr = b;
    }
    return false;
}
bool cuOk(cufftResult e, const char* what, int line) {
    if (e == CUFFT_SUCCESS) return true;
    if (tErr.empty()) {
        char b[256];
        snprintf(b, sizeof b, "%s failed: cuFFT %s (gpu_cuda.cu:%d)", what, cufftName(e), line);
        tErr = b;
    }
    return false;
}
#define CU_TRY(x)                                                  \
    do {                                                           \
        if (!cuOk((x), #x, __LINE__)) return GpuResult::Failed;    \
    } while (0)
#define CU_BOOL(x)                                     \
    do {                                               \
        if (!cuOk((x), #x, __LINE__)) return false;    \
    } while (0)

// ---------------------------------------------------------------------------
// Buffers and FFT plans
// ---------------------------------------------------------------------------
struct DevBuf {
    void* p = nullptr;
    size_t cap = 0;
    ~DevBuf() { cudaFree(p); }
    bool reserve(size_t bytes) {
        if (bytes <= cap) return true;
        cudaFree(p);
        p = nullptr;
        cap = 0;
        CU_BOOL(cudaMalloc(&p, bytes));
        cap = bytes;
        return true;
    }
    template <class T> T* as() const { return (T*)p; }
};

struct HostBuf {
    void* p = nullptr;
    size_t cap = 0;
    ~HostBuf() { cudaFreeHost(p); }
    bool reserve(size_t bytes) {
        if (bytes <= cap) return true;
        cudaFreeHost(p);
        p = nullptr;
        cap = 0;
        CU_BOOL(cudaHostAlloc(&p, bytes, cudaHostAllocDefault));
        cap = bytes;
        return true;
    }
    template <class T> T* as() const { return (T*)p; }
};

// Batched C2C plans keyed by (n, batch), least recently used dropped first.
// The plans share one work area, so cached plans do not each hold VRAM.
class PlanCache {
public:
    ~PlanCache() {
        for (auto& e : plans_) cufftDestroy(e.h);
    }
    bool get(int n, int batch, cudaStream_t s, cufftHandle& out) {
        auto it = std::find_if(plans_.begin(), plans_.end(), [&](const Entry& e) { return e.n == n && e.batch == batch; });
        if (it != plans_.end()) {
            plans_.splice(plans_.begin(), plans_, it);
        } else {
            if (plans_.size() >= 6) {
                cufftDestroy(plans_.back().h);
                plans_.pop_back();
            }
            Entry e{n, batch, 0, 0};
            CU_BOOL(cufftCreate(&e.h));
            int dims[1] = {n};
            if (!cuOk(cufftSetAutoAllocation(e.h, 0), "cufftSetAutoAllocation", __LINE__) ||
                !cuOk(cufftMakePlanMany(e.h, 1, dims, nullptr, 1, n, nullptr, 1, n, CUFFT_C2C, batch, &e.work),
                      "cufftMakePlanMany", __LINE__)) {
                cufftDestroy(e.h);
                return false;
            }
            plans_.push_front(e);
        }
        Entry& e = plans_.front();
        // The work area may move when a larger plan needs more, so set it on every use.
        if (!work_.reserve(std::max<size_t>(e.work, 1))) return false;
        CU_BOOL(cufftSetWorkArea(e.h, work_.p));
        CU_BOOL(cufftSetStream(e.h, s));
        out = e.h;
        return true;
    }

private:
    struct Entry {
        int n, batch;
        cufftHandle h;
        size_t work;
    };
    std::list<Entry> plans_;
    DevBuf work_;
};

// Rounds an FFT count up so a few plans cover the batch sizes that occur.
// The extra FFTs get zero input and add nothing.
int padBatch(int ffts) { return ffts <= 64 ? ffts : (ffts + 63) / 64 * 64; }

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------
struct Decode {
    int fmt = 0;  // SampleFormat
    int bps = 8;
    bool swapIQ = false, bigEndian = false;
};

template <class T>
__device__ T load(const uint8_t* p, bool swap) {
    uint8_t b[sizeof(T)];
    for (int i = 0; i < (int)sizeof(T); ++i) b[i] = swap ? p[sizeof(T) - 1 - i] : p[i];
    T v;
    memcpy(&v, b, sizeof(T));
    return v;
}

// Same scaling as decodeSamples() in iq_file.cpp, in single precision.
__device__ float2 decodeOne(const uint8_t* p, const Decode& d) {
    float re = 0, im = 0;
    const int half = d.bps / 2;
    const bool sw = d.bigEndian;
    switch ((SampleFormat)d.fmt) {
        case SampleFormat::CF32:
            if (!sw) {
                const float2 v = *(const float2*)p;
                re = v.x, im = v.y;
            } else {
                re = load<float>(p, true), im = load<float>(p + half, true);
            }
            break;
        case SampleFormat::CF64:
            re = (float)load<double>(p, sw), im = (float)load<double>(p + half, sw);
            break;
        case SampleFormat::CI16:
            if (!sw) {
                const short2 v = *(const short2*)p;
                re = v.x * (1.f / 32768.f), im = v.y * (1.f / 32768.f);
            } else {
                re = load<int16_t>(p, true) * (1.f / 32768.f), im = load<int16_t>(p + half, true) * (1.f / 32768.f);
            }
            break;
        case SampleFormat::CI32:
            re = load<int32_t>(p, sw) * (1.f / 2147483648.f), im = load<int32_t>(p + half, sw) * (1.f / 2147483648.f);
            break;
        case SampleFormat::CI8:
            re = (int8_t)p[0] * (1.f / 128.f), im = (int8_t)p[1] * (1.f / 128.f);
            break;
        case SampleFormat::CU8:
            re = (p[0] - 127.5f) * (1.f / 127.5f), im = (p[1] - 127.5f) * (1.f / 127.5f);
            break;
        default: break;
    }
    return d.swapIQ ? make_float2(im, re) : make_float2(re, im);
}

// Fills `total` = ffts * n complex FFT inputs. FFT f covers absolute samples
// starts[f] .. starts[f]+n-1 (starts == nullptr: f*n, contiguous). Samples
// outside [0, nValid) and FFTs at or past validFfts are zero.
// rawBase >= 0: raw[0] holds absolute sample rawBase (contiguous copy).
// rawBase < 0: raw holds n samples per FFT, gathered in FFT order.
__global__ void kDecodeWindow(const uint8_t* raw, Decode d, const long long* starts, long long rawBase,
                              long long nValid, const float* win, int n, int validFfts, long long total,
                              cufftComplex* out) {
    const long long idx = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= total) return;
    const int f = (int)(idx / n), i = (int)(idx - (long long)f * n);
    float2 v = make_float2(0.f, 0.f);
    if (f < validFfts) {
        const long long s = (starts ? starts[f] : (long long)f * n) + i;
        if (s >= 0 && s < nValid) {
            const long long off = rawBase >= 0 ? s - rawBase : idx;
            v = decodeOne(raw + off * d.bps, d);
        }
    }
    const float w = win[i];
    out[idx] = make_float2(v.x * w, v.y * w);
}

// acc[r*n + k] (+)= sum over A FFTs of row r of |X|^2, fftshifted. Row r uses
// FFTs r*A .. r*A+A-1. The FFTs are summed in order, as on the CPU.
__global__ void kAccumulate(const cufftComplex* X, int n, int R, int A, bool add, float* acc) {
    const long long idx = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= (long long)R * n) return;
    const int r = (int)(idx / n), k = (int)(idx - (long long)r * n);
    const cufftComplex* x = X + (long long)r * A * n + ((k + n / 2) & (n - 1));
    float s = 0.f;
    for (int a = 0; a < A; ++a) {
        const float2 v = x[(long long)a * n];
        s += v.x * v.x + v.y * v.y;
    }
    acc[idx] = add ? acc[idx] + s : s;
}

// Scales R accumulated rows, max-pools them to cols columns and writes the
// linear and dB values at output row offset rowOff.
__global__ void kFinalize(const float* acc, int n, int R, int cols, float scale, int rowOff, float* lin, float* db) {
    const long long idx = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= (long long)R * cols) return;
    const int r = (int)(idx / cols), c = (int)(idx - (long long)r * cols);
    const int per = n / cols;
    const float* a = acc + (long long)r * n + (long long)c * per;
    float mx = 0.f;
    for (int b = 0; b < per; ++b) mx = fmaxf(mx, a[b]);
    mx *= scale;
    const long long o = (long long)(rowOff + r) * cols + c;
    lin[o] = mx;
    db[o] = 10.f * log10f(mx + 1e-20f);
}

constexpr int kThreads = 256;
unsigned blocksFor(long long total) { return (unsigned)((total + kThreads - 1) / kThreads); }

// ---------------------------------------------------------------------------
// Per-thread context for the file spectrogram (each Worker has its own thread)
// ---------------------------------------------------------------------------
struct Slot {
    HostBuf raw, starts;  // pinned staging
    DevBuf dRaw, dStarts;
    cudaEvent_t free = nullptr;  // the decode kernel that read this slot finished
};

struct SpecCtx {
    bool ready = false;
    cudaStream_t s = nullptr;
    Slot slot[2];
    DevBuf X, acc, win, lin, db;
    PlanCache plans;
    ~SpecCtx() {
        for (auto& sl : slot)
            if (sl.free) cudaEventDestroy(sl.free);
        if (s) cudaStreamDestroy(s);
    }
    bool init() {
        if (ready) return true;
        CU_BOOL(cudaSetDevice(gInfo.device));
        CU_BOOL(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
        for (auto& sl : slot) CU_BOOL(cudaEventCreateWithFlags(&sl.free, cudaEventDisableTiming));
        ready = true;
        return true;
    }
};

GpuResult spectrogram(SpecCtx& c, const GpuSpectrogramJob& j, const std::function<bool()>& cancelled) {
    const int n = j.n, rows = j.rows, cols = j.cols, avg = j.avg;
    if (n < 16 || (n & (n - 1)) || rows < 1 || cols < 1 || n % cols || avg < 1 || !j.file) return GpuResult::Failed;
    const int64_t N = j.file->numSamples(j.fp);
    const uint8_t* src = j.file->samples(j.fp);
    const int bps = formatBytesPerSample(j.fp.fmt);
    Decode dec;
    dec.fmt = (int)j.fp.fmt;
    dec.bps = bps;
    dec.swapIQ = j.fp.swapIQ;
    dec.bigEndian = j.fp.bigEndian;

    // Batch layout: whole rows when a row fits, else part of one row per batch.
    const int maxFfts = (int)std::max<int64_t>(1, gBatch / n);
    const int rowsPerBatch = avg <= maxFfts ? std::max(1, maxFfts / avg) : 1;
    const int fftsPerSub = avg <= maxFfts ? avg : maxFfts;
    const int maxBatchFfts = padBatch(rowsPerBatch * fftsPerSub);

    if (!c.X.reserve((size_t)maxBatchFfts * n * sizeof(cufftComplex)) ||
        !c.acc.reserve((size_t)rowsPerBatch * n * sizeof(float)) || !c.win.reserve((size_t)n * sizeof(float)) ||
        !c.lin.reserve((size_t)rows * cols * sizeof(float)) || !c.db.reserve((size_t)rows * cols * sizeof(float)))
        return GpuResult::Failed;
    CU_TRY(cudaMemcpyAsync(c.win.p, j.win, (size_t)n * sizeof(float), cudaMemcpyHostToDevice, c.s));

    int batchNo = 0;
    for (int r0 = 0; r0 < rows; r0 += rowsPerBatch) {
        const int R = std::min(rowsPerBatch, rows - r0);
        for (int a0 = 0; a0 < avg; a0 += fftsPerSub) {
            const int A = std::min(fftsPerSub, avg - a0);
            const int ffts = R * A, padded = padBatch(ffts);
            Slot& sl = c.slot[batchNo++ & 1];
            CU_TRY(cudaEventSynchronize(sl.free));

            // FFT start positions, computed exactly as computeWaterfall does on the CPU.
            if (!sl.starts.reserve((size_t)ffts * sizeof(long long)) || !sl.dStarts.reserve((size_t)ffts * sizeof(long long)))
                return GpuResult::Failed;
            long long* st = sl.starts.as<long long>();
            for (int rr = 0; rr < R; ++rr)
                for (int a = a0; a < a0 + A; ++a) {
                    const int row = r0 + rr;
                    double center = j.s0 + row * j.hop + (a + 0.5) * j.hop / avg;
                    st[rr * A + (a - a0)] = (long long)std::llround(center) - n / 2;
                }
            // Copy the span when the FFTs leave no large gaps between them, else gather each FFT's samples.
            const int64_t first = st[0], span = st[ffts - 1] + n - first;
            const bool contiguous = span <= (int64_t)ffts * n + (int64_t)ffts * n / 4 + n;
            const int64_t rawSamples = contiguous ? span : (int64_t)ffts * n;
            if (!sl.raw.reserve((size_t)rawSamples * bps) || !sl.dRaw.reserve((size_t)rawSamples * bps))
                return GpuResult::Failed;
            uint8_t* stage = sl.raw.as<uint8_t>();
            int64_t copyFrom = 0, copyBytes = 0;  // part of the staging buffer that holds file data
            if (contiguous) {
                const int64_t lo = std::max<int64_t>(first, 0), hi = std::min<int64_t>(first + span, N);
                if (hi > lo) {
                    copyFrom = (lo - first) * bps;
                    copyBytes = (hi - lo) * bps;
                    memcpy(stage + copyFrom, src + lo * bps, (size_t)copyBytes);
                }
            } else {
                for (int f = 0; f < ffts; ++f) {
                    const int64_t lo = std::max<int64_t>(st[f], 0), hi = std::min<int64_t>(st[f] + n, N);
                    if (hi > lo) memcpy(stage + ((int64_t)f * n + lo - st[f]) * bps, src + lo * bps, (size_t)(hi - lo) * bps);
                }
                copyBytes = rawSamples * bps;
            }
            if (copyBytes > 0)
                CU_TRY(cudaMemcpyAsync(sl.dRaw.as<uint8_t>() + copyFrom, stage + copyFrom, (size_t)copyBytes,
                                       cudaMemcpyHostToDevice, c.s));
            CU_TRY(cudaMemcpyAsync(sl.dStarts.p, st, (size_t)ffts * sizeof(long long), cudaMemcpyHostToDevice, c.s));

            const long long total = (long long)padded * n;
            kDecodeWindow<<<blocksFor(total), kThreads, 0, c.s>>>(sl.dRaw.as<uint8_t>(), dec, sl.dStarts.as<long long>(),
                                                                  contiguous ? first : -1, N, c.win.as<float>(), n, ffts,
                                                                  total, c.X.as<cufftComplex>());
            CU_TRY(cudaGetLastError());
            CU_TRY(cudaEventRecord(sl.free, c.s));

            cufftHandle plan;
            if (!c.plans.get(n, padded, c.s, plan)) return GpuResult::Failed;
            CU_TRY(cufftExecC2C(plan, c.X.as<cufftComplex>(), c.X.as<cufftComplex>(), CUFFT_FORWARD));
            kAccumulate<<<blocksFor((long long)R * n), kThreads, 0, c.s>>>(c.X.as<cufftComplex>(), n, R, A, a0 > 0,
                                                                          c.acc.as<float>());
            CU_TRY(cudaGetLastError());
            if (a0 + A >= avg) {
                kFinalize<<<blocksFor((long long)R * cols), kThreads, 0, c.s>>>(c.acc.as<float>(), n, R, cols, j.scale,
                                                                               r0, c.lin.as<float>(), c.db.as<float>());
                CU_TRY(cudaGetLastError());
            }
            if (cancelled && cancelled()) {
                cudaStreamSynchronize(c.s);
                return GpuResult::Cancelled;
            }
        }
    }
    CU_TRY(cudaMemcpyAsync(j.lin, c.lin.p, (size_t)rows * cols * sizeof(float), cudaMemcpyDeviceToHost, c.s));
    CU_TRY(cudaMemcpyAsync(j.db, c.db.p, (size_t)rows * cols * sizeof(float), cudaMemcpyDeviceToHost, c.s));
    CU_TRY(cudaStreamSynchronize(c.s));
    return GpuResult::Ok;
}

// ---------------------------------------------------------------------------
// Live spectrum
// ---------------------------------------------------------------------------
class CudaLiveSpectrum final : public GpuLiveSpectrum {
public:
    CudaLiveSpectrum(int n, SampleFormat fmt) : n_(n) {
        dec_.fmt = (int)fmt;
        dec_.bps = formatBytesPerSample(fmt);
    }
    ~CudaLiveSpectrum() override {
        if (s_) cudaStreamDestroy(s_);
    }
    bool init() {
        CU_BOOL(cudaSetDevice(gInfo.device));
        CU_BOOL(cudaStreamCreateWithFlags(&s_, cudaStreamNonBlocking));
        return win_.reserve((size_t)n_ * sizeof(float)) && acc_.reserve((size_t)n_ * sizeof(float));
    }
    GpuResult run(const uint8_t* raw, int frames, const float* win, float* pw) override {
        tErr.clear();
        const GpuResult r = runImpl(raw, frames, win, pw);
        if (r == GpuResult::Failed) {
            gLiveFailed = true;
            addNote("live spectrum moved to the CPU: " + tErr);
        }
        return r;
    }

private:
    GpuResult runImpl(const uint8_t* raw, int frames, const float* win, float* pw) {
        if (frames < 1) return GpuResult::Failed;
        const int padded = padBatch(frames);
        const size_t bytes = (size_t)frames * n_ * dec_.bps;
        if (!stage_.reserve(bytes) || !dRaw_.reserve(bytes) || !X_.reserve((size_t)padded * n_ * sizeof(cufftComplex)))
            return GpuResult::Failed;
        memcpy(stage_.p, raw, bytes);
        CU_TRY(cudaMemcpyAsync(dRaw_.p, stage_.p, bytes, cudaMemcpyHostToDevice, s_));
        CU_TRY(cudaMemcpyAsync(win_.p, win, (size_t)n_ * sizeof(float), cudaMemcpyHostToDevice, s_));
        const long long total = (long long)padded * n_;
        kDecodeWindow<<<blocksFor(total), kThreads, 0, s_>>>(dRaw_.as<uint8_t>(), dec_, nullptr, 0,
                                                             (long long)frames * n_, win_.as<float>(), n_, frames, total,
                                                             X_.as<cufftComplex>());
        CU_TRY(cudaGetLastError());
        cufftHandle plan;
        if (!plans_.get(n_, padded, s_, plan)) return GpuResult::Failed;
        CU_TRY(cufftExecC2C(plan, X_.as<cufftComplex>(), X_.as<cufftComplex>(), CUFFT_FORWARD));
        kAccumulate<<<blocksFor(n_), kThreads, 0, s_>>>(X_.as<cufftComplex>(), n_, 1, frames, false, acc_.as<float>());
        CU_TRY(cudaGetLastError());
        CU_TRY(cudaMemcpyAsync(pw, acc_.p, (size_t)n_ * sizeof(float), cudaMemcpyDeviceToHost, s_));
        CU_TRY(cudaStreamSynchronize(s_));
        return GpuResult::Ok;
    }

    int n_;
    Decode dec_;
    cudaStream_t s_ = nullptr;
    HostBuf stage_;
    DevBuf dRaw_, X_, win_, acc_;
    PlanCache plans_;
};

std::string describe(const cudaDeviceProp& p) {
    char b[192];
    snprintf(b, sizeof b, "%s (compute %d.%d, %.1f GB)", p.name, p.major, p.minor, p.totalGlobalMem / 1073741824.0);
    return b;
}

std::string runtimeVersion() {
    int v = 0;
    cudaRuntimeGetVersion(&v);
    char b[32];
    snprintf(b, sizeof b, "%d.%d", v / 1000, (v % 1000) / 10);
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
bool gpuInit(GpuMode mode, int device, std::string& err) {
    gInfo = GpuInfo{};
    gInfo.device = device;
    if (mode == GpuMode::Off) {
        gInfo.reason = "switched off with --gpu off";
        return true;
    }
    auto fail = [&](const std::string& why) {
        gInfo.reason = why;
        if (mode == GpuMode::On) {
            err = "--gpu on: " + why;
            return false;
        }
        return true;
    };
    int count = 0;
    const cudaError_t e = cudaGetDeviceCount(&count);
    if (e == cudaErrorNoDevice || (e == cudaSuccess && count == 0)) return fail("no CUDA device found");
    if (e == cudaErrorInsufficientDriver)
        return fail("the NVIDIA driver is too old for this build (CUDA " + runtimeVersion() + ")");
    if (e != cudaSuccess) return fail(std::string("CUDA is not usable: ") + cudaGetErrorString(e));
    if (device < 0 || device >= count) return fail("no CUDA device " + std::to_string(device));
    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, device) != cudaSuccess) return fail("cannot read the device properties");
    gInfo.name = p.name;
    gInfo.ccMajor = p.major;
    gInfo.ccMinor = p.minor;
    if (p.major * 10 + p.minor < 75)
        return fail(describe(p) + " is older than compute capability 7.5 (RTX 20 series)");
    if (cudaSetDevice(device) != cudaSuccess || cudaMemGetInfo(&gInfo.memFree, &gInfo.memTotal) != cudaSuccess)
        return fail("cannot open " + describe(p));
    if (gInfo.memFree < ((size_t)1 << 30)) return fail(describe(p) + " has less than 1 GB free");
    // About 36 bytes of device memory per batched sample (FFT data, raw input,
    // cuFFT work area, accumulators). Use at most a quarter of the free memory.
    int64_t b = (int64_t)(gInfo.memFree / 4 / 36);
    int64_t pow2 = 1;
    while (pow2 * 2 <= b) pow2 *= 2;
    gBatch = std::clamp<int64_t>(pow2, 1 << 20, 1 << 25);
    gInfo.available = true;
    return true;
}

const GpuInfo& gpuInfo() { return gInfo; }

std::string gpuDeviceList() {
    int count = 0;
    const cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess || count == 0) {
        char b[160];
        snprintf(b, sizeof b, "%-8s %-28s %s\n", "gpu", "-",
                 e == cudaErrorInsufficientDriver ? "NVIDIA driver too old for this build"
                 : e == cudaSuccess || e == cudaErrorNoDevice ? "no CUDA device found"
                                                              : cudaGetErrorString(e));
        return b;
    }
    std::string out;
    for (int i = 0; i < count; ++i) {
        cudaDeviceProp p{};
        if (cudaGetDeviceProperties(&p, i) != cudaSuccess) continue;
        char b[256];
        snprintf(b, sizeof b, "%-8s %-28s %s%s\n", "gpu", std::to_string(i).c_str(), describe(p).c_str(),
                 p.major * 10 + p.minor < 75 ? "  [not supported: needs compute 7.5+]" : "");
        out += b;
    }
    return out;
}

bool gpuEnabled() { return gInfo.available && gUserOn.load(); }
void gpuSetEnabled(bool on) { gUserOn = on; }

std::string gpuNotes() {
    std::lock_guard<std::mutex> lk(gNoteM);
    return gNotes;
}

GpuResult gpuSpectrogram(const GpuSpectrogramJob& job, const std::function<bool()>& cancelled) {
    if (!gpuEnabled() || gWfFailed) return GpuResult::Failed;
    thread_local SpecCtx ctx;
    tErr.clear();
    GpuResult r = ctx.init() ? spectrogram(ctx, job, cancelled) : GpuResult::Failed;
    if (r == GpuResult::Failed) {
        if (tErr.empty()) tErr = "unsupported request";
        // Out of memory once is not a reason to stop using the GPU: the next
        // request may be smaller. Any other error turns the path off.
        if (tErr.find("out of memory") == std::string::npos) {
            gWfFailed = true;
            addNote("file waterfall moved to the CPU: " + tErr);
        }
    }
    return r;
}

std::unique_ptr<GpuLiveSpectrum> GpuLiveSpectrum::create(int n, SampleFormat fmt) {
    if (!gpuEnabled() || gLiveFailed) return nullptr;
    tErr.clear();
    auto p = std::make_unique<CudaLiveSpectrum>(n, fmt);
    if (!p->init()) {
        gLiveFailed = true;
        addNote("live spectrum moved to the CPU: " + tErr);
        return nullptr;
    }
    return p;
}
