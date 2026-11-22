#include <gtest/gtest.h>
#include "ob/flat_order_book.hpp"
#include "ob/order_book.hpp"
using namespace ob;

// Every test in this file runs twice: once against v1 (OrderBook, std::map)
// and once against v2 (FlatOrderBook, flat arrays). If v2 passes the same
// tests as v1, it behaves the same.
template <class Book> Book makeBook();
template <> OrderBook makeBook<OrderBook>() { return OrderBook{}; }
template <> FlatOrderBook makeBook<FlatOrderBook>() { return FlatOrderBook(0, 20'000); }

template <class Book>
class BookTest : public ::testing::Test {
protected:
    Book book = makeBook<Book>();
};

using Books = ::testing::Types<OrderBook, FlatOrderBook>;
TYPED_TEST_SUITE(BookTest, Books);

// ---------------------------------------------------------------- basics ---

TYPED_TEST(BookTest, EmptyBookHasNoBestBidOrAsk) {
    EXPECT_FALSE(this->book.bestBid().has_value());
    EXPECT_FALSE(this->book.bestAsk().has_value());
    EXPECT_EQ(this->book.orderCount(), 0u);
}

TYPED_TEST(BookTest, NonCrossingOrdersRest) {
    EXPECT_TRUE(this->book.add(1, Side::Buy, 100, 10).empty());
    EXPECT_TRUE(this->book.add(2, Side::Sell, 102, 20).empty());
    EXPECT_EQ(this->book.bestBid(), 100);
    EXPECT_EQ(this->book.bestAsk(), 102);
    EXPECT_EQ(this->book.orderCount(), 2u);
}

TYPED_TEST(BookTest, BestBidIsHighestBuy) {
    this->book.add(1, Side::Buy, 98, 10);
    this->book.add(2, Side::Buy, 100, 10);
    this->book.add(3, Side::Buy, 99, 10);
    EXPECT_EQ(this->book.bestBid(), 100);
}

TYPED_TEST(BookTest, BestAskIsLowestSell) {
    this->book.add(1, Side::Sell, 104, 10);
    this->book.add(2, Side::Sell, 102, 10);
    this->book.add(3, Side::Sell, 103, 10);
    EXPECT_EQ(this->book.bestAsk(), 102);
}

TYPED_TEST(BookTest, DepthAtSumsAllOrdersAtLevel) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.add(2, Side::Buy, 100, 15);
    this->book.add(3, Side::Buy, 99, 7);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 100), 25u);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 99), 7u);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 98), 0u);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 100), 0u);
}

TYPED_TEST(BookTest, DuplicateIdIsRejected) {
    this->book.add(1, Side::Buy, 100, 10);
    EXPECT_TRUE(this->book.add(1, Side::Sell, 100, 10).empty());  // would have crossed
    EXPECT_EQ(this->book.orderCount(), 1u);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 100), 10u);
}

TYPED_TEST(BookTest, ZeroQuantityIsRejected) {
    EXPECT_TRUE(this->book.add(1, Side::Buy, 100, 0).empty());
    EXPECT_EQ(this->book.orderCount(), 0u);
    EXPECT_FALSE(this->book.bestBid().has_value());
}

// -------------------------------------------------------------- matching ---

TYPED_TEST(BookTest, ExactMatchFillsBothAndEmptiesBook) {
    this->book.add(1, Side::Sell, 101, 50);
    auto trades = this->book.add(2, Side::Buy, 101, 50);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 101, 50}));
    EXPECT_EQ(this->book.orderCount(), 0u);
    EXPECT_FALSE(this->book.bestBid().has_value());
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

TYPED_TEST(BookTest, PartialFillLeavesRemainderResting) {  // the guide's example
    this->book.add(1, Side::Sell, 101, 100);
    auto trades = this->book.add(2, Side::Buy, 101, 30);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[0].qty, 30u);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 70u);
    EXPECT_FALSE(this->book.bestBid().has_value());
}

TYPED_TEST(BookTest, IncomingLargerThanRestingRemainderRests) {
    this->book.add(1, Side::Sell, 101, 30);
    auto trades = this->book.add(2, Side::Buy, 101, 50);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{2, 1, 101, 30}));
    EXPECT_EQ(this->book.bestBid(), 101);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 101), 20u);
    EXPECT_FALSE(this->book.bestAsk().has_value());
}

