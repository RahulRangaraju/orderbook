#pragma once
#include "ob/types.hpp"
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace ob {

// Run one command through any book type (v1 OrderBook or v2 FlatOrderBook).
// Trades are appended to `out`; the caller decides when to clear it.
template <class Book>
inline void process(Book& book, const Command& c, std::vector<Trade>& out) {
    switch (c.type) {
        case CmdType::Add:    book.add(c.id, c.side, c.price, c.qty, out); break;
        case CmdType::Cancel: book.cancel(c.id); break;
        case CmdType::Modify: book.modify(c.id, c.price, c.qty, out); break;
    }
}

// v2 needs a price window up front. Per the guide: the data's low minus 10%
// to its high plus 10%, in ticks. Cancels carry no price, so skip them.
inline std::pair<Price, Price> priceWindow(const std::vector<Command>& cmds) {
    Price lo = std::numeric_limits<Price>::max();
    Price hi = std::numeric_limits<Price>::min();
    for (const Command& c : cmds) {
        if (c.type == CmdType::Cancel) continue;
        lo = std::min(lo, c.price);
        hi = std::max(hi, c.price);
    }
    if (lo > hi) return {0, 1};  // no priced commands
    const Price pad = std::max<Price>(1, (hi - lo) / 10);
    return {std::max<Price>(0, lo - lo / 10 - pad), hi + hi / 10 + pad};
}
}  // namespace ob
