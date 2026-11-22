#pragma once
#include "ob/id_index.hpp"
#include "ob/types.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace ob {

// v2: same behaviour as OrderBook (v1), built from contiguous arrays.
//
//   * Price levels are a vector indexed by (price - minTick): one array access
//     per lookup instead of a red-black-tree walk.
//   * Orders live in a pool (vector<Node>) with a free list: no new/delete per
//     order. Each Node has prev/next pool indexes, forming an intrusive doubly
//     linked FIFO inside its level ("append at tail, match from head").
//   * id -> pool slot index (IdIndex: dense vector, hash-map fallback), so
//     cancel unlinks in O(1) without searching the queue.
//   * A bitmap of non-empty levels per side makes "find the next best price"
//     a few word scans instead of walking empty levels one by one.
//
// Prices outside [minTick, maxTick] are rejected and counted in rejected().
class FlatOrderBook {
public:
    FlatOrderBook(Price minTick, Price maxTick, std::size_t expectedOrders = 1 << 16);

    std::vector<Trade> add(OrderId id, Side side, Price px, Quantity qty);
    bool cancel(OrderId id);
    std::vector<Trade> modify(OrderId id, Price px, Quantity qty);

    void add(OrderId id, Side side, Price px, Quantity qty, std::vector<Trade>& out);
    void modify(OrderId id, Price px, Quantity qty, std::vector<Trade>& out);

    bool reduce(OrderId id, Quantity by);
    void clear();

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Quantity depthAt(Side side, Price px) const;
    std::size_t orderCount() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.contains(id); }

    std::uint64_t rejected() const { return rejected_; }  // out-of-window prices
    Price minTick() const { return minTick_; }
    Price maxTick() const { return maxTick_; }

    bool validate() const;

private:
    static constexpr std::uint32_t NIL = UINT32_MAX;
    static constexpr std::int64_t NONE = -1;  // "no best price on this side"

    struct Node  { Order o; std::uint32_t prev, next; };
    struct Level { std::uint32_t head = NIL, tail = NIL; std::uint64_t totalQty = 0; };

    // One side of the book: its levels, its non-empty bitmap, its best index.
    struct BookSide {
        std::vector<Level> levels;
        std::vector<std::uint64_t> bits;
        std::int64_t best = NONE;
    };

    Price minTick_, maxTick_;
    BookSide bids_, asks_;
    std::vector<Node> pool_;
    std::uint32_t freeHead_ = NIL;
    IdIndex index_;
    std::uint64_t nextSeq_ = 0;
    std::uint64_t rejected_ = 0;

    bool inWindow(Price px) const { return px >= minTick_ && px <= maxTick_; }
    std::int64_t idx(Price px) const { return px - minTick_; }
    Price priceAt(std::int64_t i) const { return minTick_ + i; }

    std::uint32_t allocNode();
    void freeNode(std::uint32_t n);
    void unlink(BookSide& s, std::int64_t i, std::uint32_t n);
    void rest(OrderId id, Side side, Price px, Quantity qty);
    void removeOrder(std::uint32_t n);  // unlink + free + index erase

    template <Side S>
    void match(OrderId id, Price px, Quantity& remaining, std::vector<Trade>& out);

    static void setBit(std::vector<std::uint64_t>& b, std::int64_t i);
    static void clearBit(std::vector<std::uint64_t>& b, std::int64_t i);
    static std::int64_t highestAtOrBelow(const std::vector<std::uint64_t>& b, std::int64_t i);
    static std::int64_t lowestAtOrAbove(const std::vector<std::uint64_t>& b, std::int64_t i);
};
}  // namespace ob
