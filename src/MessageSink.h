#ifndef MATCHING_ENGINE_MESSAGE_SINK_H
#define MATCHING_ENGINE_MESSAGE_SINK_H

#include "Price.h"
#include "Types.h"

namespace me {

// Output destination for the events the matching engine produces.
//
// The engine talks to this abstract interface rather than writing to stdout
// directly. That keeps the matching logic free of I/O concerns and lets tests
// capture the exact event stream (see RecordingSink in the tests), while the
// production driver uses a sink that formats to an ostream (StreamSink).
class MessageSink {
public:
    virtual ~MessageSink() = default;

    // msgtype 2: a trade executed for `quantity` at `price`.
    virtual void onTrade(Quantity quantity, Price price) = 0;

    // msgtype 3: the order was fully filled and removed from the book.
    virtual void onOrderFullyFilled(OrderId id) = 0;

    // msgtype 4: the order was partially filled; `remaining` is its new open quantity.
    virtual void onOrderPartiallyFilled(OrderId id, Quantity remaining) = 0;
};

} // namespace me

#endif // MATCHING_ENGINE_MESSAGE_SINK_H
