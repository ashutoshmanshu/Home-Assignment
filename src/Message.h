#ifndef MATCHING_ENGINE_MESSAGE_H
#define MATCHING_ENGINE_MESSAGE_H

#include "Price.h"
#include "Types.h"

#include <cstdint>

namespace me {

// Wire identifiers for every message in the protocol. Inputs are Add/Cancel;
// the remaining three are produced by the engine (see MessageSink).
enum class MsgType : int {
    AddOrder = 0,
    CancelOrder = 1,
    TradeEvent = 2,
    OrderFullyFilled = 3,
    OrderPartiallyFilled = 4,
};

// Parsed AddOrderRequest: "0, orderid, side, quantity, price".
struct AddOrderRequest {
    OrderId id = 0;
    Side side = Side::Buy;
    Quantity quantity = 0;
    Price price;
};

// Parsed CancelOrderRequest: "1, orderid".
struct CancelOrderRequest {
    OrderId id = 0;
};

} // namespace me

#endif // MATCHING_ENGINE_MESSAGE_H
