#include "MessageParser.h"
#include "OrderBook.h"
#include "StreamSink.h"

#include <iostream>
#include <string>

// Entry point: read order messages from stdin, drive the matching engine, write
// trade/fill messages to stdout, and log any malformed or rejected input to
// stderr. Designed so that *no* input can crash the process: every line is
// validated by the parser and bad lines are reported and skipped.
int main() {
    // We only use the C++ iostreams, so untie them from C stdio for throughput.
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    me::OrderBook book;
    me::MessageParser parser;
    me::StreamSink sink(std::cout);

    std::string line;
    while (std::getline(std::cin, line)) {
        const me::ParseResult result = parser.parseLine(line);
        switch (result.kind) {
            case me::ParseResult::Kind::Skip:
                break;

            case me::ParseResult::Kind::Error:
                std::cerr << result.error << '\n';
                break;

            case me::ParseResult::Kind::Add:
                if (!book.addOrder(result.add.id, result.add.side, result.add.price,
                                   result.add.quantity, sink)) {
                    std::cerr << "Duplicate order id ignored: " << result.add.id << '\n';
                }
                break;

            case me::ParseResult::Kind::Cancel:
                if (!book.cancelOrder(result.cancel.id)) {
                    std::cerr << "Cancel for unknown order id ignored: " << result.cancel.id << '\n';
                }
                break;
        }
    }

    return 0;
}