// The worked example from Step 3, line by line.
TYPED_TEST(BookTest, Step3WorkedExampleSweepsThreeOrders) {
    auto& b = this->book;
    EXPECT_TRUE(b.add(1, Side::Sell, 101, 100).empty());
    EXPECT_EQ(b.bestAsk(), 101);
    EXPECT_TRUE(b.add(2, Side::Sell, 101, 50).empty());
    EXPECT_TRUE(b.add(3, Side::Sell, 102, 70).empty());
    EXPECT_EQ(b.bestAsk(), 101);

    auto trades = b.add(4, Side::Buy, 102, 180);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0], (Trade{4, 1, 101, 100}));
    EXPECT_EQ(trades[1], (Trade{4, 2, 101, 50}));
    EXPECT_EQ(trades[2], (Trade{4, 3, 102, 30}));
    EXPECT_EQ(b.depthAt(Side::Sell, 102), 40u);
    EXPECT_EQ(b.depthAt(Side::Sell, 101), 0u);
    EXPECT_EQ(b.bestAsk(), 102);
    EXPECT_FALSE(b.bestBid().has_value());

    EXPECT_TRUE(b.add(5, Side::Buy, 100, 10).empty());
    EXPECT_EQ(b.bestBid(), 100);
    EXPECT_EQ(b.orderCount(), 2u);
}

TYPED_TEST(BookTest, SweepStopsAtLimitPrice) {
    this->book.add(1, Side::Sell, 101, 10);
    this->book.add(2, Side::Sell, 102, 10);
    this->book.add(3, Side::Sell, 103, 10);
    auto trades = this->book.add(4, Side::Buy, 102, 100);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(this->book.bestBid(), 102);
    EXPECT_EQ(this->book.depthAt(Side::Buy, 102), 80u);
    EXPECT_EQ(this->book.bestAsk(), 103);
}

TYPED_TEST(BookTest, TimePriorityOldestFillsFirst) {
    this->book.add(1, Side::Sell, 101, 30);
    this->book.add(2, Side::Sell, 101, 30);
    auto trades = this->book.add(3, Side::Buy, 101, 40);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{3, 1, 101, 30}));
    EXPECT_EQ(trades[1], (Trade{3, 2, 101, 10}));
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 20u);
}

TYPED_TEST(BookTest, TradePriceIsRestingOrdersPrice) {
    this->book.add(1, Side::Sell, 101, 10);
    auto trades = this->book.add(2, Side::Buy, 105, 10);  // willing to pay 105
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 101);                      // but trades at 101
}

// ------------------------------------------------- sell side mirrors buy ---

TYPED_TEST(BookTest, SellSweepsThreeBidLevels) {
    auto& b = this->book;
    b.add(1, Side::Buy, 101, 100);
    b.add(2, Side::Buy, 100, 50);
    b.add(3, Side::Buy, 99, 70);
    auto trades = b.add(4, Side::Sell, 99, 180);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0], (Trade{1, 4, 101, 100}));
    EXPECT_EQ(trades[1], (Trade{2, 4, 100, 50}));
    EXPECT_EQ(trades[2], (Trade{3, 4, 99, 30}));
    EXPECT_EQ(b.depthAt(Side::Buy, 99), 40u);
    EXPECT_EQ(b.bestBid(), 99);
    EXPECT_FALSE(b.bestAsk().has_value());
}

TYPED_TEST(BookTest, SellTimePriorityOldestFillsFirst) {
    this->book.add(1, Side::Buy, 100, 30);
    this->book.add(2, Side::Buy, 100, 30);
    auto trades = this->book.add(3, Side::Sell, 100, 40);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{1, 3, 100, 30}));
    EXPECT_EQ(trades[1], (Trade{2, 3, 100, 10}));
}

TYPED_TEST(BookTest, SellTradePriceIsRestingBid) {
    this->book.add(1, Side::Buy, 105, 10);
    auto trades = this->book.add(2, Side::Sell, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 105);
}

TYPED_TEST(BookTest, SellLargerThanBidRemainderRestsAsAsk) {
    this->book.add(1, Side::Buy, 100, 30);
    auto trades = this->book.add(2, Side::Sell, 100, 50);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{1, 2, 100, 30}));
    EXPECT_EQ(this->book.bestAsk(), 100);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 100), 20u);
    EXPECT_FALSE(this->book.bestBid().has_value());
}

// ---------------------------------------------------------------- cancel ---

TYPED_TEST(BookTest, CancelRestingOrderRemovesEmptyLevel) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.add(2, Side::Buy, 99, 10);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.depthAt(Side::Buy, 100), 0u);
    EXPECT_EQ(this->book.bestBid(), 99);  // level 100 is gone, not "empty but best"
    EXPECT_EQ(this->book.orderCount(), 1u);
}

