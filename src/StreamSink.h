#ifndef MATCHING_ENGINE_STREAM_SINK_H
#define MATCHING_ENGINE_STREAM_SINK_H

#include "MessageSink.h"

#include <ostream>

namespace me {

// Writes engine output messages to an ostream in the protocol's CSV format:
//   TradeEvent           -> "2,<quantity>,<price>"
//   OrderFullyFilled     -> "3,<orderid>"
//   OrderPartiallyFilled -> "4,<orderid>,<quantity>"
class StreamSink : public MessageSink {
public:
    explicit StreamSink(std::ostream& out) : out_(out) {}

    void onTrade(Quantity quantity, Price price) override {
        out_ << "2," << quantity << ',' << price.toString() << '\n';
    }
    void onOrderFullyFilled(OrderId id) override {
        out_ << "3," << id << '\n';
    }
    void onOrderPartiallyFilled(OrderId id, Quantity remaining) override {
        out_ << "4," << id << ',' << remaining << '\n';
    }

private:
    std::ostream& out_;
};

} // namespace me

#endif // MATCHING_ENGINE_STREAM_SINK_H
