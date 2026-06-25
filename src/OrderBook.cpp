#include "OrderBook.h"

#include <algorithm>
#include <limits>

namespace me {

namespace {
constexpr std::int64_t kPlusInf = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMinusInf = std::numeric_limits<std::int64_t>::min();
} // namespace

OrderBook::OrderBook()
    : bidBand_(kBandTicks),
      askBand_(kBandTicks),
      bidBitmap_(kBandTicks),
      askBitmap_(kBandTicks) {
    nodes_.reserve(1u << 16); // amortize early growth; pool never shrinks
}

// --- order pool ------------------------------------------------------------

Slot OrderBook::allocNode() {
    if (freeHead_ != kNoSlot) {
        const Slot s = freeHead_;
        freeHead_ = nodes_[s].next;
        return s;
    }
    nodes_.push_back(Node{});
    return static_cast<Slot>(nodes_.size() - 1);
}

void OrderBook::freeNode(Slot s) {
    nodes_[s].next = freeHead_;
    freeHead_ = s;
}

// --- band / level helpers --------------------------------------------------

void OrderBook::ensureBase(std::int64_t ticks) {
    if (baseSet_) return;
    // Centre the band on the first price we see so the active market sits mid-band.
    bandBase_ = std::max<std::int64_t>(0, ticks - static_cast<std::int64_t>(kBandTicks) / 2);
    baseSet_ = true;
}

bool OrderBook::bestAskLocation(std::int64_t& ticks, bool& fromBand) const {
    const std::size_t low = askBitmap_.findLowest();
    const std::int64_t bandTicks = (low == OccupancyBitmap::npos)
                                       ? kPlusInf
                                       : bandBase_ + static_cast<std::int64_t>(low);
    const std::int64_t fbTicks = askFallback_.empty() ? kPlusInf : askFallback_.begin()->first;
    if (bandTicks == kPlusInf && fbTicks == kPlusInf) return false;

    fromBand = bandTicks <= fbTicks;
    ticks = fromBand ? bandTicks : fbTicks;
    return true;
}

bool OrderBook::bestBidLocation(std::int64_t& ticks, bool& fromBand) const {
    const std::size_t high = bidBitmap_.findHighest();
    const std::int64_t bandTicks = (high == OccupancyBitmap::npos)
                                       ? kMinusInf
                                       : bandBase_ + static_cast<std::int64_t>(high);
    const std::int64_t fbTicks = bidFallback_.empty() ? kMinusInf : bidFallback_.begin()->first;
    if (bandTicks == kMinusInf && fbTicks == kMinusInf) return false;

    fromBand = bandTicks >= fbTicks;
    ticks = fromBand ? bandTicks : fbTicks;
    return true;
}

OrderBook::Level& OrderBook::levelForInsert(Side side, std::int64_t ticks) {
    ensureBase(ticks);
    if (inBand(ticks)) {
        const std::size_t idx = bandIndex(ticks);
        if (side == Side::Buy) { bidBitmap_.set(idx); return bidBand_[idx]; } // set is idempotent
        askBitmap_.set(idx);
        return askBand_[idx];
    }
    // The two fallback maps have different comparator types, so branch explicitly.
    if (side == Side::Buy) return bidFallback_[ticks];
    return askFallback_[ticks];
}

OrderBook::Level& OrderBook::levelForExisting(Side side, std::int64_t ticks) {
    if (inBand(ticks)) return (side == Side::Buy ? bidBand_ : askBand_)[bandIndex(ticks)];
    if (side == Side::Buy) return bidFallback_.find(ticks)->second;
    return askFallback_.find(ticks)->second;
}

void OrderBook::removeEmptyLevel(Side side, std::int64_t ticks, bool fromBand) {
    if (fromBand) {
        (side == Side::Buy ? bidBitmap_ : askBitmap_).clear(bandIndex(ticks));
        return;
    }
    if (side == Side::Buy) bidFallback_.erase(ticks);
    else                   askFallback_.erase(ticks);
}

// --- matching --------------------------------------------------------------

void OrderBook::matchAtLevel(Order& aggressive, Level& level, std::int64_t tradeTicks,
                             MessageSink& sink) {
    const Price tradePrice = Price::fromTicks(tradeTicks); // trades at the resting price
    while (aggressive.quantity > 0 && level.head != kNoSlot) {
        Node& resting = nodes_[level.head];
        const Quantity traded = std::min(aggressive.quantity, resting.qty);

        aggressive.quantity -= traded;
        resting.qty -= traded;

        // Per spec, each matched pair emits: TradeEvent, aggressive fill, resting fill.
        sink.onTrade(traded, tradePrice);

        if (aggressive.quantity == 0) sink.onOrderFullyFilled(aggressive.id);
        else                          sink.onOrderPartiallyFilled(aggressive.id, aggressive.quantity);

        if (resting.qty == 0) {
            const Slot filled = level.head;
            const OrderId restingId = resting.id;
            sink.onOrderFullyFilled(restingId);

            level.head = resting.next;          // pop the filled head (O(1))
            if (level.head != kNoSlot) nodes_[level.head].prev = kNoSlot;
            else                       level.tail = kNoSlot;

            idIndex_.erase(restingId);
            freeNode(filled);
        } else {
            sink.onOrderPartiallyFilled(resting.id, resting.qty);
        }
    }
}

bool OrderBook::addOrder(OrderId id, Side side, Price price, Quantity quantity, MessageSink& sink) {
    if (idIndex_.find(id) != kNoSlot) return false; // duplicate live id

    Order aggressive(id, side, price, quantity);
    const std::int64_t aggTicks = price.ticks();

    if (side == Side::Buy) {
        // Cross resting asks priced at or below our bid, best (lowest) first.
        while (aggressive.quantity > 0) {
            std::int64_t bestTicks;
            bool fromBand;
            if (!bestAskLocation(bestTicks, fromBand)) break;
            if (bestTicks > aggTicks) break;
            Level& level = fromBand ? askBand_[bandIndex(bestTicks)] : askFallback_.begin()->second;
            matchAtLevel(aggressive, level, bestTicks, sink);
            if (level.head == kNoSlot) removeEmptyLevel(Side::Sell, bestTicks, fromBand);
        }
    } else {
        // Cross resting bids priced at or above our ask, best (highest) first.
        while (aggressive.quantity > 0) {
            std::int64_t bestTicks;
            bool fromBand;
            if (!bestBidLocation(bestTicks, fromBand)) break;
            if (bestTicks < aggTicks) break;
            Level& level = fromBand ? bidBand_[bandIndex(bestTicks)] : bidFallback_.begin()->second;
            matchAtLevel(aggressive, level, bestTicks, sink);
            if (level.head == kNoSlot) removeEmptyLevel(Side::Buy, bestTicks, fromBand);
        }
    }

    if (aggressive.quantity > 0) insertResting(aggressive); // leftover rests
    return true;
}

void OrderBook::insertResting(const Order& order) {
    const std::int64_t ticks = order.price.ticks();
    const Slot s = allocNode(); // may reallocate nodes_, so do it before taking refs
    nodes_[s] = Node{order.id, order.quantity, ticks, order.side, kNoSlot, kNoSlot};

    Level& level = levelForInsert(order.side, ticks);
    nodes_[s].prev = level.tail;
    if (level.tail != kNoSlot) nodes_[level.tail].next = s;
    else                       level.head = s;
    level.tail = s;

    idIndex_.insert(order.id, s);
}

bool OrderBook::cancelOrder(OrderId id) {
    const Slot s = idIndex_.find(id);
    if (s == kNoSlot) return false;

    const std::int64_t ticks = nodes_[s].ticks;
    const Side side = nodes_[s].side;
    const Slot prev = nodes_[s].prev;
    const Slot next = nodes_[s].next;
    const bool fromBand = inBand(ticks);

    Level& level = levelForExisting(side, ticks);
    if (prev != kNoSlot) nodes_[prev].next = next;
    else                 level.head = next;
    if (next != kNoSlot) nodes_[next].prev = prev;
    else                 level.tail = prev;

    if (level.head == kNoSlot) removeEmptyLevel(side, ticks, fromBand);

    idIndex_.erase(id);
    freeNode(s);
    return true;
}

// --- inspection ------------------------------------------------------------

Quantity OrderBook::openQuantity(OrderId id) const {
    const Slot s = idIndex_.find(id);
    return s == kNoSlot ? 0 : nodes_[s].qty;
}

std::optional<Price> OrderBook::bestBid() const {
    std::int64_t ticks;
    bool fromBand;
    if (!bestBidLocation(ticks, fromBand)) return std::nullopt;
    return Price::fromTicks(ticks);
}

std::optional<Price> OrderBook::bestAsk() const {
    std::int64_t ticks;
    bool fromBand;
    if (!bestAskLocation(ticks, fromBand)) return std::nullopt;
    return Price::fromTicks(ticks);
}

} // namespace me