TYPED_TEST(BookTest, CancelReducesDepthButKeepsLevel) {
    this->book.add(1, Side::Sell, 101, 10);
    this->book.add(2, Side::Sell, 101, 15);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 15u);
    EXPECT_EQ(this->book.bestAsk(), 101);
}

TYPED_TEST(BookTest, CancelMiddleOfQueueKeepsOthersInOrder) {
    this->book.add(1, Side::Sell, 101, 10);
    this->book.add(2, Side::Sell, 101, 10);
    this->book.add(3, Side::Sell, 101, 10);
    EXPECT_TRUE(this->book.cancel(2));
    auto trades = this->book.add(4, Side::Buy, 101, 20);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sellId, 1u);
    EXPECT_EQ(trades[1].sellId, 3u);
}

TYPED_TEST(BookTest, CancelUnknownIdReturnsFalse) {
    EXPECT_FALSE(this->book.cancel(42));
    this->book.add(1, Side::Buy, 100, 10);
    EXPECT_FALSE(this->book.cancel(42));
    EXPECT_EQ(this->book.orderCount(), 1u);
}

TYPED_TEST(BookTest, CancelFilledOrderReturnsFalse) {
    this->book.add(1, Side::Sell, 101, 10);
    this->book.add(2, Side::Buy, 101, 10);  // fills id 1 completely
    EXPECT_FALSE(this->book.cancel(1));
    EXPECT_FALSE(this->book.cancel(2));     // the buy filled too, never rested
}

TYPED_TEST(BookTest, CancelSameIdTwiceSecondReturnsFalse) {
    this->book.add(1, Side::Buy, 100, 10);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.cancel(1));
}

TYPED_TEST(BookTest, IdCanBeReusedAfterCancel) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.cancel(1);
    this->book.add(1, Side::Sell, 105, 5);
    EXPECT_EQ(this->book.bestAsk(), 105);
    EXPECT_FALSE(this->book.bestBid().has_value());
}

// ---------------------------------------------------------------- modify ---

TYPED_TEST(BookTest, ModifyDownInQuantityKeepsQueuePosition) {
    this->book.add(1, Side::Sell, 101, 100);
    this->book.add(2, Side::Sell, 101, 100);
    EXPECT_TRUE(this->book.modify(1, 101, 40).empty());
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 140u);

    auto trades = this->book.add(3, Side::Buy, 101, 50);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{3, 1, 101, 40}));  // id 1 still first in line
    EXPECT_EQ(trades[1], (Trade{3, 2, 101, 10}));
}

TYPED_TEST(BookTest, ModifyPriceMovesOrderToBackOfNewLevel) {
    this->book.add(1, Side::Sell, 101, 100);
    this->book.add(2, Side::Sell, 102, 100);
    EXPECT_TRUE(this->book.modify(1, 102, 100).empty());
    EXPECT_EQ(this->book.bestAsk(), 102);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 0u);

    auto trades = this->book.add(3, Side::Buy, 102, 150);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0], (Trade{3, 2, 102, 100}));  // id 2 was there first
    EXPECT_EQ(trades[1], (Trade{3, 1, 102, 50}));
}

TYPED_TEST(BookTest, ModifyUpInQuantityLosesQueuePosition) {
    this->book.add(1, Side::Sell, 101, 100);
    this->book.add(2, Side::Sell, 101, 100);
    this->book.modify(1, 101, 150);
    auto trades = this->book.add(3, Side::Buy, 101, 100);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellId, 2u);
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 150u);
}

TYPED_TEST(BookTest, ModifyToCrossingPriceTrades) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.add(2, Side::Sell, 105, 10);
    auto trades = this->book.modify(2, 100, 10);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{1, 2, 100, 10}));
    EXPECT_EQ(this->book.orderCount(), 0u);
}

TYPED_TEST(BookTest, ModifyUnknownIdDoesNothing) {
    this->book.add(1, Side::Buy, 100, 10);
    EXPECT_TRUE(this->book.modify(99, 101, 5).empty());
    EXPECT_EQ(this->book.orderCount(), 1u);
    EXPECT_EQ(this->book.bestBid(), 100);
}

TYPED_TEST(BookTest, ModifyToZeroQuantityCancels) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.modify(1, 100, 0);
    EXPECT_EQ(this->book.orderCount(), 0u);
    EXPECT_FALSE(this->book.bestBid().has_value());
}

// -------------------------------------------------------- reduce / clear ---

