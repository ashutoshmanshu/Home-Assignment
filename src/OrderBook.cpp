#include "OrderBook.h"

#include <algorithm>

namespace me {

template <typename BookMap, typename CrossFn>
void OrderBook::matchAgainst(Order& aggressive, BookMap& book, CrossFn crosses, MessageSink& sink) {
    while (aggressive.quantity > 0 && !book.empty()) {
        auto level = book.begin();           // best price level on the opposite side
        const Price restingPrice = level->first;
        if (!crosses(restingPrice, aggressive.price)) break; // no longer crossing -> done

        OrderQueue& queue = level->second;
        while (aggressive.quantity > 0 && !queue.empty()) {
            Order& resting = queue.front();  // oldest order at this price (time priority)
            const Quantity traded = std::min(aggressive.quantity, resting.quantity);

            // Trades execute at the resting (passive) order's price.
            aggressive.quantity -= traded;
            resting.quantity -= traded;

            // Per spec, each matched pair emits, in order:
            //   1) the TradeEvent
            //   2) a fill message for the aggressive order
            //   3) a fill message for the resting order
            sink.onTrade(traded, restingPrice);

            if (aggressive.quantity == 0) sink.onOrderFullyFilled(aggressive.id);
            else                          sink.onOrderPartiallyFilled(aggressive.id, aggressive.quantity);

            if (resting.quantity == 0) {
                sink.onOrderFullyFilled(resting.id);
                index_.erase(resting.id);
                queue.pop_front();           // O(1) removal of the filled head
            } else {
                sink.onOrderPartiallyFilled(resting.id, resting.quantity);
            }
        }

        if (queue.empty()) book.erase(level); // drop the emptied price level
    }
}

bool OrderBook::addOrder(OrderId id, Side side, Price price, Quantity quantity, MessageSink& sink) {
    if (index_.find(id) != index_.end()) return false; // duplicate live order id

    Order aggressive(id, side, price, quantity);
    if (side == Side::Buy) {
        // A buy crosses a resting sell when the ask price is at or below our bid.
        matchAgainst(aggressive, asks_,
                     [](Price resting, Price agg) { return resting <= agg; }, sink);
    } else {
        // A sell crosses a resting buy when the bid price is at or above our ask.
        matchAgainst(aggressive, bids_,
                     [](Price resting, Price agg) { return resting >= agg; }, sink);
    }

    if (aggressive.quantity > 0) insertResting(aggressive); // leftover rests in the book
    return true;
}

void OrderBook::insertResting(const Order& order) {
    if (order.side == Side::Buy) {
        OrderQueue& queue = bids_[order.price];
        queue.push_back(order);
        index_[order.id] = OrderRef{order.side, order.price, std::prev(queue.end())};
    } else {
        OrderQueue& queue = asks_[order.price];
        queue.push_back(order);
        index_[order.id] = OrderRef{order.side, order.price, std::prev(queue.end())};
    }
}

bool OrderBook::cancelOrder(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    const OrderRef& ref = found->second;
    if (ref.side == Side::Buy) {
        auto level = bids_.find(ref.price);
        level->second.erase(ref.it);
        if (level->second.empty()) bids_.erase(level);
    } else {
        auto level = asks_.find(ref.price);
        level->second.erase(ref.it);
        if (level->second.empty()) asks_.erase(level);
    }
    index_.erase(found);
    return true;
}

Quantity OrderBook::openQuantity(OrderId id) const {
    auto found = index_.find(id);
    return found == index_.end() ? 0 : found->second.it->quantity;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

} // namespace me
