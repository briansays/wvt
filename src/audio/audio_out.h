#pragma once
// Sound card output. AudioRing carries mono samples from the DSP thread to
// the device callback; AudioOut plays them through miniaudio (CoreAudio on
// macOS, PipeWire / PulseAudio / ALSA on Linux).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Single-producer / single-consumer ring of floats. Positions are absolute
// sample counts. The consumer never blocks or allocates.
class AudioRing {
public:
    explicit AudioRing(size_t capacity) {
        cap_ = 1;
        while (cap_ < capacity) cap_ <<= 1;
        buf_.assign(cap_, 0.f);
    }
    size_t capacity() const { return cap_; }
    int64_t written() const { return w_.load(std::memory_order_acquire); }
    int64_t readPos() const { return r_.load(std::memory_order_acquire); }
    size_t fill() const { return (size_t)std::max<int64_t>(0, written() - readPos()); }

    // --- producer --------------------------------------------------------
    // Pushes what fits and returns the count.
    size_t push(const float* s, size_t n) {
        const int64_t w = w_.load(std::memory_order_relaxed);
        n = std::min(n, cap_ - (size_t)std::max<int64_t>(0, w - readPos()));
        for (size_t i = 0; i < n; ++i) buf_[(size_t)(w + (int64_t)i) & (cap_ - 1)] = s[i];
        w_.store(w + (int64_t)n, std::memory_order_release);
        return n;
    }
    // The consumer discards samples before `pos` (e.g. written() to flush).
    void skipTo(int64_t pos) {
        int64_t cur = skip_.load();
        while (pos > cur && !skip_.compare_exchange_weak(cur, pos)) {}
    }

    // --- consumer --------------------------------------------------------
    size_t pop(float* out, size_t n) {
        int64_t r = r_.load(std::memory_order_relaxed);
        const int64_t w = written();
        r = std::max(r, std::min(w, skip_.load(std::memory_order_acquire)));
        n = std::min(n, (size_t)(w - r));
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(size_t)(r + (int64_t)i) & (cap_ - 1)];
        r_.store(r + (int64_t)n, std::memory_order_release);
        return n;
    }

private:
    size_t cap_;
    std::vector<float> buf_;
    std::atomic<int64_t> w_{0}, r_{0}, skip_{0};
};

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
};

bool audioOutputBuilt();  // false when built with WVT_WITH_AUDIO=OFF
std::vector<AudioDeviceInfo> listAudioDevices(std::string& err);

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Opens the named playback device ("" = system default) and starts it.
    bool open(const std::string& device, int rate, std::shared_ptr<AudioRing> ring, std::string& err);
    void close();
    bool isOpen() const;
    std::string name() const;
    double latency() const;  // device buffer, seconds

    void setVolume(float linear);
    void setMute(bool m);
    void setPaused(bool p);  // plays silence without consuming the ring
    // Silence until the ring holds `samples` again (after a start, seek or
    // underrun), so playback does not stutter while the buffer refills.
    void setPrime(size_t samples);
    void reprime();
    // At the end of a file the ring runs dry on purpose: no underrun counted.
    void setDraining(bool d);

    int64_t underruns() const;
    float takePeak();  // peak output level since the previous call

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
