// Metal GPU backend for Apple GPUs (issue #21, docs/apple-gpu-plan.md).
// Our kernels (Metal Shading Language, compiled at run time, so Xcode is not
// needed) decode, window, sum and pool; MPSGraph does the FFT. On unified
// memory the GPU reads the memory-mapped file in place: no gather, no copy.
// Apple GPUs have no double, so cf64 is decoded on the CPU.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gpu_backend.h"
#include "log.h"

namespace {
id<MTLDevice> g_device = nil;  // the device gpuInit chose

// --- Kernels -----------------------------------------------------------------
const char* kKernels = R"MSL(
#include <metal_stdlib>
using namespace metal;

// SampleFormat: CF32 = 0, CI16, CI8, CU8, CF64 (decoded on the CPU), CI32.
struct DecodeParams { int fmt; int bps; int bigEndian; int swapIQ; int log2n; int m; };

// Byte loads: the samples may sit at any byte offset (header bytes).
inline uint loadBytes(device const uchar* p, int n, bool be) {
    uint v = 0;
    for (int k = 0; k < n; ++k) v |= uint(p[be ? n - 1 - k : k]) << (8 * k);
    return v;
}

// One sample, with the rules of decodeSamples() in iq_file.cpp.
inline float2 decodeOne(device const uchar* p, constant DecodeParams& P) {
    const bool be = P.bigEndian != 0;
    float re = 0, im = 0;
    switch (P.fmt) {
        case 0: re = as_type<float>(loadBytes(p, 4, be)); im = as_type<float>(loadBytes(p + 4, 4, be)); break;
        case 1: re = float(short(loadBytes(p, 2, be))) * (1.0f / 32768.0f);
                im = float(short(loadBytes(p + 2, 2, be))) * (1.0f / 32768.0f); break;
        case 2: re = float(char(p[0])) * (1.0f / 128.0f); im = float(char(p[1])) * (1.0f / 128.0f); break;
        case 3: re = (float(p[0]) - 127.5f) * (1.0f / 127.5f); im = (float(p[1]) - 127.5f) * (1.0f / 127.5f); break;
        case 5: re = float(int(loadBytes(p, 4, be))) * (1.0f / 2147483648.0f);
                im = float(int(loadBytes(p + 4, 4, be))) * (1.0f / 2147483648.0f); break;
        default: break;
    }
    return P.swapIQ ? float2(im, re) : float2(re, im);
}

// FFT j of the chunk starts at byte offs[j] of raw; valid[j] is the part
// [x, y) inside the file, the rest is zero. Then the window.
kernel void decodeWindow(device const uchar* raw [[buffer(0)]], device const long* offs [[buffer(1)]],
                         device const int2* valid [[buffer(2)]], device const float* win [[buffer(3)]],
                         constant DecodeParams& P [[buffer(4)]], device float2* out [[buffer(5)]],
                         uint idx [[thread_position_in_grid]]) {
    const uint n = 1u << P.log2n;
    if (idx >= uint(P.m) * n) return;
    const uint j = idx >> P.log2n, i = idx & (n - 1);
    const int2 v = valid[j];
    float2 s = float2(0);
    if (int(i) >= v.x && int(i) < v.y) s = decodeOne(raw + offs[j] + long(i) * P.bps, P);
    out[idx] = s * win[i];
}

// acc[rl * n + i] += |X|^2 at fftshifted bin i, summed over the FFTs of row
// r0 + rl in this chunk (global FFT index f = row * avg + a, f in [f0, f1)).
struct AccParams { long f0; long f1; int r0; int rowsHere; int avg; int log2n; };
kernel void accumulate(device const float2* X [[buffer(0)]], device float* acc [[buffer(1)]],
                       constant AccParams& P [[buffer(2)]], uint idx [[thread_position_in_grid]]) {
    const uint n = 1u << P.log2n;
    if (idx >= uint(P.rowsHere) * n) return;
    const uint rl = idx >> P.log2n, i = idx & (n - 1);
    const long row = long(P.r0) + rl;
    const long fa = max(P.f0, row * P.avg), fb = min(P.f1, (row + 1) * P.avg);
    const uint k = (i + n / 2) & (n - 1);
    float s = 0;
    for (long f = fa; f < fb; ++f) {
        const float2 c = X[((f - P.f0) << P.log2n) + k];
        s += c.x * c.x + c.y * c.y;
    }
    acc[idx] += s;
}

// lin[rl * cols + c] = scale * max over the bins of column c.
struct PoolParams { int rowsHere; int n; int cols; float scale; };
kernel void pool(device const float* acc [[buffer(0)]], device float* lin [[buffer(1)]],
                 constant PoolParams& P [[buffer(2)]], uint idx [[thread_position_in_grid]]) {
    if (idx >= uint(P.rowsHere * P.cols)) return;
    const int rl = idx / P.cols, c = idx - rl * P.cols, per = P.n / P.cols;
    device const float* a = acc + long(rl) * P.n + long(c) * per;
    float mx = 0;
    for (int b = 0; b < per; ++b) mx = max(mx, a[b]);
    lin[idx] = mx * P.scale;
}
)MSL";

