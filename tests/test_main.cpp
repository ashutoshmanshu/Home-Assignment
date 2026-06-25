#include "test_framework.h"

#include "../src/MessageParser.h"
#include "../src/OrderBook.h"
#include "../src/Price.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace me;

// ---------------------------------------------------------------------------
// A MessageSink that records the engine's output as canonical protocol lines,
// so a whole match can be asserted against an expected sequence.
// ---------------------------------------------------------------------------
class RecordingSink : public MessageSink {
public:
    void onTrade(Quantity quantity, Price price) override {
        lines_.push_back("2," + std::to_string(quantity) + ',' + price.toString());
    }
    void onOrderFullyFilled(OrderId id) override {
        lines_.push_back("3," + std::to_string(id));
    }
    void onOrderPartiallyFilled(OrderId id, Quantity remaining) override {
        lines_.push_back("4," + std::to_string(id) + ',' + std::to_string(remaining));
    }
    const std::vector<std::string>& lines() const { return lines_; }
    void clear() { lines_.clear(); }

private:
    std::vector<std::string> lines_;
};

namespace {

void checkSequence(const std::vector<std::string>& actual,
                   const std::vector<std::string>& expected) {
    CHECK_EQ(actual.size(), expected.size());
    const std::size_t n = std::min(actual.size(), expected.size());
    for (std::size_t i = 0; i < n; ++i) CHECK_EQ(actual[i], expected[i]);
}

Price px(const std::string& s) { return Price::parse(s).value(); }

} // namespace

// ===========================================================================
// Price
// ===========================================================================
TEST_CASE(price_parses_and_formats_integers) {
    CHECK_EQ(px("1025").toString(), std::string("1025"));
    CHECK_EQ(px("0.0001").toString(), std::string("0.0001"));
    CHECK_EQ(px("1025.5").toString(), std::string("1025.5"));
    CHECK_EQ(px("1025.0000").toString(), std::string("1025")); // trailing zeros trimmed
    CHECK_EQ(px("12.3400").toString(), std::string("12.34"));
}

TEST_CASE(price_rejects_invalid_input) {
    CHECK_FALSE(Price::parse("").has_value());
    CHECK_FALSE(Price::parse("abc").has_value());
    CHECK_FALSE(Price::parse("-5").has_value());      // must be positive
    CHECK_FALSE(Price::parse("0").has_value());       // must be > 0
    CHECK_FALSE(Price::parse("1.23456").has_value()); // more than 4 dp
    CHECK_FALSE(Price::parse("1.2.3").has_value());
    CHECK_FALSE(Price::parse("12a").has_value());
}

TEST_CASE(price_ordering_is_exact) {
    CHECK(px("1000") < px("1000.0001"));
    CHECK(px("975") < px("1000"));
    CHECK(px("1025") == px("1025.0000"));
}

// ===========================================================================
// MessageParser
// ===========================================================================
TEST_CASE(parser_reads_add_request) {
    MessageParser p;
    auto r = p.parseLine("0,123,0,9,1000");
    CHECK(r.kind == ParseResult::Kind::Add);
    CHECK_EQ(r.add.id, static_cast<OrderId>(123));
    CHECK(r.add.side == Side::Buy);
    CHECK_EQ(r.add.quantity, static_cast<Quantity>(9));
    CHECK(r.add.price == px("1000"));
}

TEST_CASE(parser_reads_cancel_request) {
    MessageParser p;
    auto r = p.parseLine("1,123");
    CHECK(r.kind == ParseResult::Kind::Cancel);
    CHECK_EQ(r.cancel.id, static_cast<OrderId>(123));
}

TEST_CASE(parser_flags_unknown_message_type) {
    MessageParser p;
    auto r = p.parseLine("BADMESSAGE");
    CHECK(r.kind == ParseResult::Kind::Error);
    CHECK_EQ(r.error, std::string("Unknown message type: BADMESSAGE"));
}

