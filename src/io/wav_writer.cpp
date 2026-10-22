#include "io/wav_writer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "io/recorder.h"

namespace {
void put16(unsigned char* p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xff; }

void header(unsigned char* h, int rate, uint32_t dataBytes) {
    std::copy_n("RIFF", 4, h);
    put32(h + 4, 36 + dataBytes);
    std::copy_n("WAVEfmt ", 8, h + 8);
    put32(h + 16, 16);
    put16(h + 20, 1);  // PCM
    put16(h + 22, 1);  // mono
    put32(h + 24, rate);
    put32(h + 28, rate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    std::copy_n("data", 4, h + 36);
    put32(h + 40, dataBytes);
}
}  // namespace

bool WavWriter::open(const std::string& path, int rate, std::string& err) {
    close();
    size_t sl = path.find_last_of('/');
    if (sl != std::string::npos && sl > 0 && !makeDirs(path.substr(0, sl), err)) return false;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        err = "cannot create '" + path + "': " + errnoText(errno);
        return false;
    }
    f_ = fdopen(fd, "wb");
    if (!f_) {
        err = "cannot open '" + path + "': " + errnoText(errno);
        ::close(fd);
        return false;
    }
    path_ = path;
    rate_ = rate;
    samples_ = sinceHeader_ = 0;
    unsigned char h[44];
    header(h, rate, 0);
    if (fwrite(h, 1, 44, f_) != 44) {
        err = "cannot write '" + path + "': " + errnoText(errno);
        close();
        return false;
    }
    return true;
}

bool WavWriter::write(const float* s, size_t n, std::string& err) {
    if (!f_) return false;
    // 16-bit data is limited to 2 GB per RIFF size field; stop well before.
    if ((samples_ + (int64_t)n) * 2 > 0x7fff0000LL) {
        err = "WAV file size limit reached";
        return false;
    }
    std::vector<unsigned char> b(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float x = std::isfinite(s[i]) ? std::fmax(-1.f, std::fmin(1.f, s[i])) : 0.f;
        put16(&b[2 * i], (unsigned)(int16_t)std::lrint(x * 32767.f) & 0xffff);
    }
    if (fwrite(b.data(), 1, b.size(), f_) != b.size()) {
        err = "cannot write '" + path_ + "': " + errnoText(errno);
        return false;
    }
    samples_ += (int64_t)n;
    sinceHeader_ += (int64_t)n;
    if (sinceHeader_ >= rate_) patchHeader();
    return true;
}

void WavWriter::patchHeader() {
    if (!f_) return;
    sinceHeader_ = 0;
    unsigned char h[44];
    header(h, rate_, (uint32_t)(samples_ * 2));
    long pos = ftell(f_);
    fseek(f_, 0, SEEK_SET);
    fwrite(h, 1, 44, f_);
    fseek(f_, pos, SEEK_SET);
    fflush(f_);
}

void WavWriter::close() {
    if (!f_) return;
    patchHeader();
    fclose(f_);
    f_ = nullptr;
}

std::string wavPath(const std::string& prefix, double freqHz, const char* mode) {
    time_t now = time(nullptr);
    struct tm tm{};
    gmtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    char f[48];
    snprintf(f, sizeof f, "%.6f", freqHz / 1e6);
    std::string fs = f;
    while (fs.size() > 1 && fs.back() == '0') fs.pop_back();
    if (fs.back() == '.') fs.pop_back();
    return expandHome(prefix.empty() ? "wvt-audio" : prefix) + "_" + stamp + "_" + fs + "MHz_" + mode + ".wav";
}
