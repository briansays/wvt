#pragma once
// Streaming audio demodulation. A channel at an offset from the center
// frequency is shifted to 0 Hz, decimated, filtered, demodulated and
// resampled to the audio rate. Unlike computeDemod() (which feeds the plots),
// every stage keeps its state between blocks, so consecutive blocks give
// continuous audio.
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp.h"

enum class AudioMode : int { AM = 0, NFM, WFM, USB, LSB, CW, Count };
const char* audioModeName(AudioMode m);
double audioModeDefaultBw(AudioMode m);

enum class Deemphasis : int { Off = 0, Us50, Us75, Count };
const char* deemphasisName(Deemphasis d);

struct AudioParams {
    int mode = (int)AudioMode::NFM;
    // Passband center relative to the center frequency, and its width. For
    // USB the carrier sits at the lower edge, for LSB at the upper edge.
    double offset = 0;
    double bw = 12.5e3;
    bool agc = true;       // AM: normalize by the carrier; SSB / CW: level the signal
    double gainDb = 0;     // after demodulation (and AGC)
    int deemph = (int)Deemphasis::Off;
    double bfo = 700;      // CW: pitch of a carrier at the passband center (Hz)
    bool squelch = false;
    double squelchDb = -60;  // channel power threshold (dBFS)
    bool operator==(const AudioParams&) const = default;
};

// FIR filter + decimator with saved history. Zero taps are skipped, so a
// half-band filter costs half its length.
template <class T>
class FirDecim {
public:
    void design(const std::vector<float>& h, int decim);
    void reset();
    void process(const T* in, size_t n, std::vector<T>& out);  // replaces out
    int decim() const { return d_; }

private:
    std::vector<int> idx_;
    std::vector<float> coef_;
    int n_ = 1, d_ = 1, skip_ = 0;
    std::vector<T> buf_;
};

// Windowed-sinc polyphase resampler for real samples. The ratio (output
// rate / input rate) can change between blocks without a glitch.
class Resampler {
public:
    void design(double ratio, double cutoff);  // cutoff in cycles per input sample
    void setRatio(double ratio) { step_ = 1.0 / ratio; }
    void reset();
    void process(const float* in, size_t n, std::vector<float>& out);  // appends to out

private:
    static constexpr int kTaps = 32, kPhases = 128;
    std::vector<float> table_;  // (kPhases + 1) * kTaps
    std::vector<float> buf_;
    double t_ = 0, step_ = 1;
};

class AudioDemod {
public:
    explicit AudioDemod(double inRate, double outRate = 48000);
    // Mode, bandwidth and de-emphasis rebuild the filters; the other fields
    // apply without a glitch.
    void setParams(const AudioParams& p);
    // Output-rate correction for clock drift: +100e-6 gives 100 ppm more output.
    void setRatioTrim(double trim);
    void reset();  // clears all filter state (after a seek or a gap)
    // Demodulates n input samples and appends the audio to out.
    void process(const cf* in, size_t n, std::vector<float>& out);

    double inRate() const { return inRate_; }
    double outRate() const { return outRate_; }
    double ifRate() const { return ifRate_; }
    float channelDb() const { return 10.f * std::log10(pwr_ + 1e-20f); }
    bool squelchOpen() const { return sqOpen_; }

private:
    void build();
    double shiftHz() const;  // USB / LSB / CW: shift from the passband center to audio

    double inRate_, outRate_;
    AudioParams p_;
    bool built_ = false;
    double ifRate_ = 0, audRate_ = 0, trim_ = 0;

    // channel
    std::complex<double> rot_{1, 0}, rotStep_{1, 0};
    int rotCount_ = 0;
    std::vector<FirDecim<cf>> halfbands_;
    FirDecim<cf> chan_;
    // demodulation
    cf prev_{0, 0};
    std::complex<double> bfoRot_{1, 0}, bfoStep_{1, 0};
    float carrier_ = 0, carA_ = 0, agcEnv_ = 0, agcAtk_ = 0, agcRel_ = 0;
    float fmScale_ = 0, deY_ = 0, deA_ = 0;
    float dcX_ = 0, dcY_ = 0, dcP_ = 0;
    float pwr_ = 0, pwrA_ = 0, sqGain_ = 0, sqA_ = 0, sqOpenThr_ = 0, sqCloseThr_ = 0;
    float gainLin_ = 1;
    bool sqOpen_ = false;
    // audio
    std::vector<FirDecim<float>> audioHalfbands_;
    Resampler rs_;
    std::vector<cf> w0_, w1_;
    std::vector<float> a0_, a1_;
};