TEST_CASE(parser_skips_blank_and_comment_lines) {
    MessageParser p;
    CHECK(p.parseLine("").kind == ParseResult::Kind::Skip);
    CHECK(p.parseLine("    \t  ").kind == ParseResult::Kind::Skip);
    CHECK(p.parseLine("# a comment").kind == ParseResult::Kind::Skip);
    CHECK(p.parseLine("// another comment").kind == ParseResult::Kind::Skip);
}

TEST_CASE(parser_strips_inline_comments) {
    MessageParser p;
    auto r = p.parseLine("0,7,1,5,1025   // resting sell");
    CHECK(r.kind == ParseResult::Kind::Add);
    CHECK_EQ(r.add.id, static_cast<OrderId>(7));
    CHECK(r.add.side == Side::Sell);
}

TEST_CASE(parser_rejects_malformed_requests) {
    MessageParser p;
    CHECK(p.parseLine("0,1,2,5,100").kind == ParseResult::Kind::Error);   // bad side
    CHECK(p.parseLine("0,1,0,0,100").kind == ParseResult::Kind::Error);   // zero quantity
    CHECK(p.parseLine("0,1,0,5,abc").kind == ParseResult::Kind::Error);   // bad price
    CHECK(p.parseLine("0,1,0,5").kind == ParseResult::Kind::Error);       // too few fields
    CHECK(p.parseLine("0,0,0,5,100").kind == ParseResult::Kind::Error);   // zero order id
    CHECK(p.parseLine("1,1,2").kind == ParseResult::Kind::Error);         // cancel too many
    CHECK(p.parseLine("5,1,2").kind == ParseResult::Kind::Error);         // unknown type id
}

// ===========================================================================
// OrderBook: resting / no-match behaviour
// ===========================================================================
TEST_CASE(book_rests_non_crossing_orders_without_trades) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("1025"), 2, sink);
    book.addOrder(2, Side::Buy, px("1000"), 9, sink);

    CHECK(sink.lines().empty());           // no crossing -> no output
    CHECK_EQ(book.liveOrderCount(), static_cast<std::size_t>(2));
    CHECK(book.bestBid() == px("1000"));
    CHECK(book.bestAsk() == px("1025"));
}

// ===========================================================================
// OrderBook: the worked example from the assignment document
// ===========================================================================
TEST_CASE(book_reproduces_assignment_example) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1000000, Side::Sell, px("1075"), 1, sink);
    book.addOrder(1000001, Side::Buy,  px("1000"), 9, sink);
    book.addOrder(1000002, Side::Buy,  px("975"), 30, sink);
    book.addOrder(1000003, Side::Sell, px("1050"), 10, sink);
    book.addOrder(1000004, Side::Buy,  px("950"), 10, sink);
    book.addOrder(1000005, Side::Sell, px("1025"), 2, sink);
    book.addOrder(1000006, Side::Buy,  px("1000"), 1, sink);
    CHECK(book.cancelOrder(1000004));
    book.addOrder(1000007, Side::Sell, px("1025"), 5, sink);
    CHECK(sink.lines().empty());           // nothing crossed yet

    // The aggressive buy of 3 @ 1050 sweeps the 1025 level (oldest first).
    book.addOrder(1000008, Side::Buy, px("1050"), 3, sink);

    checkSequence(sink.lines(), {
        "2,2,1025",     // trade 2 @ 1025 (vs 1000005)
        "4,1000008,1",  // aggressive partially filled, 1 remaining
        "3,1000005",    // resting fully filled
        "2,1,1025",     // trade 1 @ 1025 (vs 1000007)
        "3,1000008",    // aggressive fully filled
        "4,1000007,4",  // resting partially filled, 4 remaining
    });

    CHECK_FALSE(book.contains(1000008));   // fully filled, never rested
    CHECK_FALSE(book.contains(1000005));   // fully filled, removed
    CHECK_EQ(book.openQuantity(1000007), static_cast<Quantity>(4));
}