TYPED_TEST(BookTest, ReducePartialKeepsPositionFullRemoves) {
    this->book.add(1, Side::Sell, 101, 50);
    this->book.add(2, Side::Sell, 101, 50);
    EXPECT_TRUE(this->book.reduce(1, 20));
    EXPECT_EQ(this->book.depthAt(Side::Sell, 101), 80u);
    auto trades = this->book.add(3, Side::Buy, 101, 30);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{3, 1, 101, 30}));   // id 1 kept its place

    EXPECT_TRUE(this->book.reduce(2, 500));          // more than it has: removed
    EXPECT_FALSE(this->book.bestAsk().has_value());
    EXPECT_FALSE(this->book.reduce(2, 1));
}

TYPED_TEST(BookTest, ClearEmptiesEverything) {
    this->book.add(1, Side::Buy, 100, 10);
    this->book.add(2, Side::Sell, 101, 10);
    this->book.clear();
    EXPECT_EQ(this->book.orderCount(), 0u);
    EXPECT_FALSE(this->book.bestBid().has_value());
    EXPECT_FALSE(this->book.bestAsk().has_value());
    this->book.add(3, Side::Buy, 99, 5);              // still usable afterwards
    EXPECT_EQ(this->book.bestBid(), 99);
}

TYPED_TEST(BookTest, ValidateHoldsAfterMixedActivity) {
    auto& b = this->book;
    b.add(1, Side::Buy, 100, 10);
    b.add(2, Side::Sell, 103, 10);
    b.add(3, Side::Buy, 101, 10);
    b.add(4, Side::Sell, 101, 15);   // fills 3, rests 5 @ 101
    b.modify(1, 100, 4);
    b.cancel(2);
    EXPECT_TRUE(b.validate());
    EXPECT_EQ(b.bestAsk(), 101);
    EXPECT_EQ(b.bestBid(), 100);
}

// ------------------------------------------------------------- v2 only -----

TEST(FlatOrderBook, PricesOutsideWindowAreRejectedAndCounted) {
    FlatOrderBook b(100, 200);
    EXPECT_TRUE(b.add(1, Side::Buy, 99, 10).empty());
    EXPECT_TRUE(b.add(2, Side::Sell, 201, 10).empty());
    EXPECT_EQ(b.rejected(), 2u);
    EXPECT_EQ(b.orderCount(), 0u);
    b.add(3, Side::Buy, 100, 10);   // both edges are inside
    b.add(4, Side::Sell, 200, 10);
    EXPECT_EQ(b.bestBid(), 100);
    EXPECT_EQ(b.bestAsk(), 200);
}

TEST(FlatOrderBook, ModifyOutsideWindowLeavesOrderUntouched) {
    FlatOrderBook b(100, 200);
    b.add(1, Side::Buy, 150, 10);
    b.modify(1, 500, 10);
    EXPECT_EQ(b.rejected(), 1u);
    EXPECT_EQ(b.bestBid(), 150);
    EXPECT_EQ(b.depthAt(Side::Buy, 150), 10u);
}

TEST(FlatOrderBook, BestPriceScansAcrossBitmapWords) {
    FlatOrderBook b(0, 1000);
    b.add(1, Side::Buy, 3, 1);       // word 0
    b.add(2, Side::Buy, 700, 1);     // word 10
    b.add(3, Side::Sell, 64, 1);     // exactly the start of word 1 ... (would cross 700)
    // id 3 crossed the 700 bid, so the book is: bid 3, nothing else.
    EXPECT_EQ(b.bestBid(), 3);
    EXPECT_FALSE(b.bestAsk().has_value());

    b.add(4, Side::Sell, 999, 1);
    b.add(5, Side::Sell, 128, 1);
    b.add(6, Side::Sell, 63, 1);     // last bit of word 0
    EXPECT_EQ(b.bestAsk(), 63);
    b.cancel(6);
    EXPECT_EQ(b.bestAsk(), 128);
    b.cancel(5);
    EXPECT_EQ(b.bestAsk(), 999);
    b.cancel(4);
    EXPECT_FALSE(b.bestAsk().has_value());
    b.cancel(1);
    EXPECT_FALSE(b.bestBid().has_value());
    EXPECT_TRUE(b.validate());
}

TEST(FlatOrderBook, PoolSlotsAreReused) {
    FlatOrderBook b(0, 100, 4);
    for (OrderId id = 1; id <= 10'000; ++id) {
        b.add(id, Side::Buy, 50, 1);
        b.cancel(id);
    }
    EXPECT_EQ(b.orderCount(), 0u);
    EXPECT_TRUE(b.validate());
}
