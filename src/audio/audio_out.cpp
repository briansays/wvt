#include "audio/audio_out.h"

#include <cmath>
#include <mutex>

#ifdef WVT_HAVE_AUDIO
#include "miniaudio.h"

namespace {
// One context for the process, created on first use.
ma_context* sharedContext(std::string& err) {
    static std::mutex m;
    static ma_context ctx;
    static bool ok = false, tried = false;
    static std::string initErr;
    std::lock_guard<std::mutex> lk(m);
    if (!tried) {
        tried = true;
        ma_result r = ma_context_init(nullptr, 0, nullptr, &ctx);
        ok = r == MA_SUCCESS;
        if (!ok) initErr = std::string("no audio backend available (") + ma_result_description(r) + ")";
    }
    if (!ok) err = initErr;
    return ok ? &ctx : nullptr;
}
}  // namespace

struct AudioOut::Impl {
    ma_device dev{};
    bool open = false;
    std::shared_ptr<AudioRing> ring;
    std::string name;
    std::atomic<float> volume{1.f};
    std::atomic<bool> mute{false}, paused{false}, draining{false}, priming{true};
    std::atomic<size_t> prime{0};
    std::atomic<int64_t> underruns{0};
    std::atomic<float> peak{0.f};
    float gain = 0.f;  // ramps towards the volume to avoid clicks

    static void callback(ma_device* d, void* out, const void*, ma_uint32 frames) {
        auto* s = (Impl*)d->pUserData;
        float* o = (float*)out;
        const ma_uint32 ch = d->playback.channels;
        size_t n = 0;
        if (!s->paused.load()) {
            if (s->priming.load() && s->ring->fill() >= std::max<size_t>(1, s->prime.load())) s->priming = false;
            if (!s->priming.load()) {
                n = s->ring->pop(o, frames);  // mono, expanded below
                if (n < frames && !s->draining.load()) {
                    ++s->underruns;
                    s->priming = true;
                }
            }
        }
        const float target = s->mute.load() ? 0.f : s->volume.load();
        const float step = (target - s->gain) / std::max<ma_uint32>(1, frames);
        float pk = 0;
        for (ma_uint32 i = frames; i-- > 0;) {
            float v = i < n ? o[i] : 0.f;
            const float g = s->gain + step * (float)i;
            v = std::fmax(-1.f, std::fmin(1.f, v * g));
            pk = std::fmax(pk, std::fabs(v));
            for (ma_uint32 c = 0; c < ch; ++c) o[i * ch + c] = v;
        }
        s->gain = target;
        float prev = s->peak.load();
        while (pk > prev && !s->peak.compare_exchange_weak(prev, pk)) {}
    }
};

bool audioOutputBuilt() { return true; }

std::vector<AudioDeviceInfo> listAudioDevices(std::string& err) {
    std::vector<AudioDeviceInfo> v;
    ma_context* ctx = sharedContext(err);
    if (!ctx) return v;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    ma_result r = ma_context_get_devices(ctx, &infos, &count, nullptr, nullptr);
    if (r != MA_SUCCESS) {
        err = std::string("cannot list audio devices (") + ma_result_description(r) + ")";
        return v;
    }
    for (ma_uint32 i = 0; i < count; ++i) v.push_back({infos[i].name, infos[i].isDefault != 0});
    return v;
}

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}
AudioOut::~AudioOut() { close(); }

bool AudioOut::open(const std::string& device, int rate, std::shared_ptr<AudioRing> ring, std::string& err) {
    close();
    ma_context* ctx = sharedContext(err);
    if (!ctx) return false;
    ma_device_id id{};
    bool haveId = false;
    if (!device.empty()) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS)
            for (ma_uint32 i = 0; i < count && !haveId; ++i)
                if (device == infos[i].name) {
                    id = infos[i].id;
                    haveId = true;
                }
        if (!haveId) {
            err = "audio device '" + device + "' not found";
            return false;
        }
    }
    impl_->ring = std::move(ring);
    impl_->priming = true;
    impl_->underruns = 0;
    impl_->gain = 0;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = haveId ? &id : nullptr;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = (ma_uint32)rate;
    cfg.dataCallback = Impl::callback;
    cfg.pUserData = impl_.get();
    cfg.noClip = MA_TRUE;  // the callback clips
    ma_result r = ma_device_init(ctx, &cfg, &impl_->dev);
    if (r != MA_SUCCESS) {
        err = std::string("cannot open audio device (") + ma_result_description(r) + ")";
        return false;
    }
    r = ma_device_start(&impl_->dev);
    if (r != MA_SUCCESS) {
        ma_device_uninit(&impl_->dev);
        err = std::string("cannot start audio device (") + ma_result_description(r) + ")";
        return false;
    }
    impl_->open = true;
    impl_->name = impl_->dev.playback.name;
    return true;
}

void AudioOut::close() {
    if (!impl_->open) return;
    ma_device_uninit(&impl_->dev);
    impl_->open = false;
}

bool AudioOut::isOpen() const { return impl_->open; }
std::string AudioOut::name() const { return impl_->name; }

double AudioOut::latency() const {
    if (!impl_->open) return 0;
    const auto& p = impl_->dev.playback;
    return p.internalSampleRate ? (double)p.internalPeriodSizeInFrames * p.internalPeriods / p.internalSampleRate : 0;
}

#else  // built without audio output

struct AudioOut::Impl {};
bool audioOutputBuilt() { return false; }
std::vector<AudioDeviceInfo> listAudioDevices(std::string& err) {
    err = "built without audio output (WVT_WITH_AUDIO=OFF)";
    return {};
}
AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}
AudioOut::~AudioOut() = default;
bool AudioOut::open(const std::string&, int, std::shared_ptr<AudioRing>, std::string& err) {
    err = "built without audio output (WVT_WITH_AUDIO=OFF)";
    return false;
}
void AudioOut::close() {}
bool AudioOut::isOpen() const { return false; }
std::string AudioOut::name() const { return {}; }
double AudioOut::latency() const { return 0; }
#define WVT_AUDIO_STUB
#endif

#ifndef WVT_AUDIO_STUB
void AudioOut::setVolume(float linear) { impl_->volume = linear; }
void AudioOut::setMute(bool m) { impl_->mute = m; }
void AudioOut::setPaused(bool p) { impl_->paused = p; }
void AudioOut::setPrime(size_t samples) { impl_->prime = samples; }
void AudioOut::reprime() { impl_->priming = true; }
void AudioOut::setDraining(bool d) { impl_->draining = d; }
int64_t AudioOut::underruns() const { return impl_->underruns.load(); }
float AudioOut::takePeak() { return impl_->peak.exchange(0.f); }
#else
void AudioOut::setVolume(float) {}
void AudioOut::setMute(bool) {}
void AudioOut::setPaused(bool) {}
void AudioOut::setPrime(size_t) {}
void AudioOut::reprime() {}
void AudioOut::setDraining(bool) {}
int64_t AudioOut::underruns() const { return 0; }
float AudioOut::takePeak() { return 0.f; }
#endif
