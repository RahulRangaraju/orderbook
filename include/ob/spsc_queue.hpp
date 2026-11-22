#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace ob {

// Lock-free single-producer / single-consumer ring buffer.
//
// Why no mutex is needed: only the producer ever writes tail_, only the
// consumer ever writes head_. Each side reads the other's index with
// `acquire` and publishes its own with `release`:
//   * producer: write the slot, then tail_.store(release)  -> the slot write
//     is visible to anyone who acquire-loads the new tail.
//   * consumer: tail_.load(acquire) then read the slot     -> guaranteed to
//     see the producer's write.
// head_ / tail_ count up forever; `& (N - 1)` maps them onto the ring.
template <typename T, std::size_t N>   // N must be a power of 2
class SpscQueue {
    static_assert((N & (N - 1)) == 0);
    alignas(64) std::atomic<std::size_t> head_{0};  // written by consumer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by producer
    alignas(64) std::array<T, N> buf_{};
public:
    bool push(const T& v) {                // producer thread only
        auto t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == N) return false; // full
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {                     // consumer thread only
        auto h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;     // empty
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    static constexpr std::size_t capacity() { return N; }
};

// Same queue with one optimization, found by benchmarking the 2-thread
// pipeline (see README, "Threading"): each side keeps a private copy of the
// other side's index and only re-reads the shared atomic when the queue
// *looks* full (producer) or empty (consumer).
//
// Why it helps: in SpscQueue every push reads head_ and every pop reads
// tail_, and those cache lines are constantly being written by the other
// core, so nearly every operation pays a cross-core cache miss. With the
// cached copies, the shared line is touched roughly once per batch instead
// of once per message.
//
// Memory ordering is unchanged: refreshing the cached copy is still an
// acquire load that pairs with the other side's release store.
template <typename T, std::size_t N>
class SpscQueueCached {
    static_assert((N & (N - 1)) == 0);
    // Consumer-owned line: head_ plus the consumer's cached view of tail_.
    alignas(64) std::atomic<std::size_t> head_{0};
    std::size_t cachedTail_ = 0;
    // Producer-owned line: tail_ plus the producer's cached view of head_.
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::size_t cachedHead_ = 0;
    alignas(64) std::array<T, N> buf_{};
public:
    bool push(const T& v) {                // producer thread only
        auto t = tail_.load(std::memory_order_relaxed);
        if (t - cachedHead_ == N) {        // looks full: refresh, then re-check
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (t - cachedHead_ == N) return false;
        }
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {                     // consumer thread only
        auto h = head_.load(std::memory_order_relaxed);
        if (h == cachedTail_) {            // looks empty: refresh, then re-check
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (h == cachedTail_) return false;
        }
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    static constexpr std::size_t capacity() { return N; }
};

// Spin-wait hint: tells the CPU we're in a busy loop (saves power, and on
// hyper-threaded cores gives the sibling thread more resources).
inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();   // same instruction as _mm_pause()
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
}  // namespace ob
