#pragma once
#include <complex>
#include <cstdint>
#include <memory>
#include <string>

enum class SampleFormat : int { CF32 = 0, CI16, CI8, CU8, CF64, CI32, Count };

const char* formatName(SampleFormat f);
int formatBytesPerSample(SampleFormat f);  // bytes per complex sample

// How to interpret the raw bytes of a file. Lives outside IqFile so that the
// UI can change it at any time and workers get a consistent snapshot.
struct FormatParams {
    SampleFormat fmt = SampleFormat::CF32;
    int64_t headerBytes = 0;
    bool swapIQ = false;
    bool bigEndian = false;
    bool operator==(const FormatParams&) const = default;
};

// Read-only memory-mapped IQ recording. Immutable after open(), so it can be
// shared between threads; the mapping lives until the last reference drops.
class IqFile {
public:
    static std::shared_ptr<IqFile> open(const std::string& path, std::string& err);
    ~IqFile();
    IqFile(const IqFile&) = delete;
    IqFile& operator=(const IqFile&) = delete;

    const std::string& path() const { return path_; }
    uint64_t sizeBytes() const { return size_; }
    int64_t numSamples(const FormatParams& fp) const;

    // Reads `count` complex samples starting at sample `start`, normalized to
    // roughly [-1, 1]. Samples outside the file are zero-filled.
    void read(const FormatParams& fp, int64_t start, int64_t count, std::complex<float>* out) const;

private:
    IqFile() = default;
    std::string path_;
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};

// Metadata inferred from a SigMF sidecar, gqrx-style filename, or extension.
struct FileMetaGuess {
    std::string dataPath;  // file to actually open (e.g. .sigmf-meta -> .sigmf-data)
    bool haveFmt = false;
    SampleFormat fmt = SampleFormat::CF32;
    bool bigEndian = false;
    bool haveRate = false;
    double rate = 0;
    bool haveFc = false;
    double fc = 0;
    std::string source;  // human-readable note of where the guess came from
};
FileMetaGuess guessMeta(const std::string& path);
bool parseFormatName(const std::string& s, SampleFormat& fmt, bool& bigEndian);
