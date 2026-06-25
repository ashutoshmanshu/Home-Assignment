#ifndef MATCHING_ENGINE_ID_INDEX_H
#define MATCHING_ENGINE_ID_INDEX_H

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace me {

// Open-addressing hash map from OrderId to a pool slot.
//
// Replaces std::unordered_map on the hot path: entries live in one flat array
// (no per-insert node allocation, far better cache behaviour), and linear
// probing keeps lookups in adjacent cache lines. Deletion uses Knuth's
// backward-shift so there are no tombstones to degrade probe lengths over time.
//
// OrderId 0 is reserved as the empty marker; the protocol guarantees positive
// ids, and the parser rejects id 0, so this is safe.
class IdIndex {
public:
    explicit IdIndex(std::size_t initialCapacityPow2 = 1u << 16) {
        std::size_t cap = 2;
        while (cap < initialCapacityPow2) cap <<= 1;
        table_.assign(cap, Entry{});
        mask_ = cap - 1;
    }

    // Returns the slot for `id`, or kNoSlot if absent.
    Slot find(OrderId id) const {
        std::size_t i = home(id);
        while (table_[i].id != 0) {
            if (table_[i].id == id) return table_[i].slot;
            i = (i + 1) & mask_;
        }
        return kNoSlot;
    }

    // Insert a new id (must not already be present).
    void insert(OrderId id, Slot slot) {
        if ((count_ + 1) * 10 >= (mask_ + 1) * 7) grow(); // keep load factor < 0.7
        std::size_t i = home(id);
        while (table_[i].id != 0) i = (i + 1) & mask_;
        table_[i] = Entry{id, slot};
        ++count_;
    }

    // Remove `id`; returns false if it was not present.
    bool erase(OrderId id) {
        std::size_t i = home(id);
        while (table_[i].id != 0 && table_[i].id != id) i = (i + 1) & mask_;
        if (table_[i].id == 0) return false;

        // Backward-shift deletion: close the gap so probe sequences stay intact.
        std::size_t hole = i;
        table_[hole].id = 0;
        std::size_t k = hole;
        while (true) {
            k = (k + 1) & mask_;
            if (table_[k].id == 0) break;
            const std::size_t h = home(table_[k].id);
            if (!inSegment(h, hole, k)) {
                table_[hole] = table_[k];
                table_[k].id = 0;
                hole = k;
            }
        }
        --count_;
        return true;
    }

    std::size_t size() const { return count_; }

private:
    struct Entry {
        OrderId id = 0; // 0 == empty
        Slot slot = kNoSlot;
    };

    std::size_t home(OrderId id) const { return mix(id) & mask_; }

    // splitmix64 finalizer: cheap, strong avalanche so clustered ids spread out.
    static std::uint64_t mix(std::uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Is h within the cyclic interval (hole, k]? Used to decide if an element
    // probed past the hole may be shifted back to fill it.
    bool inSegment(std::size_t h, std::size_t hole, std::size_t k) const {
        return hole <= k ? (hole < h && h <= k) : (h > hole || h <= k);
    }

    void grow() {
        std::vector<Entry> old = std::move(table_);
        const std::size_t cap = (mask_ + 1) * 2;
        table_.assign(cap, Entry{});
        mask_ = cap - 1;
        count_ = 0;
        for (const Entry& e : old)
            if (e.id != 0) insert(e.id, e.slot);
    }

    std::vector<Entry> table_;
    std::size_t mask_ = 0;
    std::size_t count_ = 0;
};

} // namespace me

#endif // MATCHING_ENGINE_ID_INDEX_H
