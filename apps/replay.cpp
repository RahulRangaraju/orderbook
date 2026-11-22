// replay: drive the order book from a file.
//
//   ./build/replay data/sample.csv                      # v1 book, print trades
//   ./build/replay big.csv --book v2 --quiet            # summary only
//   ./build/replay big.csv --book v2 --threads          # parse + match on 2 threads
//   ./build/replay big.csv --book v2 --threads --queue cached
//   ./build/replay mbo.csv --format databento --book v2 # Databento MBO CSV
//
// Trades go to stdout; the summary goes to stderr.
#include "ob/engine.hpp"
#include "ob/flat_order_book.hpp"
#include "ob/order_book.hpp"
#include "ob/parser.hpp"
#include "ob/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

using namespace ob;

namespace {

struct Args {
    std::string file;
    std::string book = "v1";
    std::string format = "csv";
    bool threads = false;
    bool cachedQueue = false;              // --queue cached
    bool quiet = false;
    std::int64_t tickNanos = 10'000'000;   // Databento: 1 tick = 1 cent
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
        "usage: replay <file> [--book v1|v2] [--format csv|databento] [--threads]\n"
        "                     [--queue guide|cached] [--quiet] [--tick-nanos N]\n");
    std::exit(2);
}

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (s == "--book") a.book = val();
        else if (s == "--format") a.format = val();
        else if (s == "--threads") a.threads = true;
        else if (s == "--queue") a.cachedQueue = val() == "cached";
        else if (s == "--quiet") a.quiet = true;
        else if (s == "--tick-nanos") a.tickNanos = std::stoll(val());
        else if (s == "-h" || s == "--help") usage();
        else if (a.file.empty()) a.file = s;
        else usage();
    }
    if (a.file.empty() || (a.book != "v1" && a.book != "v2") ||
        (a.format != "csv" && a.format != "databento"))
        usage();
    if (a.threads && a.format != "csv") {
        std::fprintf(stderr, "--threads is supported for the csv format only\n");
        std::exit(2);
    }
    return a;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(1); }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

// Buffered trade printer: one fwrite per 64 KB instead of one per trade.
class TradePrinter {
public:
    explicit TradePrinter(bool quiet) : quiet_(quiet) { buf_.reserve(1 << 16); }
    ~TradePrinter() { flush(); }
    void operator()(const std::vector<Trade>& trades) {
        if (quiet_) return;
        char line[128];
        for (const Trade& t : trades) {
            int n = std::snprintf(line, sizeof line, "TRADE buy=%llu sell=%llu px=%lld qty=%u\n",
                                  static_cast<unsigned long long>(t.buyId),
                                  static_cast<unsigned long long>(t.sellId),
                                  static_cast<long long>(t.price), t.qty);
            buf_.append(line, static_cast<std::size_t>(n));
        }
        if (buf_.size() > (1 << 16) - 256) flush();
    }
    void flush() { std::fwrite(buf_.data(), 1, buf_.size(), stdout); buf_.clear(); }
private:
    bool quiet_;
    std::string buf_;
};

// v2 needs a price window before it starts. Scan the prices once up front.
std::pair<Price, Price> scanWindow(const std::string& data, const Args& a) {
    Price lo = std::numeric_limits<Price>::max(), hi = std::numeric_limits<Price>::min();
    auto see = [&](Price p) { lo = std::min(lo, p); hi = std::max(hi, p); };
    if (a.format == "csv") {
        forEachLine(data, [&](std::string_view l) {
            Command c;
            if (parseCommand(l, c) && c.type != CmdType::Cancel) see(c.price);
        });
    } else {
        bool header = true;
        MboColumns cols;
        forEachLine(data, [&](std::string_view l) {
            if (header) { cols = parseMboHeader(l); header = false; return; }
            MboRecord r;
            if (parseMboLine(l, cols, a.tickNanos, r) && (r.action == 'A' || r.action == 'M'))
                see(r.price);
        });
    }
    if (lo > hi) return {0, 1};
    return {std::max<Price>(0, lo - lo / 10 - 1), hi + hi / 10 + 1};  // ±10%
}

template <class Book>
int run(Book& book, const std::string& data, const Args& a) {
    TradePrinter print(a.quiet);
    std::uint64_t commands = 0, trades = 0, ignored = 0;
    const auto t0 = std::chrono::steady_clock::now();

    if (a.format == "csv" && a.threads) {
        auto r = a.cachedQueue ? runPipeline<SpscQueueCached>(book, data, print)
                               : runPipeline<SpscQueue>(book, data, print);
        commands = r.commands;
        trades = r.trades;
    } else if (a.format == "csv") {
        auto r = runSingleThreaded(book, data, print);
        commands = r.commands;
        trades = r.trades;
    } else {
        std::vector<Trade> out;
        bool header = true;
        MboColumns cols;
        forEachLine(data, [&](std::string_view l) {
            if (header) {
                cols = parseMboHeader(l);
                header = false;
                if (!cols.ok()) {
                    std::fprintf(stderr, "header is missing a Databento MBO column "
                                         "(need action, side, price, size, order_id)\n");
                    std::exit(1);
                }
                return;
            }
            MboRecord r;
            if (!parseMboLine(l, cols, a.tickNanos, r)) { ++ignored; return; }
            out.clear();
            if (!applyMbo(book, r, out)) ++ignored;
            ++commands;
            if (!out.empty()) { trades += out.size(); print(out); }
        });
    }
    print.flush();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    auto px = [](std::optional<Price> p) { return p ? std::to_string(*p) : std::string("-"); };
    std::fprintf(stderr,
                 "book=%s format=%s threads=%s\n"
                 "commands=%llu trades=%llu ignored=%llu resting=%zu\n"
                 "best_bid=%s best_ask=%s\n"
                 "elapsed=%.3fs (%.2fM commands/sec, including parsing and output)\n",
                 a.book.c_str(), a.format.c_str(), a.threads ? "2" : "1",
                 static_cast<unsigned long long>(commands), static_cast<unsigned long long>(trades),
                 static_cast<unsigned long long>(ignored), book.orderCount(),
                 px(book.bestBid()).c_str(), px(book.bestAsk()).c_str(), secs,
                 commands / secs / 1e6);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    const Args a = parseArgs(argc, argv);
    const std::string data = readFile(a.file);
    if (a.book == "v1") {
        OrderBook book;
        return run(book, data, a);
    }
    auto [lo, hi] = scanWindow(data, a);
    FlatOrderBook book(lo, hi, 1 << 20);
    int rc = run(book, data, a);
    if (book.rejected())
        std::fprintf(stderr, "rejected=%llu (price outside window [%lld, %lld])\n",
                     static_cast<unsigned long long>(book.rejected()),
                     static_cast<long long>(lo), static_cast<long long>(hi));
    return rc;
}
