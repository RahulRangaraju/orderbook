#pragma once
#include <cstdint>

namespace ob {
using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // in ticks, e.g. cents
using Quantity = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;        // remaining quantity
    std::uint64_t seq;   // arrival order (time priority)
};

struct Trade {
    OrderId  buyId;
    OrderId  sellId;
    Price    price;      // the resting order's price
    Quantity qty;
};

enum class CmdType : std::uint8_t { Add, Cancel, Modify };

struct Command {         // one input message
    CmdType  type;
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;
};

// Two trades are equal when every field matches (used by tests and the
// differential v1-vs-v2 check).
inline bool operator==(const Trade& a, const Trade& b) {
    return a.buyId == b.buyId && a.sellId == b.sellId &&
           a.price == b.price && a.qty == b.qty;
}
}  // namespace ob
