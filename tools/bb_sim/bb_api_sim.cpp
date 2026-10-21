// A simulated BB60D behind the bb_api calls that wvt uses (issue #2), so the
// BB60 backend can be built and tested without the SDK or a device. It checks
// the arguments the way the real API documents them, streams in real time
// (bbGetIQUnpacked waits for the samples), and makes a test signal:
//   - a CW tone at 100.2 MHz, -30 dBm (moves with the tuning);
//   - noise at about -100 dBm in the band;
//   - bbADCOverflow when the tone is above the reference level.
// Environment, for tests:
//   WVT_BB_SIM_NONE=1      no device attached
//   WVT_BB_SIM_UNPLUG=S    the device disconnects S seconds after streaming starts
#include "bb_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>

namespace {
constexpr int kSerial = 12345678;
constexpr double kToneHz = 100.2e6, kToneDbm = -30, kNoiseDbm = -100;

double maxBandwidth(int decimation) {
    static const double table[] = {27e6, 17.8e6, 8e6, 3.75e6, 2e6, 1e6, 500e3,
                                   250e3, 140e3, 65e3, 30e3, 15e3, 8e3, 4e3};
    int i = 0;
    while ((1 << (i + 1)) <= decimation && i < 13) ++i;
    return table[i];
}

bool present() {
    const char* e = getenv("WVT_BB_SIM_NONE");
    return !(e && *e && *e != '0');
}

using Clock = std::chrono::steady_clock;

struct Sim {
    std::mutex m;
    bool open = false, streaming = false;
    double fc = 1e9, ref = -20, bw = 27e6;
    int decimation = 1, amp = -1, atten = -1;
    bbDataType type = bbDataType32fc;
    // stream state
    Clock::time_point t0;
    int64_t delivered = 0;
    double phase = 0;
    std::mt19937 rng{1};
} g;

double rateOf(int decimation) { return 40e6 / decimation; }
}  // namespace

extern "C" {

bbStatus bbGetSerialNumberList2(int serialNumbers[BB_MAX_DEVICES], int deviceTypes[BB_MAX_DEVICES], int* deviceCount) {
    if (!serialNumbers || !deviceTypes || !deviceCount) return bbInvalidParameterErr;
    *deviceCount = 0;
    std::lock_guard<std::mutex> lk(g.m);
    // Like the real API, an open device is not listed.
    if (present() && !g.open) {
        serialNumbers[0] = kSerial;
        deviceTypes[0] = BB_DEVICE_BB60D;
        *deviceCount = 1;
    }
    return bbNoError;
}

bbStatus bbOpenDeviceBySerialNumber(int* device, int serialNumber) {
    std::lock_guard<std::mutex> lk(g.m);
    if (!present() || serialNumber != kSerial || g.open) return bbDeviceNotOpenErr;
    g.open = true;
    g.streaming = false;
    *device = 0;
    return bbNoError;
}

bbStatus bbOpenDevice(int* device) { return bbOpenDeviceBySerialNumber(device, kSerial); }

bbStatus bbCloseDevice(int device) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    g.open = g.streaming = false;
    return bbNoError;
}

bbStatus bbGetSerialNumber(int device, uint32_t* serialNumber) {
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    *serialNumber = kSerial;
    return bbNoError;
}

bbStatus bbGetDeviceType(int device, int* deviceType) {
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    *deviceType = BB_DEVICE_BB60D;
    return bbNoError;
}

bbStatus bbConfigureIO(int device, uint32_t, uint32_t) {
    return device == 0 && g.open ? bbNoError : bbDeviceInvalidErr;
}

bbStatus bbConfigureIQDataType(int device, bbDataType dataType) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    g.type = dataType;
    return bbNoError;
}

bbStatus bbConfigureIQCenter(int device, double centerFreq) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (centerFreq < BB_MIN_FREQ || centerFreq > BB_MAX_FREQ) return bbFrequencyRangeErr;
    g.fc = centerFreq;
    return bbNoError;
}

bbStatus bbConfigureRefLevel(int device, double refLevel) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (refLevel > BB_MAX_REFERENCE) return bbInvalidParameterErr;
    g.ref = refLevel;
    return bbNoError;
}

bbStatus bbConfigureGainAtten(int device, int gain, int atten) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (gain < BB_AUTO_GAIN || gain > BB_MAX_GAIN || atten < BB_AUTO_ATTEN || atten > BB_MAX_ATTEN)
        return bbInvalidParameterErr;
    g.amp = gain;
    g.atten = atten;
    return bbNoError;
}

