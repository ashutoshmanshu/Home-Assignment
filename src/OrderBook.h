#ifndef MATCHING_ENGINE_ORDER_BOOK_H
#define MATCHING_ENGINE_ORDER_BOOK_H

#include "MessageSink.h"
#include "Order.h"
#include "Price.h"
#include "Types.h"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

namespace me {

// A limit order book with price-time (FIFO) priority matching.
//
// Data structures (see README for the full rationale and complexity analysis):
//   * Each side is a std::map keyed by price. Bids are sorted descending and
//     asks ascending, so the best price on either side is always begin(), and
//     the map keeps every other price level sorted for the rare deep-book walk.
//   * Each price level holds a std::list<Order> in arrival order (front = oldest),
//     giving O(1) push to the back, O(1) pop of the matched front, and stable
//     iterators so we can cancel from the middle without invalidating others.
//   * `index_` maps OrderId -> the order's exact location, so cancels and lookups
//     are O(1) average without scanning the book.
class OrderBook {
public:
    using OrderQueue = std::list<Order>;
    // Bids: highest price first. Asks: lowest price first. begin() == best.
    using BidMap = std::map<Price, OrderQueue, std::greater<Price>>;
    using AskMap = std::map<Price, OrderQueue, std::less<Price>>;

    // Process an AddOrderRequest: match against the opposite side first, then
    // rest any leftover quantity. All resulting trade/fill events go to `sink`.
    // Returns false (and changes nothing) if `id` is already live in the book.
    bool addOrder(OrderId id, Side side, Price price, Quantity quantity, MessageSink& sink);

    // Process a CancelOrderRequest: remove the order from the book.
    // Returns false if no live order has that id. Produces no output messages
    // (the protocol defines no cancel acknowledgement).
    bool cancelOrder(OrderId id);

    // --- Inspection helpers (used by tests; cheap and const) ---
    bool contains(OrderId id) const { return index_.find(id) != index_.end(); }
    Quantity openQuantity(OrderId id) const;
    std::size_t liveOrderCount() const { return index_.size(); }
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

private:
    // Location of a live order, enabling O(1) cancellation.
    struct OrderRef {
        Side side;
        Price price;
        OrderQueue::iterator it;
    };

    // Match `aggressive` against `book` (the opposite side) while its best level
    // crosses, per `crosses(restingPrice, aggressivePrice)`. Mutates `aggressive`.
    template <typename BookMap, typename CrossFn>
    void matchAgainst(Order& aggressive, BookMap& book, CrossFn crosses, MessageSink& sink);

    void insertResting(const Order& order);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, OrderRef> index_;
};

} // namespace me

#endif // MATCHING_ENGINE_ORDER_BOOK_H
