#include "MessageParser.h"
#include "OrderBook.h"
#include "SpscRing.h"
#include "StreamSink.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace me;

// Apply one parsed request to the book, logging engine-level rejections.
void apply(OrderBook& book, MessageSink& sink, const ParseResult& r) {
    if (r.kind == ParseResult::Kind::Add) {
        if (!book.addOrder(r.add.id, r.add.side, r.add.price, r.add.quantity, sink))
            std::cerr << "Duplicate order id ignored: " << r.add.id << '\n';
    } else if (r.kind == ParseResult::Kind::Cancel) {
        if (!book.cancelOrder(r.cancel.id))
            std::cerr << "Cancel for unknown order id ignored: " << r.cancel.id << '\n';
    }
}

// Default path: parse and match on one thread. Deterministic and simplest.
int runSingleThreaded() {
    OrderBook book;
    MessageParser parser;
    StreamSink sink(std::cout);

    std::string line;
    while (std::getline(std::cin, line)) {
        const ParseResult r = parser.parseLine(line);
        if (r.kind == ParseResult::Kind::Error) std::cerr << r.error << '\n';
        else                                    apply(book, sink, r);
    }
    return 0;
}

// A compact, fixed-size command crossing the lock-free ring. Validation happens
// on the ingress thread, so only well-formed commands reach the matcher — the
// queue carries no std::string and allocates nothing.
struct Command {
    enum class Op : std::uint8_t { Add, Cancel } op;
    OrderId id;
    Side side;
    Quantity qty;
    Price price;
};

// Optional pipeline path (--pipeline): the I/O/parse thread feeds the
// single-writer matching thread through the SPSC ring. This is the production
// shape — lock-free hand-off at the boundary, the book still single-threaded so
// price-time priority stays deterministic. stdout is written only by the
// matcher, so output order is identical to single-threaded mode.
int runPipeline() {
    OrderBook book;
    StreamSink sink(std::cout);
    SpscRing<Command> ring(1u << 16);
    std::atomic<bool> ingressDone{false};

    std::thread ingress([&] {
        MessageParser parser;
        std::string line;
        while (std::getline(std::cin, line)) {
            const ParseResult r = parser.parseLine(line);
            if (r.kind == ParseResult::Kind::Skip) continue;
            if (r.kind == ParseResult::Kind::Error) {
                std::cerr << r.error << '\n';
                continue;
            }
            Command c{};
            if (r.kind == ParseResult::Kind::Add) {
                c.op = Command::Op::Add;
                c.id = r.add.id; c.side = r.add.side; c.qty = r.add.quantity; c.price = r.add.price;
            } else {
                c.op = Command::Op::Cancel;
                c.id = r.cancel.id;
            }
            while (!ring.push(c)) { /* spin: ring full, matcher will drain */ }
        }
        ingressDone.store(true, std::memory_order_release);
    });

    auto process = [&](const Command& c) {
        if (c.op == Command::Op::Add) {
            if (!book.addOrder(c.id, c.side, c.price, c.qty, sink))
                std::cerr << "Duplicate order id ignored: " << c.id << '\n';
        } else {
            if (!book.cancelOrder(c.id))
                std::cerr << "Cancel for unknown order id ignored: " << c.id << '\n';
        }
    };

    // Matcher: process in FIFO order; once the producer signals done, fully
    // drain whatever remains (no more will be pushed) and stop.
    Command c{};
    while (true) {
        if (ring.pop(c)) {
            process(c);
        } else if (ingressDone.load(std::memory_order_acquire)) {
            while (ring.pop(c)) process(c);
            break;
        }
    }

    ingress.join();
    return 0;
}

} // namespace

// Entry point. Reads order messages from stdin, drives the matching engine, and
// writes trade/fill messages to stdout (errors to stderr). No input can crash
// the process: every line is validated by the parser and bad lines are skipped.
int main(int argc, char** argv) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    bool pipeline = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--pipeline") == 0) pipeline = true;

    return pipeline ? runPipeline() : runSingleThreaded();
}
