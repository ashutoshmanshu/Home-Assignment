#ifndef MATCHING_ENGINE_TYPES_H
#define MATCHING_ENGINE_TYPES_H

#include <cstdint>

namespace me {

// Unique identifier for an order. The spec guarantees a positive integer; we
// store it unsigned and reserve 0 as an "invalid id" sentinel during parsing.
using OrderId = std::uint64_t;

// Order quantities are positive integers. 64 bits keeps us safe well past any
// realistic aggregate size and avoids overflow when summing levels.
using Quantity = std::uint64_t;

// Slot index into the order pool / id index. 0xFFFFFFFF is the "none" sentinel
// (we never have 2^32-1 live orders), so we avoid pointers and stay cache-dense.
using Slot = std::uint32_t;
inline constexpr Slot kNoSlot = 0xFFFFFFFFu;

// Side of an order. Wire format encodes Buy as 0 and Sell as 1.
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline Side opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace me

#endif // MATCHING_ENGINE_TYPES_H
