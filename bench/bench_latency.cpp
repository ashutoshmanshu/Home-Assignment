// Per-operation latency harness for the matching engine.
//
// It measures the three paths the assignment calls out — an add that matches,
// an add that rests (no match), and a cancel — and reports both:
//   * amortized ns/op  : a clean timer-free loop (total wall time / N), the
//                        truest per-op cost.
//   * p50/p99/p99.9/max: a separate per-op timed loop, showing the distribution
//                        shape (in HFT the tail matters more than the mean).
// The per-op figures include the timer's own ~20-40 ns/call cost, so treat them
// as an upper bound. Output goes to a do-nothing sink so we time the engine,
// not formatting or I/O.
//
// Caveats (see README): user space, no pinned/isolated cores, no huge pages, no
// kernel-bypass, and a virtualized clock. These numbers show the algorithm's
// shape, not production hardware latency.

#include "../src/MessageSink.h"
#include "../src/OrderBook.h"
#include "../src/Price.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace me;
using Clock = std::chrono::steady_clock;

namespace {

struct NullSink : MessageSink {
    void onTrade(Quantity, Price) override {}
    void onOrderFullyFilled(OrderId) override {}
    void onOrderPartiallyFilled(OrderId, Quantity) override {}
};

std::uint64_t nanosSince(Clock::time_point t0) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

void report(const char* name, double amortizedNs, std::vector<std::uint64_t>& lat) {
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
        std::size_t i = static_cast<std::size_t>(p * lat.size());
        if (i >= lat.size()) i = lat.size() - 1;
        return lat[i];
    };
    std::printf("%-24s amortized=%6.1f ns/op | p50=%4llu p99=%5llu p99.9=%6llu max=%8llu ns\n",
                name, amortizedNs,
                (unsigned long long)pct(0.50), (unsigned long long)pct(0.99),
                (unsigned long long)pct(0.999), (unsigned long long)lat.back());
}

Price tick(std::int64_t t) { return Price::fromTicks(t); }

} // namespace

int main(int argc, char** argv) {
    const std::size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const std::int64_t center = 1000 * Price::SCALE; // mid-market, mid-band
    NullSink sink;

    std::printf("Matching-engine latency, N=%zu ops per scenario\n", N);

    // --- add (resting, no match) and cancel, on one book --------------------
    {
        OrderBook book;
        auto bidTick = [&](std::size_t i) { return center - 2048 + static_cast<std::int64_t>(i % 4096); };

        // Warm up: grow the pool to N nodes so measured passes allocate nothing.
        for (std::size_t i = 0; i < N; ++i) book.addOrder(1 + i, Side::Buy, tick(bidTick(i)), 1, sink);
        for (std::size_t i = 0; i < N; ++i) book.cancelOrder(1 + i);

        // Clean amortized (no per-op timer): add N, then cancel N.
        auto t = Clock::now();
        for (std::size_t i = 0; i < N; ++i) book.addOrder(1 + i, Side::Buy, tick(bidTick(i)), 1, sink);
        const double addAmort = double(nanosSince(t)) / N;
        t = Clock::now();
        for (std::size_t i = 0; i < N; ++i) book.cancelOrder(1 + i);
        const double cancelAmort = double(nanosSince(t)) / N;

        // Per-op percentiles.
        std::vector<std::uint64_t> addLat, cancelLat;
        addLat.reserve(N);
        cancelLat.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            auto t0 = Clock::now();
            book.addOrder(1 + i, Side::Buy, tick(bidTick(i)), 1, sink);
            addLat.push_back(nanosSince(t0));
        }
        for (std::size_t i = 0; i < N; ++i) {
            auto t0 = Clock::now();
            book.cancelOrder(1 + i);
            cancelLat.push_back(nanosSince(t0));
        }
        report("add (resting, no match)", addAmort, addLat);
        report("cancel", cancelAmort, cancelLat);
    }

    // --- add (matches one resting order) ------------------------------------
    {
        OrderBook book;
        // 2N resting asks at one price: one set for the clean pass, one for percentiles.
        for (std::size_t i = 0; i < 2 * N; ++i) book.addOrder(1 + i, Side::Sell, tick(center), 1, sink);

        auto t = Clock::now();
        for (std::size_t i = 0; i < N; ++i) book.addOrder(2 * N + 1 + i, Side::Buy, tick(center), 1, sink);
        const double matchAmort = double(nanosSince(t)) / N;

        std::vector<std::uint64_t> matchLat;
        matchLat.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            auto t0 = Clock::now();
            book.addOrder(3 * N + 1 + i, Side::Buy, tick(center), 1, sink);
            matchLat.push_back(nanosSince(t0));
        }
        report("add (matches 1 resting)", matchAmort, matchLat);
    }

    return 0;
}