bbStatus bbConfigureIQ(int device, int downsampleFactor, double bandwidth) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (downsampleFactor < 1 || downsampleFactor > BB_MAX_DECIMATION || (downsampleFactor & (downsampleFactor - 1)))
        return bbInvalidParameterErr;
    if (bandwidth <= 0 || bandwidth > maxBandwidth(downsampleFactor) * 1.000001) return bbBandwidthErr;
    g.decimation = downsampleFactor;
    g.bw = bandwidth;
    return bbNoError;
}

bbStatus bbInitiate(int device, uint32_t mode, uint32_t flag) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (mode != BB_STREAMING || flag != BB_STREAM_IQ) return bbInvalidParameterErr;
    if (g.streaming) return bbDeviceAlreadyStreamingErr;
    g.streaming = true;
    g.t0 = Clock::now();
    g.delivered = 0;
    return bbNoError;
}

bbStatus bbAbort(int device) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    g.streaming = false;
    return bbNoError;
}

bbStatus bbQueryIQParameters(int device, double* sampleRate, double* bandwidth) {
    std::lock_guard<std::mutex> lk(g.m);
    if (device != 0 || !g.open) return bbDeviceInvalidErr;
    if (!g.streaming) return bbDeviceNotStreamingErr;
    *sampleRate = rateOf(g.decimation);
    *bandwidth = g.bw;
    return bbNoError;
}

bbStatus bbGetIQUnpacked(int device, void* iqData, int iqCount, int*, int, int purge, int* dataRemaining,
                         int* sampleLoss, int* sec, int* nano) {
    double rate, fc, ref, phase;
    int64_t from;
    Clock::time_point t0;
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (device != 0 || !g.open) return bbDeviceInvalidErr;
        if (!g.streaming) return bbDeviceNotStreamingErr;
        if (purge) {
            g.t0 = Clock::now();
            g.delivered = 0;
        }
        rate = rateOf(g.decimation);
        fc = g.fc;
        ref = g.ref;
        phase = g.phase;
        from = g.delivered;
        t0 = g.t0;
    }
    if (const char* e = getenv("WVT_BB_SIM_UNPLUG")) {
        const double after = atof(e);
        if (after > 0 && from / rate >= after) return bbDeviceConnectionErr;
    }
    // Real time: wait until the device would have made these samples.
    std::this_thread::sleep_until(t0 + std::chrono::duration<double>((from + iqCount) / rate));

    // The tone, if it is in the band, and noise; full scale is the reference level.
    const double offset = kToneHz - fc;
    const bool inBand = std::fabs(offset) < 0.5 * rate;
    const double toneAmp = inBand ? std::pow(10.0, (kToneDbm - ref) / 20) : 0;
    const double noiseAmp = std::pow(10.0, (kNoiseDbm - ref) / 20);
    const double dphi = 2 * M_PI * offset / rate;
    std::normal_distribution<float> n(0.f, (float)(noiseAmp / std::sqrt(2.0)));
    bool clipped = false;
    for (int i = 0; i < iqCount; ++i) {
        double re = toneAmp * std::cos(phase) + n(g.rng), im = toneAmp * std::sin(phase) + n(g.rng);
        phase += dphi;
        if (std::fabs(re) > 1 || std::fabs(im) > 1) {
            clipped = true;
            re = std::clamp(re, -1.0, 1.0);
            im = std::clamp(im, -1.0, 1.0);
        }
        if (g.type == bbDataType16sc) {
            auto* p = (int16_t*)iqData;
            p[2 * i] = (int16_t)std::lround(re * 32767);
            p[2 * i + 1] = (int16_t)std::lround(im * 32767);
        } else {
            auto* p = (float*)iqData;
            p[2 * i] = (float)re;
            p[2 * i + 1] = (float)im;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.phase = std::fmod(phase, 2 * M_PI);
        g.delivered = from + iqCount;
    }
    if (dataRemaining) *dataRemaining = 0;
    if (sampleLoss) *sampleLoss = BB_FALSE;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    if (sec) *sec = (int)std::chrono::duration_cast<std::chrono::seconds>(now).count();
    if (nano) *nano = (int)(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000);
    return clipped || (inBand && kToneDbm > ref) ? bbADCOverflow : bbNoError;
}

const char* bbGetErrorString(bbStatus status) {
    switch (status) {
        case bbNoError: return "No error";
        case bbADCOverflow: return "ADC overflow";
        case bbFrequencyRangeErr: return "Frequency outside the range of the device";
        case bbBandwidthErr: return "Bandwidth too wide for this decimation";
        case bbDeviceConnectionErr: return "Device disconnected";
        case bbDeviceInvalidErr: return "Invalid device";
        case bbDeviceAlreadyStreamingErr: return "Device already streaming";
        case bbInvalidParameterErr: return "Invalid parameter";
        case bbDeviceNotStreamingErr: return "Device not streaming";
        case bbDeviceNotOpenErr: return "Unable to open device";
        default: return "Unknown status";
    }
}

}  // extern "C"
