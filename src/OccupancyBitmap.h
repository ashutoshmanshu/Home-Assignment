#ifndef MATCHING_ENGINE_OCCUPANCY_BITMAP_H
#define MATCHING_ENGINE_OCCUPANCY_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace me {

// Three-level occupancy bitmap over a fixed range of indices.
//
// It answers "which is the lowest / highest occupied index?" in O(1) using
// hardware bit-scan instructions, which is exactly what best-bid / best-ask
// discovery needs once price levels live in a flat array. Each level summarizes
// the one below it (1 bit per 64-bit word), so for a 2^21 range we descend at
// most three words deep regardless of how sparse the book is. No tree walk,
// no pointer chasing.
class OccupancyBitmap {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    explicit OccupancyBitmap(std::size_t capacity) {
        l0_.assign(words(capacity), 0);
        l1_.assign(words(l0_.size()), 0);
        l2_.assign(words(l1_.size()), 0);
    }

    void set(std::size_t i) {
        const std::size_t w0 = i >> 6;
        l0_[w0] |= bit(i);
        const std::size_t w1 = w0 >> 6;
        l1_[w1] |= bit(w0);
        l2_[w1 >> 6] |= bit(w1);
    }

    void clear(std::size_t i) {
        const std::size_t w0 = i >> 6;
        l0_[w0] &= ~bit(i);
        if (l0_[w0] != 0) return;                 // word still occupied
        const std::size_t w1 = w0 >> 6;
        l1_[w1] &= ~bit(w0);
        if (l1_[w1] != 0) return;
        l2_[w1 >> 6] &= ~bit(w1);
    }

    bool test(std::size_t i) const { return (l0_[i >> 6] >> (i & 63)) & 1u; }

    // Lowest occupied index, or npos if empty.
    std::size_t findLowest() const {
        for (std::size_t w2 = 0; w2 < l2_.size(); ++w2) {
            if (l2_[w2] == 0) continue;
            const std::size_t w1 = (w2 << 6) + ctz(l2_[w2]);
            const std::size_t w0 = (w1 << 6) + ctz(l1_[w1]);
            return (w0 << 6) + ctz(l0_[w0]);
        }
        return npos;
    }

    // Highest occupied index, or npos if empty.
    std::size_t findHighest() const {
        for (std::size_t k = l2_.size(); k-- > 0;) {
            if (l2_[k] == 0) continue;
            const std::size_t w1 = (k << 6) + msb(l2_[k]);
            const std::size_t w0 = (w1 << 6) + msb(l1_[w1]);
            return (w0 << 6) + msb(l0_[w0]);
        }
        return npos;
    }

private:
    static std::size_t words(std::size_t bits) { return (bits + 63) / 64; }
    static std::uint64_t bit(std::size_t i) { return std::uint64_t(1) << (i & 63); }
    static std::size_t ctz(std::uint64_t x) { return static_cast<std::size_t>(__builtin_ctzll(x)); }
    static std::size_t msb(std::uint64_t x) { return 63 - static_cast<std::size_t>(__builtin_clzll(x)); }

    std::vector<std::uint64_t> l0_; // 1 bit per index
    std::vector<std::uint64_t> l1_; // 1 bit per l0_ word
    std::vector<std::uint64_t> l2_; // 1 bit per l1_ word
};

} // namespace me

#endif // MATCHING_ENGINE_OCCUPANCY_BITMAP_H
