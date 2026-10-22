#pragma once
// Ring buffer between an SDR device thread (the single producer) and its
// consumers. Samples stay in the device's native packed format.
//
// Two kinds of reader:
//  - Lossy readers (the live spectrum) copy recent samples with read() and
//    get `false` if the producer overwrote them during the copy.
//  - One lossless gate reader (the recorder). While the gate is open the
//    producer drops an incoming block instead of overwriting samples the gate
//    has not consumed yet, and records the loss as a Gap.
//
// Positions are absolute sample counts since the stream started.
//
// The lossy read is a seqlock (issue #42). Before it copies a block, the
// producer publishes where the block will end (writing_); after it, where
// the written data ends (written_). A reader copies, then checks writing_:
// if the producer's current block reaches the samples it copied, the copy
// may be torn and the read fails.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// GCC before 14 has no __has_feature, and it parses the whole #if line, so
// the test is in steps.
#if defined(__SANITIZE_THREAD__)
#define WVT_RING_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WVT_RING_TSAN 1
#endif
#endif
#ifndef WVT_RING_TSAN
#define WVT_RING_TSAN 0
#endif

class SampleRing {
public:
    struct Gap {
        int64_t at = 0;     // ring position where samples are missing
        int64_t lost = 0;   // number of samples lost, or -1 if unknown
        std::string why;
    };

    // capacitySamples is rounded up to a power of two.
    SampleRing(int64_t capacitySamples, int bytesPerSample) : bps_(bytesPerSample) {
        cap_ = 1;
        while (cap_ < capacitySamples) cap_ <<= 1;
        buf_.resize((size_t)cap_ * bps_);
    }

    int bytesPerSample() const { return bps_; }
    int64_t capacity() const { return cap_; }
    int64_t written() const { return written_.load(std::memory_order_acquire); }

    // --- producer ------------------------------------------------------------
    void push(const uint8_t* data, int64_t n) {
        while (n > 0) {
            const int64_t m = std::min(n, cap_ / 2);
            pushBlock(data, m);
            data += m * bps_;
            n -= m;
        }
    }
    // Samples that can be pushed now without dropping any, for producers that
    // can wait (a file generator) instead of losing data.
    int64_t gateRoom() const {
        if (!gateOpen_.load(std::memory_order_acquire)) return cap_;
        return cap_ - (written() - gatePos_.load(std::memory_order_acquire));
    }
    // Samples lost before reaching the ring (e.g. a USB overflow).
    void noteGap(int64_t lost, const std::string& why) {
        if (lost > 0) droppedDevice_ += lost;
        ++deviceGapEvents_;
        addGap(written_.load(std::memory_order_relaxed), lost, why);
    }

    // --- lossy reader --------------------------------------------------------
    // Copies samples [pos, pos+n). Returns false if any of them are not (or no
    // longer) in the ring.
    bool read(int64_t pos, int64_t n, uint8_t* out) const {
        const int64_t w = written();
        if (pos < 0 || n > cap_ || pos + n > w || w - pos > cap_) return false;
        copyOut(pos, n, out);
        // If the copy saw any byte of a newer block, the fence makes that
        // block's writing_ visible here.
        std::atomic_thread_fence(std::memory_order_acquire);
        return writing_.load(std::memory_order_relaxed) - pos <= cap_;
    }

    // --- gate reader (recorder) ----------------------------------------------
    void openGate(int64_t pos) {
        gatePos_.store(pos, std::memory_order_release);
        gateOpen_.store(true, std::memory_order_release);
    }
    void advanceGate(int64_t pos) { gatePos_.store(pos, std::memory_order_release); }
    void closeGate() { gateOpen_.store(false, std::memory_order_release); }

