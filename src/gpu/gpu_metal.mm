// Metal GPU backend for Apple GPUs (issue #21, docs/design/apple-gpu-plan.md).
// Our kernels are Metal Shading Language, compiled at run time (no Xcode
// needed). FFTs that fit in threadgroup memory (N <= 4096 on Apple GPUs) run
// in one pass: decode, window, FFT and |X|^2 per threadgroup, so each sample
// is read once. Larger FFTs use MPSGraph between our decode and sum kernels.
// On unified memory the GPU reads the memory-mapped file in place: no
// gather, no copy. Apple GPUs have no double, so cf64 is decoded on the CPU.
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

#include "gpu/gpu_backend.h"
#include "common/log.h"

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

struct AccParams { long f0; long f1; int r0; int rowsHere; int avg; int log2n; };

// One FFT per threadgroup, in threadgroup memory: decode and window into
// bit-reversed order, radix-2 butterflies, then the fftshifted |X|^2 of FFT j
// goes to pw[j * n ...]. Same inputs as decodeWindow.
kernel void fftPower(device const uchar* raw [[buffer(0)]], device const long* offs [[buffer(1)]],
                     device const int2* valid [[buffer(2)]], device const float* win [[buffer(3)]],
                     constant DecodeParams& P [[buffer(4)]], device float* pw [[buffer(5)]],
                     device const float2* tw [[buffer(6)]], threadgroup float2* buf [[threadgroup(0)]],
                     uint j [[threadgroup_position_in_grid]], uint t [[thread_position_in_threadgroup]],
                     uint nt [[threads_per_threadgroup]]) {
    const uint n = 1u << P.log2n;
    const int2 v = valid[j];
    device const uchar* x = raw + offs[j];
    for (uint i = t; i < n; i += nt) {
        float2 s = float2(0);
        if (int(i) >= v.x && int(i) < v.y) s = decodeOne(x + long(i) * P.bps, P);
        const uint r = P.log2n ? reverse_bits(i) >> (32 - P.log2n) : 0;
        buf[r] = s * win[i];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint len = 2; len <= n; len <<= 1) {
        const uint hl = len >> 1, stp = n / len;
        for (uint b = t; b < n / 2; b += nt) {
            const uint grp = b / hl, k = b - grp * hl, i0 = grp * len + k, i1 = i0 + hl;
            const float2 w = tw[k * stp], a = buf[i0], c = buf[i1];
            const float2 cw = float2(c.x * w.x - c.y * w.y, c.x * w.y + c.y * w.x);
            buf[i0] = a + cw;
            buf[i1] = a - cw;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    device float* o = pw + long(j) * n;
    for (uint i = t; i < n; i += nt) {
        const float2 c = buf[(i + n / 2) & (n - 1)];  // fftshift
        o[i] = c.x * c.x + c.y * c.y;
    }
}

// acc[rl * n + i] += the powers of the FFTs of row r0 + rl in this chunk
// (already fftshifted, from fftPower).
kernel void accumulatePower(device const float* pw [[buffer(0)]], device float* acc [[buffer(1)]],
                            constant AccParams& P [[buffer(2)]], uint idx [[thread_position_in_grid]]) {
    const uint n = 1u << P.log2n;
    if (idx >= uint(P.rowsHere) * n) return;
    const uint rl = idx >> P.log2n, i = idx & (n - 1);
    const long row = long(P.r0) + rl;
    const long fa = max(P.f0, row * P.avg), fb = min(P.f1, (row + 1) * P.avg);
    float s = 0;
    for (long f = fa; f < fb; ++f) s += pw[((f - P.f0) << P.log2n) + i];
    acc[idx] += s;
}

// acc[rl * n + i] += |X|^2 at fftshifted bin i, summed over the FFTs of row
// r0 + rl in this chunk (global FFT index f = row * avg + a, f in [f0, f1)).
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

// Channel filter. Samples in0 + k, k < len, zero outside [lo, hi) (raw starts
// at sample lo), mixed down
// by frac cycles per sample. The phase is a 64-bit fixed-point number of
// cycles (no double on the GPU): ph = ph0 + k * inc, wrapping; ph0 and inc
// come from the host. The top 24 bits give the phase in cycles.
struct MixParams { long lo; long hi; long len; ulong ph0; ulong inc; int mix; };
kernel void decodeMix(device const uchar* raw [[buffer(0)]], constant DecodeParams& P [[buffer(1)]],
                      constant MixParams& M [[buffer(2)]], device float2* x [[buffer(3)]],
                      uint k [[thread_position_in_grid]]) {
    if (long(k) >= M.len) return;
    float2 s = float2(0);
    if (long(k) >= M.lo && long(k) < M.hi) s = decodeOne(raw + (long(k) - M.lo) * P.bps, P);
    if (M.mix) {
        const ulong ph = M.ph0 + ulong(k) * M.inc;
        const float cyc = float(ph >> 40) * (1.0f / 16777216.0f);  // [0, 1)
        const float c = cospi(-2.0f * cyc), sn = sinpi(-2.0f * cyc);
        s = float2(s.x * c - s.y * sn, s.x * sn + s.y * c);
    }
    x[k] = s;
}

// y[j] = sum over m of h[m] * x[j * D + m], j < J. The taps are copied to
// threadgroup memory when they fit (tapsShared).
struct FirParams { int taps; int decim; long outLen; int tapsShared; };
kernel void fir(device const float2* x [[buffer(0)]], device const float* h [[buffer(1)]],
                constant FirParams& F [[buffer(2)]], device float2* y [[buffer(3)]],
                threadgroup float* sh [[threadgroup(0)]], uint j [[thread_position_in_grid]],
                uint t [[thread_position_in_threadgroup]], uint nt [[threads_per_threadgroup]]) {
    if (F.tapsShared) {
        for (int m = t; m < F.taps; m += nt) sh[m] = h[m];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (long(j) >= F.outLen) return;
    device const float2* xp = x + long(j) * F.decim;
    float2 acc = float2(0);
    if (F.tapsShared)
        for (int m = 0; m < F.taps; ++m) acc += sh[m] * xp[m];
    else
        for (int m = 0; m < F.taps; ++m) acc += h[m] * xp[m];
    y[j] = acc;
}
)MSL";

struct MixParams {
    int64_t lo, hi, len;
    uint64_t ph0, inc;
    int mix;
};
struct FirParams {
    int taps, decim;
    int64_t outLen;
    int tapsShared;
};

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
    id<MTLComputePipelineState> decodeWindow, accumulate, pool, fftPower, accumulatePower, decodeMix, fir;
    std::string err;
};
Pipelines& pipelines() {
    static Pipelines p;
    static std::once_flag once;
    std::call_once(once, [] {
        @autoreleasepool {
            MTLCompileOptions* o = [MTLCompileOptions new];
            // Same float rounding as the CPU: no fast math. mathMode is in the
            // macOS 15 SDK only, so an older SDK (Xcode 15) uses fastMathEnabled.
            bool safeMath = false;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
            if (@available(macOS 15.0, *)) {
                o.mathMode = MTLMathModeSafe;
                safeMath = true;
            }
#endif
            if (!safeMath) {
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
            p.fftPower = make("fftPower");
            p.accumulatePower = make("accumulatePower");
            p.decodeMix = make("decodeMix");
            p.fir = make("fir");
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
    Shared xin, xout, offs, valid, win, acc, lin, decoded, tw;
    int twN = 0;
    GraphCache graphs;
};

// Twiddles e^(-2 pi i k / n), k < n / 2, for fftPower (computed in double).
bool twiddles(Shared& tw, int& twN, int n) {
    if (twN == n) return true;
    if (!tw.reserve((size_t)std::max(1, n / 2) * 8)) return false;
    float* t = tw.ptr<float>();
    for (int k = 0; k < n / 2; ++k) {
        t[2 * k] = (float)std::cos(-2 * M_PI * k / n);
        t[2 * k + 1] = (float)std::sin(-2 * M_PI * k / n);
    }
    twN = n;
    return true;
}

// One-pass FFTs fit when an FFT of n complex floats fits in threadgroup memory.
bool onePass(int n) { return (NSUInteger)n * 8 <= g_device.maxThreadgroupMemoryLength; }

// Encodes m FFTs of size n (their byte offsets and valid ranges in c.offs and
// c.valid, the window in c.win) and adds their fftshifted powers to the rows
// of c.acc that `ap` names. One pass when the FFT fits in threadgroup memory,
// else decode, MPSGraph FFT (g, tin -> tout) and sum. Returns an open compute
// encoder for the caller's next step.
id<MTLComputeCommandEncoder> encodeFftSum(SpecContext& c, Pipelines& pl, MPSCommandBuffer* cb, id<MTLBuffer> raw,
                                          const DecodeParams& dp, const AccParams& ap, int n, int m, FftGraph* g,
                                          MPSGraphTensorData* tin, MPSGraphTensorData* tout) API_AVAILABLE(macos(14.0)) {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setBuffer:raw offset:0 atIndex:0];
    [e setBuffer:c.offs.buf offset:0 atIndex:1];
    [e setBuffer:c.valid.buf offset:0 atIndex:2];
    [e setBuffer:c.win.buf offset:0 atIndex:3];
    [e setBytes:&dp length:sizeof dp atIndex:4];
    if (!g) {
        // One pass: the powers of each FFT into xout, then summed per row.
        [e setBuffer:c.xout.buf offset:0 atIndex:5];
        [e setBuffer:c.tw.buf offset:0 atIndex:6];
        [e setComputePipelineState:pl.fftPower];
        [e setThreadgroupMemoryLength:(NSUInteger)n * 8 atIndex:0];
        const NSUInteger tg = std::max<NSUInteger>(1, std::min<NSUInteger>(pl.fftPower.maxTotalThreadsPerThreadgroup, n / 2));
        [e dispatchThreadgroups:MTLSizeMake(m, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
        [e setBuffer:c.xout.buf offset:0 atIndex:0];
        [e setBuffer:c.acc.buf offset:0 atIndex:1];
        [e setBytes:&ap length:sizeof ap atIndex:2];
        dispatch(e, pl.accumulatePower, (NSUInteger)ap.rowsHere * n);
        return e;
    }
    [e setBuffer:c.xin.buf offset:0 atIndex:5];
    dispatch(e, pl.decodeWindow, (NSUInteger)m * n);
    [e endEncoding];
    // The batch may be larger than m; the extra transforms are ignored.
    [g->graph encodeToCommandBuffer:cb feeds:@{g->in : tin} targetOperations:nil resultsDictionary:@{g->out : tout}
                executionDescriptor:nil];
    e = [cb computeCommandEncoder];
    [e setBuffer:c.xout.buf offset:0 atIndex:0];
    [e setBuffer:c.acc.buf offset:0 atIndex:1];
    [e setBytes:&ap length:sizeof ap atIndex:2];
    dispatch(e, pl.accumulate, (NSUInteger)ap.rowsHere * n);
    return e;
}

// Buffers and graph for FFTs of size n in chunks of up to `batch`.
bool prepareFft(SpecContext& c, int n, int batch, FftGraph*& g, MPSGraphTensorData* __strong& tin,
                MPSGraphTensorData* __strong& tout)
    API_AVAILABLE(macos(14.0)) {
    const bool fused = onePass(n);
    if ((!fused && !c.xin.reserve((size_t)batch * n * 8)) || !c.xout.reserve((size_t)batch * n * 8) ||
        (fused && !twiddles(c.tw, c.twN, n)) || !c.offs.reserve((size_t)batch * 8) || !c.valid.reserve((size_t)batch * 8) ||
        !c.win.reserve((size_t)n * 4))
        return false;
    g = nullptr;
    tin = tout = nil;
    if (!fused) {
        g = &c.graphs.get(n, batch);
        tin = [[MPSGraphTensorData alloc] initWithMTLBuffer:c.xin.buf shape:@[ @(batch), @(n) ]
                                                   dataType:MPSDataTypeComplexFloat32];
        tout = [[MPSGraphTensorData alloc] initWithMTLBuffer:c.xout.buf shape:@[ @(batch), @(n) ]
                                                    dataType:MPSDataTypeComplexFloat32];
    }
    return true;
}
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
bool gpuBackendImplements(GpuJob) { return true; }

// From gpu_check --bench on an M5 (docs/design/apple-gpu-plan.md §11). The CPU uses
// vDSP there, so the GPU wins only on large jobs, and by less than CUDA does.
GpuLimits gpuBackendLimits() {
    GpuLimits l;
    l.specBudget = 512e6;
    l.specMinInput = 16e6;
    l.liveMinInput = 131072;  // smaller rows (an RTL-SDR at 2.4 MS/s) are as fast on one CPU core
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

        FftGraph* g = nullptr;
        MPSGraphTensorData *tin = nil, *tout = nil;
        if (!prepareFft(c, n, batch, g, tin, tout) || !c.acc.reserve((size_t)rowsPerGroup * n * 4) ||
            !c.lin.reserve((size_t)j.rows * j.cols * 4) || (cf64 && !c.decoded.reserve((size_t)batch * n * 8))) {
            err = "out of GPU memory for the buffers";
            oom = true;
            return GpuStatus::Failed;
        }
        memcpy(c.win.ptr<float>(), j.win, (size_t)n * 4);
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
                const AccParams ap{f0, f0 + m, r0, rowsHere, j.avg, log2n};
                id<MTLComputeCommandEncoder> e = encodeFftSum(c, pl, cb, raw, dp, ap, n, m, g, tin, tout);
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
                if (j.progress) j.progress->add(m);
                f0 += m;
            }
        }
        memcpy(lin, c.lin.ptr<float>(), (size_t)j.rows * j.cols * 4);
    }
    return GpuStatus::Ok;
}
// ---------------------------------------------------------------------------
// Channel filter (dense demodulator, region export)
// ---------------------------------------------------------------------------
namespace {
struct ChanContext {
    id<MTLCommandQueue> queue = nil;
    Shared x, y, h, decoded;
};
thread_local ChanContext t_chan;
}  // namespace

GpuStatus gpuBackendChannelize(const ChanJob& j, float* y, const std::function<bool()>& cancelled, std::string& err,
                               bool& oom) {
    @autoreleasepool {
        Pipelines& pl = pipelines();
        if (!pl.err.empty()) {
            err = pl.err;
            return GpuStatus::Failed;
        }
        ChanContext& c = t_chan;
        if (!c.queue) c.queue = [g_device newCommandQueue];
        const int N = j.taps, D = j.decim;
        if (N < 1 || D < 1 || j.outLen < 1 || !c.queue) {
            err = "bad channel filter job";
            return GpuStatus::Failed;
        }
        const bool cf64 = j.fp.fmt == SampleFormat::CF64;
        const int bps = formatBytesPerSample(j.fp.fmt);
        const int64_t fileSamples = j.file->numSamples(j.fp);
        const size_t page = (size_t)getpagesize();
        const uint8_t* base = j.file->data();
        const size_t mapped = (j.file->sizeBytes() + page - 1) / page * page;
        // Outputs per chunk, so that its input ((J - 1) * D + N samples) fits.
        const int64_t chunkOut = std::min<int64_t>(j.outLen, std::max<int64_t>(1, (kChunkSamples - N) / D + 1));
        const int64_t chunkIn = (chunkOut - 1) * D + N;
        if (!c.x.reserve((size_t)chunkIn * 8) || !c.y.reserve((size_t)chunkOut * 8) || !c.h.reserve((size_t)N * 4) ||
            (cf64 && !c.decoded.reserve((size_t)chunkIn * 8))) {
            err = "out of GPU memory for the buffers";
            oom = true;
            return GpuStatus::Failed;
        }
        memcpy(c.h.ptr<float>(), j.h, (size_t)N * 4);
        const bool tapsShared = (NSUInteger)N * 4 <= g_device.maxThreadgroupMemoryLength;
        // The phase increment in 2^-64 cycles: frac reduced to [0, 1), 52 bits kept.
        const double f = j.frac - std::floor(j.frac);
        const uint64_t inc = (uint64_t)std::llround(f * 4503599627370496.0) << 12;  // 2^52

        for (int64_t j0 = 0; j0 < j.outLen; j0 += chunkOut) {
            if (cancelled()) return GpuStatus::Cancelled;
            const int64_t J = std::min(chunkOut, j.outLen - j0);
            if (j.progress) j.progress->add(J);  // counted when submitted
            const int64_t in0 = j.inStart + j0 * D, len = (J - 1) * D + N;
            const int64_t lo = std::max<int64_t>(0, -in0), hi = std::max(lo, std::min<int64_t>(len, fileSamples - in0));
            DecodeParams dp{(int)j.fp.fmt, bps, j.fp.bigEndian, j.fp.swapIQ, 0, 0};
            MixParams mp{lo, hi, len, (uint64_t)in0 * inc, inc, j.frac != 0.0};
            id<MTLBuffer> raw;
            NSUInteger rawOffset = 0;
            if (cf64) {
                // No double on the GPU: decode on the CPU (read() zero-fills outside the file).
                j.file->read(j.fp, in0, len, c.decoded.ptr<std::complex<float>>());
                raw = c.decoded.buf;
                dp = DecodeParams{0, 8, 0, 0, 0, 0};
                mp.lo = 0;
                mp.hi = len;
            } else if (hi > lo) {
                // Wrap the mapped pages of the samples inside the file; the GPU reads them in place.
                const size_t first = (size_t)j.fp.headerBytes + (size_t)(in0 + lo) * bps;
                const size_t b0 = first / page * page;
                const size_t b1 = std::min(mapped, ((size_t)j.fp.headerBytes + (size_t)(in0 + hi) * bps + page - 1) / page * page);
                raw = [g_device newBufferWithBytesNoCopy:(void*)(base + b0) length:std::max(b1 - b0, page)
                                                 options:MTLResourceStorageModeShared deallocator:nil];
                if (!raw) {
                    err = "cannot map the file for the GPU";
                    return GpuStatus::Failed;
                }
                rawOffset = (NSUInteger)(first - b0);  // sample lo of the chunk
            } else {
                raw = c.x.buf;  // nothing inside the file: decodeMix writes zeros and reads nothing
                mp.lo = mp.hi = 0;
            }
            MPSCommandBuffer* cb = [MPSCommandBuffer commandBufferFromCommandQueue:c.queue];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setBuffer:raw offset:rawOffset atIndex:0];
            [e setBytes:&dp length:sizeof dp atIndex:1];
            [e setBytes:&mp length:sizeof mp atIndex:2];
            [e setBuffer:c.x.buf offset:0 atIndex:3];
            dispatch(e, pl.decodeMix, (NSUInteger)len);
            const FirParams fpar{N, D, J, tapsShared ? 1 : 0};
            [e setBuffer:c.x.buf offset:0 atIndex:0];
            [e setBuffer:c.h.buf offset:0 atIndex:1];
            [e setBytes:&fpar length:sizeof fpar atIndex:2];
            [e setBuffer:c.y.buf offset:0 atIndex:3];
            [e setComputePipelineState:pl.fir];
            [e setThreadgroupMemoryLength:tapsShared ? (((NSUInteger)N * 4 + 15) / 16 * 16) : 16 atIndex:0];
            const NSUInteger tg = std::min<NSUInteger>(pl.fir.maxTotalThreadsPerThreadgroup, 256);
            [e dispatchThreads:MTLSizeMake((NSUInteger)J, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
            [e endEncoding];
            if (!finish(cb, err, oom)) return GpuStatus::Failed;
            memcpy(y + 2 * j0, c.y.ptr<float>(), (size_t)J * 8);
        }
    }
    return GpuStatus::Ok;
}
// ---------------------------------------------------------------------------
// Live spectrum: one row of `frames` FFTs, summed into n powers
// ---------------------------------------------------------------------------
struct GpuLiveImpl {
    SpecContext c;
    Shared raw;
};

GpuLiveImpl* gpuBackendLiveCreate() { return new GpuLiveImpl; }
void gpuBackendLiveDestroy(GpuLiveImpl* impl) { delete impl; }

GpuStatus gpuBackendLiveRun(GpuLiveImpl* L, const uint8_t* raw, const FormatParams& fp, int n, int frames, const float* win,
                            float* pw, std::string& err, bool& oom) {
    if (@available(macOS 14.0, *)) {
    } else {
        return GpuStatus::Unavailable;
    }
    if (fp.fmt == SampleFormat::CF64) return GpuStatus::Unavailable;  // no double on the GPU; devices never give cf64
    @autoreleasepool {
        Pipelines& pl = pipelines();
        if (!pl.err.empty()) {
            err = pl.err;
            return GpuStatus::Failed;
        }
        SpecContext& c = L->c;
        if (!c.queue) c.queue = [g_device newCommandQueue];
        const int log2n = log2Of(n), bps = formatBytesPerSample(fp.fmt);
        if (log2n < 0 || frames < 1 || !c.queue) {
            err = "bad live spectrum job";
            return GpuStatus::Failed;
        }
        const int64_t maxFfts = std::max<int64_t>(1, kChunkSamples / n);
        const int64_t chunk = std::min<int64_t>(frames, maxFfts);
        const int batch = batchFor(chunk, maxFfts);
        FftGraph* g = nullptr;
        MPSGraphTensorData *tin = nil, *tout = nil;
        if (!prepareFft(c, n, batch, g, tin, tout) || !c.acc.reserve((size_t)n * 4) ||
            !L->raw.reserve((size_t)frames * n * bps)) {
            err = "out of GPU memory for the buffers";
            oom = true;
            return GpuStatus::Failed;
        }
        memcpy(L->raw.ptr<uint8_t>(), raw, (size_t)frames * n * bps);
        memcpy(c.win.ptr<float>(), win, (size_t)n * 4);
        memset(c.acc.ptr<float>(), 0, (size_t)n * 4);
        const DecodeParams dp{(int)fp.fmt, bps, fp.bigEndian, fp.swapIQ, log2n, 0};
        for (int64_t f0 = 0; f0 < frames; f0 += chunk) {
            const int m = (int)std::min<int64_t>(chunk, frames - f0);
            int64_t* offs = c.offs.ptr<int64_t>();
            int32_t* valid = c.valid.ptr<int32_t>();
            for (int k = 0; k < m; ++k) {
                offs[k] = (f0 + k) * n * bps;
                valid[2 * k] = 0;
                valid[2 * k + 1] = n;
            }
            DecodeParams d = dp;
            d.m = m;
            MPSCommandBuffer* cb = [MPSCommandBuffer commandBufferFromCommandQueue:c.queue];
            // One "row" holding all the frames; this chunk adds frames [f0, f0 + m).
            const AccParams ap{f0, f0 + m, 0, 1, frames, log2n};
            id<MTLComputeCommandEncoder> e = encodeFftSum(c, pl, cb, L->raw.buf, d, ap, n, m, g, tin, tout);
            [e endEncoding];
            if (!finish(cb, err, oom)) return GpuStatus::Failed;
        }
        memcpy(pw, c.acc.ptr<float>(), (size_t)n * 4);
    }
    return GpuStatus::Ok;
}
