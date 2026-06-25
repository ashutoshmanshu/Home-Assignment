#ifndef MATCHING_ENGINE_ORDER_H
#define MATCHING_ENGINE_ORDER_H

#include "Price.h"
#include "Types.h"

namespace me {

// A resting (or in-flight aggressive) order. `quantity` is the *remaining* open
// quantity, which shrinks as the order is filled.
struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price;
    Quantity quantity = 0;

    Order() = default;
    Order(OrderId id_, Side side_, Price price_, Quantity quantity_)
        : id(id_), side(side_), price(price_), quantity(quantity_) {}
};

} // namespace me

#endif // MATCHING_ENGINE_ORDER_H
