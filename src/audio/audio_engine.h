#pragma once
// Audio playback sessions. A DSP thread reads IQ from a file or from a live
// SampleRing, demodulates one channel with AudioDemod, and feeds an AudioRing
// that the sound card drains. It can also write the audio to a WAV file.
// WavExportJob demodulates a file range to WAV faster than real time.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio/audio_dsp.h"
#include "audio/audio_out.h"
#include "dsp/engine.h"
#include "common/sample_ring.h"
#include "io/wav_writer.h"

constexpr int kAudioRate = 48000;

enum class AudioOutput { Require, Optional, None };

class AudioEngine {
public:
    struct Stats {
        bool running = false, live = false, paused = false, finished = false;
        bool hasOutput = false;  // a sound device is open
        std::string device, error;
        double pos = 0;          // file: input sample being heard now
        float chanDb = -200;     // channel power, dBFS
        bool squelchOpen = true;
        float peakDb = -200;     // output peak since the previous stats() call
        int64_t underruns = 0, resyncs = 0;
        double latency = 0;      // seconds from the DSP thread to the speaker
        double ppm = 0;          // live: drift correction
        double load = 0;         // DSP time / audio time
        double ifRate = 0;
        bool recording = false;  // WAV
        std::string wavPath, wavError;
        double wavSeconds = 0;
    };

    explicit AudioEngine(std::function<void()> wake = nullptr);
    ~AudioEngine();

    // Plays samples [from, s1) of a file, then [s0, s1) again if `loop`.
    bool startFile(const Source& src, int64_t s0, int64_t s1, int64_t from, bool loop, const AudioParams& p,
                   const std::string& device, std::string& err);
    // Plays a live stream. With AudioOutput::Optional the session also runs
    // without a sound device, so that it can record to WAV; None never plays.
    bool startLive(std::shared_ptr<SampleRing> ring, SampleFormat fmt, double fs, const AudioParams& p,
                   const std::string& device, AudioOutput output, std::string& err);
    void stop();
    bool running() const { return th_.joinable(); }
    bool isLive() const { return live_; }
    const std::shared_ptr<SampleRing>& liveRing() const { return liveRing_; }
    const Source& fileSource() const { return src_; }

    void setParams(const AudioParams& p);
    void setVolume(double db, bool mute);
    void setPaused(bool p);
    void setLoop(bool loop);
    void seek(int64_t sample);
    // Live: samples before this ring position belong to an old tuning.
    void noteFresh(int64_t pos) { fresh_ = pos; }

    bool startWav(const std::string& path, std::string& err);
    void stopWav();

    Stats stats();

private:
    bool startCommon(const AudioParams& p, double fs, const std::string& device, AudioOutput output, std::string& err);
    void runFile();
    void runLive();
    void produce(AudioDemod& d, const cf* in, size_t n, double inputEnd);
    double heardPos();

    std::function<void()> wake_;
    bool live_ = false;
    Source src_;
    std::shared_ptr<SampleRing> liveRing_;
    SampleFormat liveFmt_ = SampleFormat::CI16;
    double fs_ = 1;

    std::shared_ptr<AudioRing> ring_;
    AudioOut out_;
    size_t target_ = 0;  // samples to keep buffered

    std::thread th_;
    std::atomic<bool> quit_{false};
    std::mutex m_;  // guards everything below
    std::condition_variable cv_;
    AudioParams params_;
    uint64_t paramsGen_ = 0;
    bool paused_ = false, loop_ = false, finished_ = false;
    int64_t s0_ = 0, s1_ = 0, seekTo_ = -1;
    std::deque<std::pair<int64_t, double>> map_;  // (audio ring position, input sample) for the playhead
    std::string error_;
    float chanDb_ = -200;
    bool sqOpen_ = true;
    int64_t resyncs_ = 0;
    double ppm_ = 0, ifRate_ = 0;
    double busy_ = 0, produced_ = 0, load_ = 0;
    std::atomic<int64_t> fresh_{0};

    std::mutex wavM_;
    WavWriter wav_;
    std::string wavError_;
    std::vector<float> audio_;
};

struct WavExportConfig {
    Source src;
    int64_t s0 = 0, s1 = 0;
    AudioParams params;
    std::string path;
};

class WavExportJob {
public:
    struct Status {
        bool active = false;
        double progress = 0;
        std::string path, error;
        double seconds = 0;
    };
    ~WavExportJob() { cancel(); }
    bool start(const WavExportConfig& cfg, std::string& err);
    void cancel();
    Status status() const;

private:
    void run();
    WavExportConfig cfg_;
    WavWriter wav_;
    std::thread th_;
    std::atomic<bool> active_{false}, cancel_{false};
    std::atomic<double> progress_{0}, seconds_{0};
    mutable std::mutex m_;
    std::string error_;
};
