#include <gtest/gtest.h>
#include "ob/flat_order_book.hpp"
#include "ob/order_book.hpp"
#include "ob/parser.hpp"

#include <string>
#include <vector>

using namespace ob;

// ------------------------------------------------------------ our CSV ------

TEST(CsvParser, ParsesAddCancelModify) {
    Command c{};
    ASSERT_TRUE(parseCommand("A,1,S,10125,100", c));
    EXPECT_EQ(c.type, CmdType::Add);
    EXPECT_EQ(c.id, 1u);
    EXPECT_EQ(c.side, Side::Sell);
    EXPECT_EQ(c.price, 10125);
    EXPECT_EQ(c.qty, 100u);

    ASSERT_TRUE(parseCommand("C,1,,,", c));
    EXPECT_EQ(c.type, CmdType::Cancel);
    EXPECT_EQ(c.id, 1u);

    ASSERT_TRUE(parseCommand("M,2,B,10122,40\r\n", c));  // Windows line ending
    EXPECT_EQ(c.type, CmdType::Modify);
    EXPECT_EQ(c.side, Side::Buy);
    EXPECT_EQ(c.price, 10122);
    EXPECT_EQ(c.qty, 40u);
}

TEST(CsvParser, SkipsCommentsBlankAndMalformedLines) {
    Command c{};
    EXPECT_FALSE(parseCommand("# type,id,side,price,qty", c));
    EXPECT_FALSE(parseCommand("", c));
    EXPECT_FALSE(parseCommand("X,1,B,100,10", c));      // unknown type
    EXPECT_FALSE(parseCommand("A,abc,B,100,10", c));    // bad id
    EXPECT_FALSE(parseCommand("A,1,Q,100,10", c));      // bad side
    EXPECT_FALSE(parseCommand("A,1,B,10x,10", c));      // bad price
    EXPECT_FALSE(parseCommand("A,1,B,100,0", c));       // add with zero qty
}

TEST(CsvParser, FormatRoundTrips) {
    for (const Command& in : {Command{CmdType::Add, 7, Side::Buy, 9999, 3},
                              Command{CmdType::Modify, 8, Side::Sell, 10001, 12},
                              Command{CmdType::Cancel, 9, Side::Buy, 0, 0}}) {
        Command out{};
        ASSERT_TRUE(parseCommand(formatCommand(in), out));
        EXPECT_EQ(out.type, in.type);
        EXPECT_EQ(out.id, in.id);
        if (in.type != CmdType::Cancel) {
            EXPECT_EQ(out.side, in.side);
            EXPECT_EQ(out.price, in.price);
            EXPECT_EQ(out.qty, in.qty);
        }
    }
}

TEST(CsvParser, ForEachLineHandlesMissingTrailingNewline) {
    std::vector<std::string> lines;
    forEachLine("a\nb\nc", [&](std::string_view l) { lines.emplace_back(l); });
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[2], "c");
}

// ------------------------------------------------------- Databento MBO -----

TEST(Databento, HeaderColumnsFoundByName) {
    auto cols = parseMboHeader(
        "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,price,size,"
        "channel_id,order_id,flags,ts_in_delta,sequence,symbol");
    ASSERT_TRUE(cols.ok());
    EXPECT_EQ(cols.action, 5);
    EXPECT_EQ(cols.side, 6);
    EXPECT_EQ(cols.price, 7);
    EXPECT_EQ(cols.size, 8);
    EXPECT_EQ(cols.orderId, 10);
    EXPECT_FALSE(parseMboHeader("action,side,price").ok());
}

TEST(Databento, PriceConvertsFixedPointAndDecimalToCents) {
    Price p = 0;
    ASSERT_TRUE(mboPriceToTicks("101250000000", 10'000'000, p));  // 101.25 * 1e9
    EXPECT_EQ(p, 10125);
    ASSERT_TRUE(mboPriceToTicks("101.25", 10'000'000, p));
    EXPECT_EQ(p, 10125);
    ASSERT_TRUE(mboPriceToTicks("7.5", 10'000'000, p));
    EXPECT_EQ(p, 750);
    EXPECT_FALSE(mboPriceToTicks("9223372036854775807", 10'000'000, p));  // UNDEF_PRICE
}

TEST(Databento, ParsesRecordAndMapsAskToSell) {
    auto cols = parseMboHeader("action,side,price,size,order_id");
    MboRecord r{};
    ASSERT_TRUE(parseMboLine("A,A,101250000000,200,555", cols, 10'000'000, r));
    EXPECT_EQ(r.action, 'A');
    EXPECT_EQ(r.side, Side::Sell);
    EXPECT_EQ(r.price, 10125);
    EXPECT_EQ(r.size, 200u);
    EXPECT_EQ(r.id, 555u);
}

template <class Book>
void databentoScenario(Book& b) {
    auto cols = parseMboHeader("action,side,price,size,order_id");
    std::vector<Trade> out;
    auto feed = [&](const char* line) {
        MboRecord r{};
        if (!parseMboLine(line, cols, 10'000'000, r)) return false;
        return applyMbo(b, r, out);
    };
    EXPECT_TRUE(feed("A,B,100000000000,100,1"));    // bid 100 @ $100.00
    EXPECT_TRUE(feed("A,A,100100000000,50,2"));     // ask  50 @ $100.10
    EXPECT_FALSE(feed("T,B,100100000000,10,0"));    // trade: ignored
    EXPECT_FALSE(feed("F,A,100100000000,10,2"));    // fill: ignored
    EXPECT_TRUE(feed("C,A,100100000000,10,2"));     // partial cancel: 50 -> 40
    EXPECT_EQ(b.depthAt(Side::Sell, 10010), 40u);
    EXPECT_TRUE(feed("M,B,100050000000,100,1"));    // reprice bid to $100.05
    EXPECT_EQ(b.bestBid(), 10005);
    EXPECT_TRUE(feed("M,B,99900000000,7,3"));       // unknown id -> treated as add
    EXPECT_EQ(b.depthAt(Side::Buy, 9990), 7u);
    EXPECT_TRUE(feed("C,A,100100000000,40,2"));     // full cancel
    EXPECT_FALSE(b.bestAsk().has_value());
    EXPECT_TRUE(feed("R,N,,0,0"));                  // clear book
    EXPECT_EQ(b.orderCount(), 0u);
    EXPECT_TRUE(out.empty());                       // nothing here should cross
}

TEST(Databento, ApplyHandlesPartialCancelModifyAndClearV1) {
    OrderBook b;
    databentoScenario(b);
}

TEST(Databento, ApplyHandlesPartialCancelModifyAndClearV2) {
    FlatOrderBook b(9'000, 11'000);
    databentoScenario(b);
}