// ===========================================================================
// OrderBook: sweeping multiple price levels, price-then-time priority
// ===========================================================================
TEST_CASE(book_sweeps_multiple_levels_best_price_first) {
    OrderBook book;
    RecordingSink sink;
    // Resting asks at three prices; 1000 has two orders (time priority).
    book.addOrder(1, Side::Sell, px("1002"), 5, sink);
    book.addOrder(2, Side::Sell, px("1000"), 3, sink);  // older at 1000
    book.addOrder(3, Side::Sell, px("1000"), 4, sink);  // newer at 1000
    book.addOrder(4, Side::Sell, px("1001"), 2, sink);
    sink.clear();

    // Aggressive buy of 10 @ 1002 should fill 1000(3), 1000(4), 1001(2), 1002(1).
    book.addOrder(99, Side::Buy, px("1002"), 10, sink);

    checkSequence(sink.lines(), {
        "2,3,1000", "4,99,7", "3,2",   // fill order 2 (best price, oldest)
        "2,4,1000", "4,99,3", "3,3",   // fill order 3 (same price, newer)
        "2,2,1001", "4,99,1", "3,4",   // next price level
        "2,1,1002", "3,99", "4,1,4",   // last unit; aggressive done, order 1 -> 4 left
    });
    CHECK_EQ(book.openQuantity(1), static_cast<Quantity>(4));
    CHECK_FALSE(book.contains(99));
}

TEST_CASE(book_aggressive_remainder_rests_in_book) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("100"), 3, sink);
    sink.clear();

    // Buy 10 @ 100: trades 3, then 7 remain and rest as a new bid at 100.
    book.addOrder(2, Side::Buy, px("100"), 10, sink);
    checkSequence(sink.lines(), {"2,3,100", "4,2,7", "3,1"});
    CHECK(book.contains(2));
    CHECK_EQ(book.openQuantity(2), static_cast<Quantity>(7));
    CHECK(book.bestBid() == px("100"));
    CHECK(!book.bestAsk().has_value());
}

TEST_CASE(book_trade_executes_at_resting_price_not_aggressive) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("100"), 5, sink); // resting ask at 100
    sink.clear();

    // Aggressive buy willing to pay 120 trades at the resting 100, not 120.
    book.addOrder(2, Side::Buy, px("120"), 5, sink);
    checkSequence(sink.lines(), {"2,5,100", "3,2", "3,1"});
}

TEST_CASE(book_equal_quantities_fully_fill_both) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("100"), 5, sink);
    sink.clear();
    book.addOrder(2, Side::Buy, px("100"), 5, sink);
    // Exactly one trade, two fully-filled, zero partials.
    checkSequence(sink.lines(), {"2,5,100", "3,2", "3,1"});
    CHECK_EQ(book.liveOrderCount(), static_cast<std::size_t>(0));
}

// ===========================================================================
// OrderBook: cancel + duplicate id handling
// ===========================================================================
TEST_CASE(book_cancel_removes_resting_order) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("100"), 5, sink);
    CHECK(book.contains(1));
    CHECK(book.cancelOrder(1));
    CHECK_FALSE(book.contains(1));
    CHECK(!book.bestAsk().has_value());

    // A subsequent crossing buy now finds nothing to match.
    book.addOrder(2, Side::Buy, px("100"), 5, sink);
    CHECK(sink.lines().empty());
    CHECK(book.contains(2));
}

TEST_CASE(book_cancel_unknown_id_returns_false) {
    OrderBook book;
    CHECK_FALSE(book.cancelOrder(424242));
}

TEST_CASE(book_rejects_duplicate_order_id) {
    OrderBook book;
    RecordingSink sink;
    CHECK(book.addOrder(1, Side::Buy, px("100"), 5, sink));
    CHECK_FALSE(book.addOrder(1, Side::Buy, px("101"), 7, sink)); // duplicate id
    CHECK_EQ(book.openQuantity(1), static_cast<Quantity>(5));     // original untouched
}

TEST_CASE(book_id_is_reusable_after_full_fill) {
    OrderBook book;
    RecordingSink sink;
    book.addOrder(1, Side::Sell, px("100"), 5, sink);
    book.addOrder(2, Side::Buy, px("100"), 5, sink); // fully fills both
    CHECK_FALSE(book.contains(1));
    // id 1 is free again because it left the book.
    CHECK(book.addOrder(1, Side::Buy, px("90"), 4, sink));
    CHECK(book.contains(1));
}

int main() {
    return metest::runAll();
}
