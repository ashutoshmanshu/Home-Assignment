#include "test_framework.h"

#include "../src/IdIndex.h"
#include "../src/OccupancyBitmap.h"
#include "../src/SpscRing.h"

#include <cstdint>
#include <thread>
#include <vector>

using namespace me;

// ===========================================================================
// OccupancyBitmap
// ===========================================================================
TEST_CASE(bitmap_empty_returns_npos) {
    OccupancyBitmap b(1024);
    CHECK(b.findLowest() == OccupancyBitmap::npos);
    CHECK(b.findHighest() == OccupancyBitmap::npos);
}

TEST_CASE(bitmap_set_test_clear) {
    OccupancyBitmap b(1u << 16);
    b.set(0);
    b.set(12345);
    b.set(65535);
    CHECK(b.test(0));
    CHECK(b.test(12345));
    CHECK(b.test(65535));
    CHECK_FALSE(b.test(7));
    CHECK_EQ(b.findLowest(), static_cast<std::size_t>(0));
    CHECK_EQ(b.findHighest(), static_cast<std::size_t>(65535));
    b.clear(0);
    CHECK_EQ(b.findLowest(), static_cast<std::size_t>(12345));
    b.clear(65535);
    CHECK_EQ(b.findHighest(), static_cast<std::size_t>(12345));
    b.clear(12345);
    CHECK(b.findLowest() == OccupancyBitmap::npos);
}

TEST_CASE(bitmap_tracks_extremes_across_many_bits) {
    OccupancyBitmap b(1u << 21);
    for (std::size_t i = 1000; i <= 100000; i += 1000) b.set(i);
    CHECK_EQ(b.findLowest(), static_cast<std::size_t>(1000));
    CHECK_EQ(b.findHighest(), static_cast<std::size_t>(100000));
    b.clear(1000);
    b.clear(100000);
    CHECK_EQ(b.findLowest(), static_cast<std::size_t>(2000));
    CHECK_EQ(b.findHighest(), static_cast<std::size_t>(99000));
}

// ===========================================================================
// IdIndex
// ===========================================================================
TEST_CASE(idindex_insert_find_erase) {
    IdIndex idx(16);
    idx.insert(1000000, 5);
    idx.insert(1000001, 6);
    idx.insert(42, 7);
    CHECK_EQ(idx.find(1000000), static_cast<Slot>(5));
    CHECK_EQ(idx.find(1000001), static_cast<Slot>(6));
    CHECK_EQ(idx.find(42), static_cast<Slot>(7));
    CHECK(idx.find(99) == kNoSlot);
    CHECK(idx.erase(1000000));
    CHECK(idx.find(1000000) == kNoSlot);
    CHECK_EQ(idx.find(1000001), static_cast<Slot>(6)); // survives backward-shift
    CHECK_EQ(idx.find(42), static_cast<Slot>(7));
    CHECK_FALSE(idx.erase(1000000));
    CHECK_EQ(idx.size(), static_cast<std::size_t>(2));
}

TEST_CASE(idindex_survives_rehash_and_many_keys) {
    IdIndex idx(16);
    const Slot n = 50000;
    for (Slot i = 0; i < n; ++i) idx.insert(1000000 + i, i);
    CHECK_EQ(idx.size(), static_cast<std::size_t>(n));
    for (Slot i = 0; i < n; ++i) CHECK_EQ(idx.find(1000000 + i), i);

    // Erase the evens; odds must remain findable (probe chains stay intact).
    for (Slot i = 0; i < n; i += 2) CHECK(idx.erase(1000000 + i));
    for (Slot i = 1; i < n; i += 2) CHECK_EQ(idx.find(1000000 + i), i);
    for (Slot i = 0; i < n; i += 2) CHECK(idx.find(1000000 + i) == kNoSlot);
}

// ===========================================================================
// SpscRing
// ===========================================================================
TEST_CASE(spscring_single_thread_fifo) {
    SpscRing<int> ring(4); // capacity 4 (all slots usable)
    int out = 0;
    CHECK_FALSE(ring.pop(out));            // empty
    CHECK(ring.push(1));
    CHECK(ring.push(2));
    CHECK(ring.push(3));
    CHECK(ring.push(4));                    // holds the full capacity
    CHECK_FALSE(ring.push(5));              // now full
    CHECK(ring.pop(out)); CHECK_EQ(out, 1);
    CHECK(ring.pop(out)); CHECK_EQ(out, 2);
    CHECK(ring.push(5));                    // room again
    CHECK(ring.pop(out)); CHECK_EQ(out, 3);
    CHECK(ring.pop(out)); CHECK_EQ(out, 4);
    CHECK(ring.pop(out)); CHECK_EQ(out, 5);
    CHECK_FALSE(ring.pop(out));
}

TEST_CASE(spscring_rounds_capacity_to_power_of_two) {
    SpscRing<int> ring(1000);
    CHECK_EQ(ring.capacity(), static_cast<std::size_t>(1024));
}

TEST_CASE(spscring_producer_consumer_preserves_order) {
    SpscRing<std::uint64_t> ring(1024);
    const std::uint64_t n = 1'000'000;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n; ++i)
            while (!ring.push(i)) { /* spin until space */ }
    });

    bool ordered = true;
    std::uint64_t received = 0;
    std::uint64_t value = 0;
    while (received < n) {
        if (ring.pop(value)) {
            if (value != received) ordered = false;
            ++received;
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK_EQ(received, n);
}