struct DecodeParams {
    int fmt, bps, bigEndian, swapIQ, log2n, m;
};
struct AccParams {
    int64_t f0, f1;
    int r0, rowsHere, avg, log2n;
};
struct PoolParams {
    int rowsHere, n, cols;
    float scale;
};

// The compiled kernels, shared by all threads. Compiled on first use (about
// 0.2 s); a failure is kept, so it is reported once per job and not retried.
struct Pipelines {
    id<MTLComputePipelineState> decodeWindow, accumulate, pool;
    std::string err;
};
Pipelines& pipelines() {
    static Pipelines p;
    static std::once_flag once;
    std::call_once(once, [] {
        @autoreleasepool {
            MTLCompileOptions* o = [MTLCompileOptions new];
            // Same float rounding as the CPU: no fast math.
            if (@available(macOS 15.0, *)) {
                o.mathMode = MTLMathModeSafe;
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                o.fastMathEnabled = NO;
#pragma clang diagnostic pop
            }
            NSError* e = nil;
            id<MTLLibrary> lib = [g_device newLibraryWithSource:@(kKernels) options:o error:&e];
            if (!lib) {
                p.err = std::string("kernel compile failed: ") + e.localizedDescription.UTF8String;
                return;
            }
            auto make = [&](const char* name) -> id<MTLComputePipelineState> {
                id<MTLFunction> f = [lib newFunctionWithName:@(name)];
                NSError* pe = nil;
                id<MTLComputePipelineState> ps = f ? [g_device newComputePipelineStateWithFunction:f error:&pe] : nil;
                if (!ps && p.err.empty()) p.err = std::string("pipeline ") + name + " failed";
                return ps;
            };
            p.decodeWindow = make("decodeWindow");
            p.accumulate = make("accumulate");
            p.pool = make("pool");
        }
    });
    return p;
}

void dispatch(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> ps, NSUInteger threads) {
    [e setComputePipelineState:ps];
    const NSUInteger w = std::min<NSUInteger>(ps.maxTotalThreadsPerThreadgroup, 256);
    [e dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(w, 1, 1)];
}

// A shared buffer that grows as needed (unified memory: the CPU writes it directly).
struct Shared {
    id<MTLBuffer> buf = nil;
    size_t cap = 0;
    bool reserve(size_t bytes) {
        if (bytes <= cap && buf) return true;
        buf = [g_device newBufferWithLength:std::max<size_t>(bytes, 16) options:MTLResourceStorageModeShared];
        cap = buf ? bytes : 0;
        return buf != nil;
    }
    template <class T>
    T* ptr() const { return (T*)buf.contents; }
};

// Batched FFT graphs, one per (N, batch), as cuFFT plans. MPSGraph compiles a
// graph the first time it runs it (10-100 ms), so they are kept.
struct FftGraph {
    MPSGraph* graph = nil;
    MPSGraphTensor *in = nil, *out = nil;
};
struct GraphCache {
    std::map<std::pair<int, int>, FftGraph> graphs;
    FftGraph& get(int n, int batch) API_AVAILABLE(macos(14.0)) {
        FftGraph& g = graphs[{n, batch}];
        if (!g.graph) {
            g.graph = [MPSGraph new];
            g.in = [g.graph placeholderWithShape:@[ @(batch), @(n) ] dataType:MPSDataTypeComplexFloat32 name:nil];
            MPSGraphFFTDescriptor* d = [MPSGraphFFTDescriptor descriptor];
            d.inverse = NO;
            d.scalingMode = MPSGraphFFTScalingModeNone;
            g.out = [g.graph fastFourierTransformWithTensor:g.in axes:@[ @1 ] descriptor:d name:nil];
        }
        return g;
    }
};

