#pragma once
// Mono 16-bit PCM WAV file writer. The header sizes are rewritten about once a
// second, so a file cut short (crash, power loss) still opens.
#include <cstdint>
#include <cstdio>
#include <string>

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Creates `path` (and missing directories); fails if it exists.
    bool open(const std::string& path, int rate, std::string& err);
    // Samples are clipped to [-1, 1].
    bool write(const float* s, size_t n, std::string& err);
    void close();
    bool isOpen() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    int64_t samples() const { return samples_; }
    double seconds() const { return rate_ > 0 ? (double)samples_ / rate_ : 0; }

private:
    void patchHeader();

    FILE* f_ = nullptr;
    std::string path_;
    int rate_ = 0;
    int64_t samples_ = 0, sinceHeader_ = 0;
};

// "<prefix>_YYYYMMDD_HHMMSS_<freq>MHz_<mode>.wav" with ~ expanded.
std::string wavPath(const std::string& prefix, double freqHz, const char* mode);
