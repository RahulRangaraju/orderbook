#pragma once
#include "ob/types.hpp"
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

// v1: the simple, obviously-correct book. Price levels live in std::map
// (a red-black tree), each level is a FIFO std::deque of orders.
// v2 (FlatOrderBook) must behave exactly like this class.
class OrderBook {
public:
    // Returns trades produced by this command (empty if none).
    std::vector<Trade> add(OrderId id, Side side, Price px, Quantity qty);
    bool cancel(OrderId id);                      // false if unknown/already filled
    std::vector<Trade> modify(OrderId id, Price px, Quantity qty);

    // Same as above, but append trades to a caller-owned buffer so the hot
    // path doesn't allocate a new vector per order (see Step 9).
    void add(OrderId id, Side side, Price px, Quantity qty, std::vector<Trade>& out);
    void modify(OrderId id, Price px, Quantity qty, std::vector<Trade>& out);

    // Remove `by` shares from a resting order without losing queue position
    // (used for partial cancels in Databento data). Cancels it if it hits 0.
    bool reduce(OrderId id, Quantity by);
    void clear();                                 // remove every resting order

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Quantity depthAt(Side side, Price px) const;  // total qty resting at a level
    std::size_t orderCount() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.count(id) != 0; }

    // Walks the whole book and checks every invariant. Slow; tests only.
    bool validate() const;

private:
    struct Locator { Side side; Price price; };
    std::map<Price, std::deque<Order>, std::greater<Price>> bids_;
    std::map<Price, std::deque<Order>> asks_;
    std::unordered_map<OrderId, Locator> index_;
    std::uint64_t nextSeq_ = 0;

    template <class OppMap, class Crosses>
    void matchAgainst(OppMap& opp, Side side, OrderId id, Price px,
                      Quantity& remaining, Crosses crosses, std::vector<Trade>& out);
    void rest(OrderId id, Side side, Price px, Quantity qty);
    Order* find(OrderId id, Locator loc);
};
}  // namespace ob
