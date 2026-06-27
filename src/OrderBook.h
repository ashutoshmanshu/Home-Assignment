#ifndef MATCHING_ENGINE_ORDER_BOOK_H
#define MATCHING_ENGINE_ORDER_BOOK_H

#include "IdIndex.h"
#include "MessageSink.h"
#include "OccupancyBitmap.h"
#include "Order.h"
#include "Price.h"
#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace me {

// A limit order book with price-time (FIFO) priority, engineered for low and
// predictable latency. See README "Architecture" for the full rationale; the
// short version:
//
//   * Orders live in a pre-grown pool (`nodes_`) and are linked into per-level
//     FIFOs through intrusive prev/next slot indices. There is *no* per-order
//     heap allocation on the hot path and no pointer chasing across cache lines
//     the way std::list / std::map nodes would impose.
//   * Price levels are a flat array indexed by price tick, with a 3-level
//     occupancy bitmap that yields best-bid / best-ask in O(1) via bit-scan,
//     replacing a red-black tree's O(log L) walk. The array spans a price band
//     (the instrument's intraday range); prices outside it fall back to an
//     ordered map so *any* price is still handled correctly, just on a slower
//     path. Correctness never depends on the band.
//   * `idIndex_` (open-addressing) maps OrderId -> pool slot for O(1) cancels.
//
// The book is intentionally single-threaded: deterministic price-time priority
// and atomic multi-leg trades require serialized processing. Concurrency lives
// at the I/O boundary (see SpscRing), not inside the book.
class OrderBook {
public:
    OrderBook();

    // Process an AddOrderRequest: match against the opposite side, then rest any
    // leftover. Emits trade/fill events to `sink`. Returns false (no change) if
    // `id` is already live.
    bool addOrder(OrderId id, Side side, Price price, Quantity quantity, MessageSink& sink);

    // Process a CancelOrderRequest. Returns false if no live order has that id.
    bool cancelOrder(OrderId id);

    // --- Inspection helpers (used by tests; all O(1)) ---
    bool contains(OrderId id) const { return idIndex_.find(id) != kNoSlot; }
    Quantity openQuantity(OrderId id) const;
    std::size_t liveOrderCount() const { return idIndex_.size(); }
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

private:
    // Size of the flat price-level array per side, in ticks. 2^20 ticks at the
    // Price scale (1e-4) spans ~104 price units around the market, comfortably
    // an instrument's intraday band, for ~8 MB/side. Out-of-band prices use the
    // ordered-map fallback.
    static constexpr std::size_t kBandTicks = std::size_t(1) << 20;

    // A pooled order. `prev`/`next` thread the intrusive FIFO at its price level.
    struct Node {
        OrderId id;
        Quantity qty;       // remaining open quantity
        std::int64_t ticks; // price, in Price ticks
        Side side;
        Slot prev;
        Slot next;
    };

    // FIFO at one price level: head = oldest, tail = newest.
    struct Level {
        Slot head = kNoSlot;
        Slot tail = kNoSlot;
    };

    // --- pool management ---
    Slot allocNode();
    void freeNode(Slot s);

    // --- band / level helpers ---
    void ensureBase(std::int64_t ticks);
    bool inBand(std::int64_t ticks) const {
        return baseSet_ && ticks >= bandBase_ &&
               static_cast<std::size_t>(ticks - bandBase_) < kBandTicks;
    }
    std::size_t bandIndex(std::int64_t ticks) const {
        return static_cast<std::size_t>(ticks - bandBase_);
    }

    // Best opposite price + whether it sits in the band (vs. the fallback map).
    // Returns false when that side of the book is empty.
    bool bestAskLocation(std::int64_t& ticks, bool& fromBand) const;
    bool bestBidLocation(std::int64_t& ticks, bool& fromBand) const;

    Level& levelForInsert(Side side, std::int64_t ticks);  // creates if needed
    Level& levelForExisting(Side side, std::int64_t ticks); // must exist
    void removeEmptyLevel(Side side, std::int64_t ticks, bool fromBand);

    void insertResting(const Order& order);
    void matchAtLevel(Order& aggressive, Level& level, std::int64_t tradeTicks, MessageSink& sink);

    // Order pool + free list.
    std::vector<Node> nodes_;
    Slot freeHead_ = kNoSlot;

    // id -> pool slot.
    IdIndex idIndex_;

    // Flat price-level arrays + occupancy bitmaps (the fast path).
    std::vector<Level> bidBand_;
    std::vector<Level> askBand_;
    OccupancyBitmap bidBitmap_; // best bid = highest occupied
    OccupancyBitmap askBitmap_; // best ask = lowest occupied

    // Out-of-band fallback (ordered so begin() is the best price on each side).
    std::map<std::int64_t, Level, std::greater<std::int64_t>> bidFallback_;
    std::map<std::int64_t, Level, std::less<std::int64_t>> askFallback_;

    std::int64_t bandBase_ = 0; // tick mapped to band index 0
    bool baseSet_ = false;
};

} // namespace me

#endif // MATCHING_ENGINE_ORDER_BOOK_H