    // Gaps from index `cursor` on, advancing it. Only gaps that can no longer
    // grow are returned (the producer has written past them), unless `all`.
    std::vector<Gap> takeGaps(size_t& cursor, bool all = false) const {
        std::lock_guard<std::mutex> lk(gapM_);
        std::vector<Gap> out;
        const int64_t w = written_.load(std::memory_order_acquire);
        while (cursor < gaps_.size() && (all || gaps_[cursor].at < w)) out.push_back(gaps_[cursor++]);
        return out;
    }
    size_t gapCount() const {
        std::lock_guard<std::mutex> lk(gapM_);
        return gaps_.size();
    }

    int64_t droppedDevice() const { return droppedDevice_.load(); }  // known count only
    int64_t deviceGapEvents() const { return deviceGapEvents_.load(); }
    int64_t droppedGate() const { return droppedGate_.load(); }

private:
    void pushBlock(const uint8_t* data, int64_t n) {
        const int64_t w = written_.load(std::memory_order_relaxed);
        if (gateOpen_.load(std::memory_order_acquire) && w + n - gatePos_.load(std::memory_order_acquire) > cap_) {
            droppedGate_ += n;
            addGap(w, n, "recorder fell behind (storage too slow?)");
            return;
        }
        // Readers must see the new end before any byte of the block.
        writing_.store(w + n, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const int64_t i = w & (cap_ - 1);
        const int64_t first = std::min(n, cap_ - i);
        storeShared(&buf_[(size_t)i * bps_], data, (size_t)first * bps_);
        if (first < n) storeShared(&buf_[0], data + first * bps_, (size_t)(n - first) * bps_);
        written_.store(w + n, std::memory_order_release);
    }

    void copyOut(int64_t pos, int64_t n, uint8_t* out) const {
        const int64_t i = pos & (cap_ - 1);
        const int64_t first = std::min(n, cap_ - i);
        loadShared(out, &buf_[(size_t)i * bps_], (size_t)first * bps_);
        if (first < n) loadShared(out + first * bps_, &buf_[0], (size_t)(n - first) * bps_);
    }

    // The data copies of a seqlock race by design: a torn copy is detected
    // and dropped. C++ has no racy memcpy yet (P1478 proposes one), so this
    // is memcpy, as in other seqlocks. Under ThreadSanitizer the copies are
    // relaxed atomics, so TSan checks the protocol around them instead of
    // reporting the intended race.
#if WVT_RING_TSAN
    static void storeShared(uint8_t* dst, const uint8_t* src, size_t n) {
        for (size_t k = 0; k < n; ++k) std::atomic_ref<uint8_t>(dst[k]).store(src[k], std::memory_order_relaxed);
    }
    static void loadShared(uint8_t* dst, const uint8_t* src, size_t n) {
        for (size_t k = 0; k < n; ++k)
            dst[k] = std::atomic_ref<uint8_t>(const_cast<uint8_t&>(src[k])).load(std::memory_order_relaxed);
    }
#else
    static void storeShared(uint8_t* dst, const uint8_t* src, size_t n) { memcpy(dst, src, n); }
    static void loadShared(uint8_t* dst, const uint8_t* src, size_t n) { memcpy(dst, src, n); }
#endif

    void addGap(int64_t at, int64_t lost, const std::string& why) {
        std::lock_guard<std::mutex> lk(gapM_);
        // Consecutive losses at the same position (the producer did not advance
        // in between) are one gap.
        if (!gaps_.empty() && gaps_.back().at == at && gaps_.back().why == why) {
            Gap& g = gaps_.back();
            g.lost = (g.lost < 0 || lost < 0) ? -1 : g.lost + lost;
            return;
        }
        gaps_.push_back({at, lost, why});
    }

    int bps_;
    int64_t cap_;
    std::vector<uint8_t> buf_;
    std::atomic<int64_t> written_{0};
    std::atomic<int64_t> writing_{0};  // the end of the block being copied in; >= written_
    std::atomic<bool> gateOpen_{false};
    std::atomic<int64_t> gatePos_{0};
    std::atomic<int64_t> droppedDevice_{0}, deviceGapEvents_{0}, droppedGate_{0};
    mutable std::mutex gapM_;
    std::vector<Gap> gaps_;
};
