#include <gtest/gtest.h>
#include "ob/flat_order_book.hpp"
#include "ob/generator.hpp"
#include "ob/order_book.hpp"
#include "ob/parser.hpp"
#include "ob/pipeline.hpp"
#include "ob/spsc_queue.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ob;

TEST(SpscQueue, EmptyAndFullSingleThread) {
    SpscQueue<int, 4> q;
    int v = 0;
    EXPECT_FALSE(q.pop(v));                 // starts empty
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99));               // full at exactly N
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);                    // FIFO
    }
    EXPECT_FALSE(q.pop(v));
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<std::uint64_t, 8> q;
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.push(i));
        ASSERT_TRUE(q.pop(v));
        ASSERT_EQ(v, i);
    }
}

// The guide's test: 10M sequential integers from one thread to another;
// every value must arrive exactly once, in order. Run this under
// ThreadSanitizer (-DOB_SANITIZE=thread). Runs for both queue versions.
template <class Q>
void tenMillionInOrder() {
    constexpr std::uint64_t kCount = 10'000'000;
    auto q = std::make_unique<Q>();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i)
            while (!q->push(i)) cpuRelax();
    });

    std::uint64_t expected = 0, outOfOrder = 0, v = 0;
    while (expected < kCount) {
        if (!q->pop(v)) { cpuRelax(); continue; }
        if (v != expected) ++outOfOrder;
        ++expected;
    }
    producer.join();
    EXPECT_EQ(outOfOrder, 0u);
    EXPECT_FALSE(q->pop(v));  // nothing extra
}

TEST(SpscQueue, TenMillionIntegersArriveOnceInOrder) {
    tenMillionInOrder<SpscQueue<std::uint64_t, 1024>>();
}

TEST(SpscQueueCached, TenMillionIntegersArriveOnceInOrder) {
    tenMillionInOrder<SpscQueueCached<std::uint64_t, 1024>>();
}

TEST(SpscQueueCached, EmptyAndFullSingleThread) {
    SpscQueueCached<int, 4> q;
    int v = 0;
    EXPECT_FALSE(q.pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(i));
    EXPECT_FALSE(q.push(99));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(10 + i));  // refills after wrap
    EXPECT_FALSE(q.push(99));
}

// Threaded replay must give exactly the same trades as single-threaded.
template <template <class, std::size_t> class Q, class Book>
void threadedMatchesSingle(Book a, Book b) {
    Generator gen(GenConfig{.seed = 3});
    std::string csv = "# type,id,side,price,qty\n";
    for (int i = 0; i < 200'000; ++i) { csv += formatCommand(gen.next()); csv += '\n'; }

    std::vector<Trade> single, threaded;
    auto r1 = runSingleThreaded(a, csv, [&](const std::vector<Trade>& t) {
        single.insert(single.end(), t.begin(), t.end());
    });
    auto r2 = runPipeline<Q>(b, csv, [&](const std::vector<Trade>& t) {
        threaded.insert(threaded.end(), t.begin(), t.end());
    });

    EXPECT_EQ(r1.commands, 200'000u);
    EXPECT_EQ(r2.commands, 200'000u);
    ASSERT_EQ(single.size(), threaded.size());
    for (std::size_t i = 0; i < single.size(); ++i) ASSERT_EQ(single[i], threaded[i]) << i;
    EXPECT_GT(single.size(), 0u);
    EXPECT_EQ(a.bestBid(), b.bestBid());
    EXPECT_EQ(a.bestAsk(), b.bestAsk());
    EXPECT_EQ(a.orderCount(), b.orderCount());
}

TEST(Pipeline, ThreadedReplayMatchesSingleThreadedV1) {
    threadedMatchesSingle<SpscQueue>(OrderBook{}, OrderBook{});
}

TEST(Pipeline, ThreadedReplayMatchesSingleThreadedV2) {
    threadedMatchesSingle<SpscQueue>(FlatOrderBook(8'000, 12'000), FlatOrderBook(8'000, 12'000));
}

TEST(Pipeline, ThreadedReplayMatchesSingleThreadedV2CachedQueue) {
    threadedMatchesSingle<SpscQueueCached>(FlatOrderBook(8'000, 12'000),
                                           FlatOrderBook(8'000, 12'000));
}
