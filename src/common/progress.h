#pragma once
// How far a background job is (issue #75), for the status bar. The job sets
// the total amount of work and adds the work it has done, from any thread;
// the UI thread reads fraction().
#include <algorithm>
#include <atomic>
#include <cstdint>

class Progress {
public:
    void reset(int64_t total = 0) {
        done_.store(0, std::memory_order_relaxed);
        total_.store(total, std::memory_order_relaxed);
    }
    void add(int64_t n) { done_.fetch_add(n, std::memory_order_relaxed); }
    // 0 to 1, or -1 while the job has not said how much work it has.
    float fraction() const {
        const int64_t t = total_.load(std::memory_order_relaxed);
        if (t <= 0) return -1;
        return std::clamp((float)done_.load(std::memory_order_relaxed) / (float)t, 0.f, 1.f);
    }

private:
    std::atomic<int64_t> done_{0}, total_{0};
};
