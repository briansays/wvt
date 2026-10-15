#pragma once
#include <complex>
#include <functional>
#include <memory>
#include <vector>

using cf = std::complex<float>;

// In-place iterative radix-2 complex FFT. Immutable after construction, so a
// single instance can be shared by many threads.
class FFT {
public:
    explicit FFT(int n);
    int size() const { return n_; }
    void forward(cf* data) const;

private:
    int n_;
    std::vector<cf> tw_;
    std::vector<int> rev_;
};
std::shared_ptr<const FFT> getFFT(int n);  // cached by size

enum class WindowType : int { Rect = 0, Hann, Hamming, Blackman, BlackmanHarris, FlatTop, Count };
const char* windowName(WindowType w);
std::vector<float> makeWindow(WindowType w, int n);

// Blackman-windowed sinc low-pass, unity DC gain. cutoff in cycles/sample (0..0.5).
std::vector<float> designLowpass(int taps, double cutoff);

// Runs fn(begin, end) over [0, n) split across hardware threads.
void parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk = 1);

// Centered boxcar moving average, NaN-aware (NaNs are ignored in the mean).
void movingAverage(std::vector<float>& v, int len);

inline bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }
