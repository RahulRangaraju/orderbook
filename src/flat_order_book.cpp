#include "ob/flat_order_book.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace ob {

FlatOrderBook::FlatOrderBook(Price minTick, Price maxTick, std::size_t expectedOrders)
    : minTick_(minTick), maxTick_(maxTick) {
    if (maxTick < minTick) throw std::invalid_argument("maxTick < minTick");
    const auto n = static_cast<std::size_t>(maxTick - minTick + 1);
    for (BookSide* s : {&bids_, &asks_}) {
        s->levels.assign(n, Level{});
        s->bits.assign((n + 63) / 64, 0);
    }
    pool_.reserve(expectedOrders);
    index_.reserve(expectedOrders);
}

// ---------------------------------------------------------------------------
// Bitmap helpers: bit i set <=> level i on that side has orders.
// ---------------------------------------------------------------------------
void FlatOrderBook::setBit(std::vector<std::uint64_t>& b, std::int64_t i) {
    b[static_cast<std::size_t>(i >> 6)] |= (1ULL << (i & 63));
}
void FlatOrderBook::clearBit(std::vector<std::uint64_t>& b, std::int64_t i) {
    b[static_cast<std::size_t>(i >> 6)] &= ~(1ULL << (i & 63));
}

// Highest set bit at index <= i, or NONE. Used to find the next-best bid.
std::int64_t FlatOrderBook::highestAtOrBelow(const std::vector<std::uint64_t>& b,
                                             std::int64_t i) {
    if (i < 0) return NONE;
    std::int64_t w = i >> 6;
    const int bit = static_cast<int>(i & 63);
    std::uint64_t word = b[static_cast<std::size_t>(w)] &
                         (bit == 63 ? ~0ULL : ((1ULL << (bit + 1)) - 1));
    while (true) {
        if (word) return w * 64 + 63 - std::countl_zero(word);
        if (--w < 0) return NONE;
        word = b[static_cast<std::size_t>(w)];
    }
}

// Lowest set bit at index >= i, or NONE. Used to find the next-best ask.
std::int64_t FlatOrderBook::lowestAtOrAbove(const std::vector<std::uint64_t>& b,
                                            std::int64_t i) {
    const auto words = static_cast<std::int64_t>(b.size());
    std::int64_t w = i >> 6;
    if (w >= words) return NONE;
    std::uint64_t word = b[static_cast<std::size_t>(w)] & (~0ULL << (i & 63));
    while (true) {
        if (word) return w * 64 + std::countr_zero(word);
        if (++w >= words) return NONE;
        word = b[static_cast<std::size_t>(w)];
    }
}

// ---------------------------------------------------------------------------
// Pool: a free list threaded through Node::next. No heap allocation once the
// pool has grown to the book's peak size.
// ---------------------------------------------------------------------------
std::uint32_t FlatOrderBook::allocNode() {
    if (freeHead_ != NIL) {
        const std::uint32_t n = freeHead_;
        freeHead_ = pool_[n].next;
        return n;
    }
    pool_.emplace_back();
    return static_cast<std::uint32_t>(pool_.size() - 1);
}

void FlatOrderBook::freeNode(std::uint32_t n) {
    pool_[n].next = freeHead_;
    freeHead_ = n;
}

// Unlink node n from level i's FIFO and keep totalQty / bitmap / best in sync.
void FlatOrderBook::unlink(BookSide& s, std::int64_t i, std::uint32_t n) {
    Level& L = s.levels[static_cast<std::size_t>(i)];
    Node& node = pool_[n];
    if (node.prev != NIL) pool_[node.prev].next = node.next; else L.head = node.next;
    if (node.next != NIL) pool_[node.next].prev = node.prev; else L.tail = node.prev;
    L.totalQty -= node.o.qty;

    if (L.head == NIL) {                       // level is now empty
        clearBit(s.bits, i);
        if (i == s.best) {                     // scan toward worse prices
            s.best = (&s == &bids_) ? highestAtOrBelow(s.bits, i - 1)
                                    : lowestAtOrAbove(s.bits, i + 1);
        }
    }
}

void FlatOrderBook::removeOrder(std::uint32_t n) {
    const Order& o = pool_[n].o;
    BookSide& s = (o.side == Side::Buy) ? bids_ : asks_;
    const OrderId id = o.id;
    unlink(s, idx(o.price), n);
    index_.erase(id);
    freeNode(n);
}

void FlatOrderBook::rest(OrderId id, Side side, Price px, Quantity qty) {
    BookSide& s = (side == Side::Buy) ? bids_ : asks_;
    const std::int64_t i = idx(px);
    const std::uint32_t n = allocNode();          // may grow pool_: take refs after
    Level& L = s.levels[static_cast<std::size_t>(i)];

    Node& node = pool_[n];
    node.o = Order{id, side, px, qty, nextSeq_++};
    node.prev = L.tail;
    node.next = NIL;
    if (L.tail != NIL) pool_[L.tail].next = n; else L.head = n;
    L.tail = n;
    L.totalQty += qty;

    setBit(s.bits, i);
    if (side == Side::Buy) {
        if (s.best == NONE || i > s.best) s.best = i;
    } else {
        if (s.best == NONE || i < s.best) s.best = i;
    }
    index_.insert(id, n);
}

// ---------------------------------------------------------------------------
// Matching, templated on the incoming side so the hot loop has no buy/sell
// branches. A buy eats asks from the lowest index up; a sell eats bids from
// the highest index down.
// ---------------------------------------------------------------------------
template <Side S>
void FlatOrderBook::match(OrderId id, Price px, Quantity& remaining,
                          std::vector<Trade>& out) {
    BookSide& opp = (S == Side::Buy) ? asks_ : bids_;
    while (remaining > 0 && opp.best != NONE) {
        const Price levelPx = priceAt(opp.best);
        if constexpr (S == Side::Buy) { if (levelPx > px) break; }
        else                          { if (levelPx < px) break; }

        const std::int64_t i = opp.best;
        Level& L = opp.levels[static_cast<std::size_t>(i)];
        while (remaining > 0 && L.head != NIL) {
            const std::uint32_t n = L.head;
            Order& resting = pool_[n].o;
            const Quantity fill = std::min(remaining, resting.qty);
            if constexpr (S == Side::Buy)
                out.push_back(Trade{id, resting.id, levelPx, fill});
            else
                out.push_back(Trade{resting.id, id, levelPx, fill});
            remaining -= fill;
            if (fill == resting.qty) {
                removeOrder(n);                    // also advances opp.best if empty
            } else {
                resting.qty -= fill;
                L.totalQty -= fill;
            }
        }
    }
}

void FlatOrderBook::add(OrderId id, Side side, Price px, Quantity qty,
                        std::vector<Trade>& out) {
    if (qty == 0 || index_.contains(id)) return;
    if (!inWindow(px)) { ++rejected_; return; }

    Quantity remaining = qty;
    if (side == Side::Buy) match<Side::Buy>(id, px, remaining, out);
    else                   match<Side::Sell>(id, px, remaining, out);
    if (remaining > 0) rest(id, side, px, remaining);
}

std::vector<Trade> FlatOrderBook::add(OrderId id, Side side, Price px, Quantity qty) {
    std::vector<Trade> out;
    add(id, side, px, qty, out);
    return out;
}

bool FlatOrderBook::cancel(OrderId id) {
    const std::uint32_t n = index_.find(id);
    if (n == IdIndex::NIL) return false;
    removeOrder(n);
    return true;
}

bool FlatOrderBook::reduce(OrderId id, Quantity by) {
    const std::uint32_t n = index_.find(id);
    if (n == IdIndex::NIL) return false;
    Order& o = pool_[n].o;
    if (by >= o.qty) { removeOrder(n); return true; }
    BookSide& s = (o.side == Side::Buy) ? bids_ : asks_;
    s.levels[static_cast<std::size_t>(idx(o.price))].totalQty -= by;
    o.qty -= by;
    return true;
}

void FlatOrderBook::modify(OrderId id, Price px, Quantity qty, std::vector<Trade>& out) {
    const std::uint32_t n = index_.find(id);
    if (n == IdIndex::NIL) return;
    if (qty == 0) { removeOrder(n); return; }
    if (!inWindow(px)) { ++rejected_; return; }  // leave the order untouched

    Order& o = pool_[n].o;
    if (px == o.price && qty <= o.qty) {         // shrink in place, keep priority
        reduce(id, o.qty - qty);
        return;
    }
    const Side side = o.side;
    removeOrder(n);
    add(id, side, px, qty, out);
}

std::vector<Trade> FlatOrderBook::modify(OrderId id, Price px, Quantity qty) {
    std::vector<Trade> out;
    modify(id, px, qty, out);
    return out;
}

void FlatOrderBook::clear() {
    for (BookSide* s : {&bids_, &asks_}) {
        std::fill(s->levels.begin(), s->levels.end(), Level{});
        std::fill(s->bits.begin(), s->bits.end(), 0);
        s->best = NONE;
    }
    pool_.clear();
    freeHead_ = NIL;
    index_.clear();
}

std::optional<Price> FlatOrderBook::bestBid() const {
    if (bids_.best == NONE) return std::nullopt;
    return priceAt(bids_.best);
}

std::optional<Price> FlatOrderBook::bestAsk() const {
    if (asks_.best == NONE) return std::nullopt;
    return priceAt(asks_.best);
}

Quantity FlatOrderBook::depthAt(Side side, Price px) const {
    if (!inWindow(px)) return 0;
    const BookSide& s = (side == Side::Buy) ? bids_ : asks_;
    return static_cast<Quantity>(s.levels[static_cast<std::size_t>(idx(px))].totalQty);
}

// Same invariants as OrderBook::validate, plus the v2 bookkeeping:
// totalQty, bitmap bits, best indexes, prev/next links and index_ slots.
bool FlatOrderBook::validate() const {
    std::size_t resting = 0;
    auto checkSide = [&](const BookSide& s, Side side) {
        std::int64_t expectBest = NONE;
        for (std::size_t i = 0; i < s.levels.size(); ++i) {
            const Level& L = s.levels[i];
            const bool bit = (s.bits[i >> 6] >> (i & 63)) & 1;
            if ((L.head == NIL) != (L.tail == NIL)) return false;
            if ((L.head != NIL) != bit) return false;
            if (L.head == NIL) { if (L.totalQty != 0) return false; continue; }

            if (side == Side::Buy || expectBest == NONE)
                expectBest = static_cast<std::int64_t>(i);

            std::uint64_t total = 0, lastSeq = 0;
            std::uint32_t prev = NIL;
            for (std::uint32_t n = L.head; n != NIL; n = pool_[n].next) {
                const Node& node = pool_[n];
                if (node.prev != prev) return false;
                if (node.o.qty == 0 || node.o.side != side ||
                    idx(node.o.price) != static_cast<std::int64_t>(i))
                    return false;
                if (prev != NIL && node.o.seq <= lastSeq) return false;
                if (index_.find(node.o.id) != n) return false;
                lastSeq = node.o.seq;
                total += node.o.qty;
                prev = n;
                ++resting;
            }
            if (prev != L.tail || total != L.totalQty) return false;
        }
        return expectBest == s.best;
    };
    if (!checkSide(bids_, Side::Buy) || !checkSide(asks_, Side::Sell)) return false;
    if (resting != index_.size()) return false;
    if (bids_.best != NONE && asks_.best != NONE && bids_.best >= asks_.best) return false;
    return true;
}

}  // namespace ob
