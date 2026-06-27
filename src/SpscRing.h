#ifndef MATCHING_ENGINE_SPSC_RING_H
#define MATCHING_ENGINE_SPSC_RING_H

#include <atomic>
#include <cstddef>
#include <new>
#include <vector>

namespace me {

// Single-producer / single-consumer lock-free ring buffer.
//
// This is the *correct* place for lock-free code in a matching system: the
// order book itself must stay single-threaded for deterministic price-time
// priority, so concurrency is confined to the hand-off between the I/O thread
// and the matching thread. One producer pushes, one consumer pops; no mutex,
// no CAS loop, just a pair of indices published with release/acquire ordering.
//
// Design notes for low latency:
//   * Capacity is a power of two so wrap-around is a mask, not a modulo.
//   * head_ (consumer) and tail_ (producer) sit on separate cache lines to
//     avoid false sharing; otherwise each side's store would invalidate the
//     other core's cache line on every operation.
//   * Each side caches the other index and only reloads the shared atomic when
//     its cached view says "full"/"empty", cutting cross-core traffic.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacityPow2)
        : buffer_(capacityPow2), mask_(capacityPow2 - 1) {
        // Require a power of two and at least 2 slots.
        // (capacity & mask) == 0 verifies the power-of-two property.
        if (capacityPow2 < 2 || (capacityPow2 & mask_) != 0) {
            // Round up to the next power of two.
            std::size_t cap = 2;
            while (cap < capacityPow2) cap <<= 1;
            buffer_.assign(cap, T{});
            mask_ = cap - 1;
        }
    }

    // Producer side. Returns false if the ring is full.
    bool push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = tail + 1;
        if (next - headCache_ > mask_ + 1) {          // cached view says full?
            headCache_ = head_.load(std::memory_order_acquire); // resync
            if (next - headCache_ > mask_ + 1) return false;    // truly full
        }
        buffer_[tail & mask_] = value;
        tail_.store(next, std::memory_order_release); // publish the slot
        return true;
    }

    // Consumer side. Returns false if the ring is empty.
    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tailCache_) {                      // cached view says empty?
            tailCache_ = tail_.load(std::memory_order_acquire); // resync
            if (head == tailCache_) return false;               // truly empty
        }
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const { return mask_ + 1; }

private:
    static constexpr std::size_t kCacheLine = 64;

    std::vector<T> buffer_;
    std::size_t mask_;

    // Producer-owned: tail index + cached head.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t headCache_ = 0;

    // Consumer-owned: head index + cached tail.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tailCache_ = 0;
};

} // namespace me

#endif // MATCHING_ENGINE_SPSC_RING_H
