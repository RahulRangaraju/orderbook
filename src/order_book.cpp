#include "ob/order_book.hpp"

#include <algorithm>

namespace ob {

// ---------------------------------------------------------------------------
// Best prices: the first key of each map. bids_ is sorted high->low, asks_
// low->high, so begin() is always the best price.
// ---------------------------------------------------------------------------
std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::depthAt(Side side, Price px) const {
    auto sum = [](const std::deque<Order>& q) {
        Quantity total = 0;
        for (const Order& o : q) total += o.qty;
        return total;
    };
    if (side == Side::Buy) {
        auto it = bids_.find(px);
        return it == bids_.end() ? 0 : sum(it->second);
    }
    auto it = asks_.find(px);
    return it == asks_.end() ? 0 : sum(it->second);
}

// ---------------------------------------------------------------------------
// Matching. One template handles both sides:
//   * a buy walks asks_ (lowest first) while  ask <= px
//   * a sell walks bids_ (highest first) while bid >= px
// Inside a level, the front of the deque is the oldest order (time priority).
// Every trade prints at the resting order's price.
// ---------------------------------------------------------------------------
template <class OppMap, class Crosses>
void OrderBook::matchAgainst(OppMap& opp, Side side, OrderId id, Price px,
                             Quantity& remaining, Crosses crosses,
                             std::vector<Trade>& out) {
    while (remaining > 0 && !opp.empty() && crosses(opp.begin()->first, px)) {
        auto level = opp.begin();
        auto& queue = level->second;
        while (remaining > 0 && !queue.empty()) {
            Order& resting = queue.front();
            const Quantity fill = std::min(remaining, resting.qty);
            if (side == Side::Buy)
                out.push_back(Trade{id, resting.id, level->first, fill});
            else
                out.push_back(Trade{resting.id, id, level->first, fill});
            remaining -= fill;
            resting.qty -= fill;
            if (resting.qty == 0) {
                index_.erase(resting.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) opp.erase(level);  // Gotcha: never leave empty levels
    }
}

void OrderBook::rest(OrderId id, Side side, Price px, Quantity qty) {
    Order o{id, side, px, qty, nextSeq_++};
    if (side == Side::Buy)
        bids_[px].push_back(o);
    else
        asks_[px].push_back(o);
    index_.emplace(id, Locator{side, px});
}

void OrderBook::add(OrderId id, Side side, Price px, Quantity qty,
                    std::vector<Trade>& out) {
    if (qty == 0 || index_.count(id)) return;  // reject zero qty / duplicate id

    Quantity remaining = qty;
    if (side == Side::Buy)
        matchAgainst(asks_, side, id, px, remaining,
                     [](Price ask, Price p) { return ask <= p; }, out);
    else
        matchAgainst(bids_, side, id, px, remaining,
                     [](Price bid, Price p) { return bid >= p; }, out);

    if (remaining > 0) rest(id, side, px, remaining);
}

std::vector<Trade> OrderBook::add(OrderId id, Side side, Price px, Quantity qty) {
    std::vector<Trade> out;
    add(id, side, px, qty, out);
    return out;
}

// ---------------------------------------------------------------------------
// Cancel: locator -> level -> linear search of that one deque (v1 only; v2
// makes this O(1)).
// ---------------------------------------------------------------------------
Order* OrderBook::find(OrderId id, Locator loc) {
    auto search = [id](std::deque<Order>& q) -> Order* {
        auto it = std::find_if(q.begin(), q.end(),
                               [id](const Order& o) { return o.id == id; });
        return it == q.end() ? nullptr : &*it;
    };
    if (loc.side == Side::Buy) {
        auto lvl = bids_.find(loc.price);
        return lvl == bids_.end() ? nullptr : search(lvl->second);
    }
    auto lvl = asks_.find(loc.price);
    return lvl == asks_.end() ? nullptr : search(lvl->second);
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Locator loc = it->second;

    auto eraseFrom = [id](auto& book, Price px) {
        auto lvl = book.find(px);
        auto& q = lvl->second;
        q.erase(std::find_if(q.begin(), q.end(),
                             [id](const Order& o) { return o.id == id; }));
        if (q.empty()) book.erase(lvl);
    };
    if (loc.side == Side::Buy)
        eraseFrom(bids_, loc.price);
    else
        eraseFrom(asks_, loc.price);

    index_.erase(it);
    return true;
}

bool OrderBook::reduce(OrderId id, Quantity by) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = find(id, it->second);
    if (by >= o->qty) return cancel(id);
    o->qty -= by;
    return true;
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
}

// ---------------------------------------------------------------------------
// Modify rule (same as real exchanges):
//   * same price, lower quantity  -> shrink in place, keep queue position
//   * new price or higher qty     -> cancel + add (back of the queue, may trade)
//   * qty 0                       -> cancel
// ---------------------------------------------------------------------------
void OrderBook::modify(OrderId id, Price px, Quantity qty, std::vector<Trade>& out) {
    auto it = index_.find(id);
    if (it == index_.end()) return;
    if (qty == 0) {
        cancel(id);
        return;
    }
    const Locator loc = it->second;
    Order* o = find(id, loc);
    if (px == loc.price && qty <= o->qty) {
        o->qty = qty;
        return;
    }
    cancel(id);
    add(id, loc.side, px, qty, out);
}

std::vector<Trade> OrderBook::modify(OrderId id, Price px, Quantity qty) {
    std::vector<Trade> out;
    modify(id, px, qty, out);
    return out;
}

// ---------------------------------------------------------------------------
// Invariants (tests only):
//   * no empty price levels and no zero-qty orders
//   * every resting order is in index_ with the right locator, and vice versa
//   * the book is never crossed: best bid < best ask
//   * seq strictly increases inside each level (FIFO)
// ---------------------------------------------------------------------------
bool OrderBook::validate() const {
    std::size_t resting = 0;
    auto checkSide = [&](const auto& book, Side side) {
        for (const auto& [px, q] : book) {
            if (q.empty()) return false;
            std::uint64_t lastSeq = 0;
            bool first = true;
            for (const Order& o : q) {
                if (o.qty == 0 || o.price != px || o.side != side) return false;
                if (!first && o.seq <= lastSeq) return false;
                lastSeq = o.seq;
                first = false;
                auto it = index_.find(o.id);
                if (it == index_.end() || it->second.side != side ||
                    it->second.price != px)
                    return false;
                ++resting;
            }
        }
        return true;
    };
    if (!checkSide(bids_, Side::Buy) || !checkSide(asks_, Side::Sell)) return false;
    if (resting != index_.size()) return false;
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
        return false;
    return true;
}

}  // namespace ob
