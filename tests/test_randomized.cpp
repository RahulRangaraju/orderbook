#include <gtest/gtest.h>
#include "ob/engine.hpp"
#include "ob/flat_order_book.hpp"
#include "ob/generator.hpp"
#include "ob/id_index.hpp"
#include "ob/order_book.hpp"

#include <random>
#include <vector>

using namespace ob;

namespace {

// A nastier stream than the generator: random (often unknown or already
// filled) ids, duplicate adds, zero-qty modifies, prices on a narrow band so
// almost everything crosses something.
std::vector<Command> chaosStream(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto below = [&](std::uint64_t k) { return rng() % k; };
    std::vector<Command> out;
    out.reserve(n);
    OrderId nextId = 1;
    for (std::size_t i = 0; i < n; ++i) {
        Command c{};
        const auto roll = below(100);
        c.side = below(2) ? Side::Buy : Side::Sell;
        c.price = 1'000 + static_cast<Price>(below(15));
        c.qty = static_cast<Quantity>(below(60));            // includes 0
        if (roll < 55) {
            c.type = CmdType::Add;
            c.id = below(20) == 0 ? 1 + below(nextId) : nextId++;  // ~5% duplicates
        } else if (roll < 85) {
            c.type = CmdType::Cancel;
            c.id = 1 + below(nextId + 10);                   // some never existed
        } else {
            c.type = CmdType::Modify;
            c.id = 1 + below(nextId + 10);
        }
        out.push_back(c);
    }
    return out;
}

template <class Book>
Book makeFor(const std::vector<Command>& cmds) {
    if constexpr (std::is_same_v<Book, FlatOrderBook>) {
        auto [lo, hi] = priceWindow(cmds);
        return FlatOrderBook(lo, hi);
    } else {
        return OrderBook{};
    }
}

// Run cmds, checking every invariant after every single command.
template <class Book>
void checkInvariantsThroughout(const std::vector<Command>& cmds) {
    Book book = makeFor<Book>(cmds);
    std::vector<Trade> trades;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        trades.clear();
        process(book, cmds[i], trades);
        for (const Trade& t : trades) ASSERT_GT(t.qty, 0u) << "command " << i;
        const auto bid = book.bestBid(), ask = book.bestAsk();
        if (bid && ask) {
            ASSERT_LT(*bid, *ask) << "crossed book after command " << i;
        }
        if (bid) {
            ASSERT_GT(book.depthAt(Side::Buy, *bid), 0u) << "empty best bid level, command " << i;
        }
        if (ask) {
            ASSERT_GT(book.depthAt(Side::Sell, *ask), 0u) << "empty best ask level, command " << i;
        }
        ASSERT_TRUE(book.validate()) << "invariant broken after command " << i;
    }
}

// Differential test: v1 and v2 must produce identical trades and identical
// top of book after every command.
void differential(const std::vector<Command>& cmds) {
    OrderBook v1;
    auto [lo, hi] = priceWindow(cmds);
    FlatOrderBook v2(lo, hi, cmds.size());
    std::vector<Trade> t1, t2;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        t1.clear();
        t2.clear();
        process(v1, cmds[i], t1);
        process(v2, cmds[i], t2);
        ASSERT_EQ(t1.size(), t2.size()) << "trade count differs at command " << i;
        for (std::size_t k = 0; k < t1.size(); ++k)
            ASSERT_EQ(t1[k], t2[k]) << "trade " << k << " differs at command " << i;
        ASSERT_EQ(v1.bestBid(), v2.bestBid()) << "best bid differs at command " << i;
        ASSERT_EQ(v1.bestAsk(), v2.bestAsk()) << "best ask differs at command " << i;
        ASSERT_EQ(v1.orderCount(), v2.orderCount()) << "order count differs at command " << i;
    }
    EXPECT_EQ(v2.rejected(), 0u);
}
}  // namespace

// --- Step 4 bonus: 100,000 random commands, invariants after every one. ---

TEST(Randomized, V1InvariantsHoldFor100kGeneratedCommands) {
    checkInvariantsThroughout<OrderBook>(Generator(GenConfig{.seed = 1}).generate(100'000));
}

TEST(Randomized, V1InvariantsHoldFor100kChaosCommands) {
    checkInvariantsThroughout<OrderBook>(chaosStream(100'000, 2));
}

TEST(Randomized, V2InvariantsHoldFor100kGeneratedCommands) {
    checkInvariantsThroughout<FlatOrderBook>(Generator(GenConfig{.seed = 1}).generate(100'000));
}

TEST(Randomized, V2InvariantsHoldFor100kChaosCommands) {
    checkInvariantsThroughout<FlatOrderBook>(chaosStream(100'000, 2));
}

// --- Step 7: differential test, v1 vs v2. ---

TEST(Differential, V1AndV2Agree1MGeneratedCommands) {
    differential(Generator(GenConfig{.seed = 7}).generate(1'000'000));
}

TEST(Differential, V1AndV2Agree300kChaosCommands) {
    differential(chaosStream(300'000, 11));
}

// --- Generator sanity. ---

TEST(Generator, SameSeedSameStream) {
    auto a = Generator(GenConfig{.seed = 5}).generate(10'000);
    auto b = Generator(GenConfig{.seed = 5}).generate(10'000);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i].type, b[i].type);
        ASSERT_EQ(a[i].id, b[i].id);
        ASSERT_EQ(a[i].price, b[i].price);
        ASSERT_EQ(a[i].qty, b[i].qty);
    }
}

TEST(Generator, CommandMixIsRoughly70_25_5) {
    auto cmds = Generator().generate(200'000);
    std::size_t adds = 0, cancels = 0, mods = 0;
    for (const auto& c : cmds) {
        if (c.type == CmdType::Add) ++adds;
        else if (c.type == CmdType::Cancel) ++cancels;
        else ++mods;
    }
    EXPECT_NEAR(adds / 200'000.0, 0.70, 0.01);
    EXPECT_NEAR(cancels / 200'000.0, 0.25, 0.01);
    EXPECT_NEAR(mods / 200'000.0, 0.05, 0.01);
}

// --- Step 9: the dense-vector id index must behave exactly like a hash map,
//     including for huge exchange-style ids that take the fallback path. ---

TEST(IdIndex, DenseAndSparseIdsBehaveTheSame) {
    IdIndex idx;
    const OrderId big = IdIndex::kDenseLimit + 12345;
    EXPECT_FALSE(idx.contains(5));
    EXPECT_FALSE(idx.contains(big));
    idx.insert(5, 50);
    idx.insert(big, 77);
    EXPECT_EQ(idx.find(5), 50u);
    EXPECT_EQ(idx.find(big), 77u);
    EXPECT_EQ(idx.size(), 2u);
    idx.erase(5);
    idx.erase(5);           // erasing twice is harmless
    idx.erase(999'999);     // never inserted
    EXPECT_EQ(idx.find(5), IdIndex::NIL);
    EXPECT_EQ(idx.size(), 1u);
    idx.clear();
    EXPECT_EQ(idx.size(), 0u);
    EXPECT_FALSE(idx.contains(big));
}

TEST(Differential, V1AndV2AgreeWithHugeExchangeStyleIds) {
    // Same chaos stream, but every id is pushed into 64-bit territory (like
    // real exchange order ids), with some left small, so both index paths
    // are exercised in one book.
    auto cmds = chaosStream(200'000, 21);
    for (auto& c : cmds)
        if (c.id % 3 != 0) c.id = c.id * 1'000'003ULL + (OrderId{1} << 40);
    differential(cmds);
}