// Per-thread state: a command queue, the buffers and the graphs.
struct SpecContext {
    id<MTLCommandQueue> queue = nil;
    Shared xin, xout, offs, valid, win, acc, lin, decoded;
    GraphCache graphs;
};
thread_local SpecContext t_spec;

int log2Of(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return n > 0 && (1 << k) == n ? k : -1;
}

// The smallest power of two >= count, at most cap.
int batchFor(int64_t count, int64_t cap) {
    int64_t b = 1;
    while (b < count) b <<= 1;
    return (int)std::min(b, cap);
}

constexpr int64_t kChunkSamples = int64_t(1) << 23;  // FFT input samples per chunk (64 MB as cf32)
constexpr int64_t kMaxSpanBytes = int64_t(1) << 30;  // file bytes one chunk may wrap

std::string describe(id<MTLDevice> d) {
    char b[256];
    snprintf(b, sizeof b, "%s (%s, %.1f GB working set)", d.name.UTF8String,
             d.hasUnifiedMemory ? "unified memory" : "own memory", d.recommendedMaxWorkingSetSize / 1e9);
    return b;
}

NSArray<id<MTLDevice>>* allDevices() {
    NSArray<id<MTLDevice>>* devs = MTLCopyAllDevices();
    if (devs.count == 0) {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (d) devs = @[ d ];
    }
    return devs;
}
}  // namespace

bool gpuBackendBuilt() { return true; }
const char* gpuBackendName() { return "Metal"; }
bool gpuBackendImplements(GpuJob job) { return job == GpuJob::Spectrogram; }

// From gpu_check --bench on an M5 (docs/apple-gpu-plan.md §11). The CPU uses
// vDSP there, so the GPU wins only on large jobs, and by less than CUDA does.
GpuLimits gpuBackendLimits() {
    GpuLimits l;
    l.specBudget = 512e6;
    l.specMinInput = 16e6;
    return l;
}

void gpuBackendProbe(int device, GpuInfo& info) {
    info.built = true;
    info.backend = "Metal";
    info.available = false;
    @autoreleasepool {
        if (@available(macOS 14.0, *)) {
        } else {
            info.reason = "the Metal GPU path needs macOS 14 or later";
            return;
        }
        NSArray<id<MTLDevice>>* devs = allDevices();
        if (devs.count == 0) {
            info.reason = "no Metal GPU found";
            return;
        }
        id<MTLDevice> d = nil;
        if (device >= 0) {
            if (device >= (int)devs.count) {
                info.reason = "--gpu-device " + std::to_string(device) + ": there " +
                              (devs.count == 1 ? "is 1 GPU" : "are " + std::to_string(devs.count) + " GPUs");
                return;
            }
            d = devs[device];
        } else {
            d = MTLCreateSystemDefaultDevice();
            if (!d) d = devs[0];
        }
        if (d.recommendedMaxWorkingSetSize < kGpuMinFreeBytes) {
            info.reason = describe(d) + " has less than 1 GB of working set";
            return;
        }
        g_device = d;
        info.available = true;
        info.device = (int)[devs indexOfObject:d];
        if (info.device < 0 || info.device >= (int)devs.count) info.device = 0;
        info.name = describe(d);
        info.model = d.name.UTF8String;  // "Apple M5"
        const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
        info.version = "Metal, macOS " + std::to_string(v.majorVersion) + "." + std::to_string(v.minorVersion) + "." +
                       std::to_string(v.patchVersion);
        info.reason.clear();
    }
}

std::vector<std::string> gpuBackendList() {
    std::vector<std::string> out;
    @autoreleasepool {
        NSArray<id<MTLDevice>>* devs = allDevices();
        if (devs.count == 0) return {"none (no Metal GPU found)"};
        for (NSUInteger i = 0; i < devs.count; ++i) out.push_back(std::to_string(i) + "  " + describe(devs[i]));
    }
    return out;
}

// Runs a command buffer to the end; false (err set) when the GPU reports an error.
static bool finish(MPSCommandBuffer* cb, std::string& err, bool& oom) {
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) {
        NSError* e = cb.error ? cb.error : cb.rootCommandBuffer.error;
        err = std::string("Metal command buffer: ") + (e ? e.localizedDescription.UTF8String : "error");
        oom = e && e.code == MTLCommandBufferErrorOutOfMemory;
        return false;
    }
    return true;
}

GpuStatus gpuBackendSpectrogram(const SpecJob& j, float* lin, const std::function<bool()>& cancelled, std::string& err,
                                bool& oom) {
    if (@available(macOS 14.0, *)) {
    } else {
        return GpuStatus::Unavailable;
    }
    @autoreleasepool {
        Pipelines& pl = pipelines();
        if (!pl.err.empty()) {
            err = pl.err;
            return GpuStatus::Failed;
        }
        SpecContext& c = t_spec;
        if (!c.queue) c.queue = [g_device newCommandQueue];
        const int n = j.n, log2n = log2Of(n);
        if (j.rows <= 0 || j.cols <= 0 || log2n < 0 || !c.queue) {
            err = "bad spectrogram job";
            return GpuStatus::Failed;
        }
        const bool cf64 = j.fp.fmt == SampleFormat::CF64;
        const int bps = formatBytesPerSample(j.fp.fmt);
        const int64_t totalFfts = (int64_t)j.rows * j.avg;
        const int64_t fileSamples = j.file->numSamples(j.fp);
        const int64_t maxFfts = std::max<int64_t>(1, kChunkSamples / n);
        const int64_t chunkFfts = std::min(maxFfts, totalFfts);
        const int rowsPerGroup = j.avg <= chunkFfts ? (int)(chunkFfts / j.avg) : 1;
        const int batch = batchFor(chunkFfts, maxFfts);
        const size_t page = (size_t)getpagesize();
        const uint8_t* base = j.file->data();
        const size_t mapped = (j.file->sizeBytes() + page - 1) / page * page;

        if (!c.xin.reserve((size_t)batch * n * 8) || !c.xout.reserve((size_t)batch * n * 8) ||
            !c.offs.reserve((size_t)batch * 8) || !c.valid.reserve((size_t)batch * 8) || !c.win.reserve((size_t)n * 4) ||
            !c.acc.reserve((size_t)rowsPerGroup * n * 4) || !c.lin.reserve((size_t)j.rows * j.cols * 4) ||
            (cf64 && !c.decoded.reserve((size_t)batch * n * 8))) {
            err = "out of GPU memory for the buffers";
            oom = true;
            return GpuStatus::Failed;
        }
        memcpy(c.win.ptr<float>(), j.win, (size_t)n * 4);
        FftGraph& g = c.graphs.get(n, batch);
        MPSGraphTensorData* tin = [[MPSGraphTensorData alloc] initWithMTLBuffer:c.xin.buf shape:@[ @(batch), @(n) ]
                                                                      dataType:MPSDataTypeComplexFloat32];
        MPSGraphTensorData* tout = [[MPSGraphTensorData alloc] initWithMTLBuffer:c.xout.buf shape:@[ @(batch), @(n) ]
                                                                       dataType:MPSDataTypeComplexFloat32];
        const float scale = j.norm / j.avg;

        for (int r0 = 0; r0 < j.rows; r0 += rowsPerGroup) {
            const int rowsHere = std::min(rowsPerGroup, j.rows - r0);
            const int64_t gEnd = (int64_t)(r0 + rowsHere) * j.avg;
            memset(c.acc.ptr<float>(), 0, (size_t)rowsHere * n * 4);
            for (int64_t f0 = (int64_t)r0 * j.avg; f0 < gEnd;) {
                if (cancelled()) return GpuStatus::Cancelled;
                int m = (int)std::min(chunkFfts, gEnd - f0);
                auto startOf = [&](int64_t f) { return specFftStart(j, (int)(f / j.avg), (int)(f % j.avg)); };
                // The file bytes this chunk reads; fewer FFTs when they span too much of the file.
                auto spanOf = [&](int count, int64_t& lo, int64_t& hi) {
                    lo = std::clamp<int64_t>(startOf(f0), 0, fileSamples);
                    hi = std::clamp<int64_t>(startOf(f0 + count - 1) + n, lo, fileSamples);
                };
                int64_t lo, hi;
                spanOf(m, lo, hi);
                while (!cf64 && m > 1 && (hi - lo) * bps > kMaxSpanBytes) {
                    m /= 2;
                    spanOf(m, lo, hi);
                }
                int64_t* offs = c.offs.ptr<int64_t>();
                int32_t* valid = c.valid.ptr<int32_t>();
                id<MTLBuffer> raw;
                size_t rawShift = 0;
                DecodeParams dp{(int)j.fp.fmt, bps, j.fp.bigEndian, j.fp.swapIQ, log2n, m};
                if (cf64) {
                    // No double on the GPU: decode on the CPU (read() zero-fills outside the file).
                    auto* dec = c.decoded.ptr<std::complex<float>>();
                    for (int k = 0; k < m; ++k) {
                        j.file->read(j.fp, startOf(f0 + k), n, dec + (size_t)k * n);
                        offs[k] = (int64_t)k * n * 8;
                        valid[2 * k] = 0;
                        valid[2 * k + 1] = n;
                    }
                    raw = c.decoded.buf;
                    dp = DecodeParams{0, 8, 0, 0, log2n, m};
                } else {
                    // Wrap the mapped pages of [lo, hi) as a buffer: the GPU reads the file in place.
                    const size_t b0 = ((size_t)j.fp.headerBytes + (size_t)lo * bps) / page * page;
                    const size_t b1 = std::min(mapped, ((size_t)j.fp.headerBytes + (size_t)hi * bps + page - 1) / page * page);
                    rawShift = b0;
                    raw = [g_device newBufferWithBytesNoCopy:(void*)(base + b0) length:std::max(b1 - b0, page)
                                                     options:MTLResourceStorageModeShared deallocator:nil];
                    if (!raw) {
                        err = "cannot map the file for the GPU";
                        return GpuStatus::Failed;
                    }
                    for (int k = 0; k < m; ++k) {
                        const int64_t st = startOf(f0 + k);
                        const int64_t vlo = std::max<int64_t>(0, -st), vhi = std::min<int64_t>(n, fileSamples - st);
                        valid[2 * k] = (int32_t)(vhi > vlo ? vlo : 0);
                        valid[2 * k + 1] = (int32_t)(vhi > vlo ? vhi : 0);
                        offs[k] = (int64_t)j.fp.headerBytes + st * bps - (int64_t)rawShift;
                    }
                }
                MPSCommandBuffer* cb = [MPSCommandBuffer commandBufferFromCommandQueue:c.queue];
                id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                [e setBuffer:raw offset:0 atIndex:0];
                [e setBuffer:c.offs.buf offset:0 atIndex:1];
                [e setBuffer:c.valid.buf offset:0 atIndex:2];
                [e setBuffer:c.win.buf offset:0 atIndex:3];
                [e setBytes:&dp length:sizeof dp atIndex:4];
                [e setBuffer:c.xin.buf offset:0 atIndex:5];
                dispatch(e, pl.decodeWindow, (NSUInteger)m * n);
                [e endEncoding];
                // The batch may be larger than m; the extra transforms are ignored.
                [g.graph encodeToCommandBuffer:cb feeds:@{g.in : tin} targetOperations:nil resultsDictionary:@{g.out : tout}
                           executionDescriptor:nil];
                AccParams ap{f0, f0 + m, r0, rowsHere, j.avg, log2n};
                e = [cb computeCommandEncoder];
                [e setBuffer:c.xout.buf offset:0 atIndex:0];
                [e setBuffer:c.acc.buf offset:0 atIndex:1];
                [e setBytes:&ap length:sizeof ap atIndex:2];
                dispatch(e, pl.accumulate, (NSUInteger)rowsHere * n);
                const bool groupDone = f0 + m >= gEnd;
                if (groupDone) {
                    PoolParams pp{rowsHere, n, j.cols, scale};
                    [e setBuffer:c.acc.buf offset:0 atIndex:0];
                    [e setBuffer:c.lin.buf offset:(NSUInteger)r0 * j.cols * 4 atIndex:1];
                    [e setBytes:&pp length:sizeof pp atIndex:2];
                    dispatch(e, pl.pool, (NSUInteger)rowsHere * j.cols);
                }
                [e endEncoding];
                if (!finish(cb, err, oom)) return GpuStatus::Failed;
                f0 += m;
            }
        }
        memcpy(lin, c.lin.ptr<float>(), (size_t)j.rows * j.cols * 4);
    }
    return GpuStatus::Ok;
}
GpuStatus gpuBackendChannelize(const ChanJob&, float*, const std::function<bool()>&, std::string&, bool&) {
    return GpuStatus::Unavailable;
}
GpuLiveImpl* gpuBackendLiveCreate() { return nullptr; }
void gpuBackendLiveDestroy(GpuLiveImpl*) {}
GpuStatus gpuBackendLiveRun(GpuLiveImpl*, const uint8_t*, const FormatParams&, int, int, const float*, float*, std::string&,
                            bool&) {
    return GpuStatus::Unavailable;
}
