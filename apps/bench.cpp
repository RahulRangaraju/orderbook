// bench: throughput and per-order latency for each book.
//
//   ./build/bench 5000000              # all books + the 2-thread pipeline
//   ./build/bench 5000000 --book v2    # one book only (v1, v2 or pipeline)
//   ./build/bench 5000000 --runs 9     # more timed runs
//   ./build/bench 5000000 --api vector # the allocating API (Step 9 "before")
//
// Rules (from the guide):
//   * Release build only.
//   * All commands are generated into a std::vector<Command> before timing,
//     so we time the engine, not file I/O.
//   * One warm-up pass, then N timed runs (default 5); report the median.
//   * Latency: time every command on its own, store in a pre-sized vector,
//     then nth_element for p50 / p99 / p99.9.
#include "ob/engine.hpp"
#include "ob/flat_order_book.hpp"
#include "ob/generator.hpp"
#include "ob/order_book.hpp"
#include "ob/parser.hpp"
#include "ob/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef OB_BUILD_FLAGS
#define OB_BUILD_FLAGS "unknown"
#endif

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Args {
    std::size_t n = 5'000'000;
    int runs = 5;
    std::string book = "all";
    bool vectorApi = false;   // true = book.add(...) returns a fresh vector per order
    bool pipeline = true;
};

std::string cpuModel() {
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    return "unknown";
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Percentiles { double p50, p99, p999, max; };

Percentiles percentiles(std::vector<std::int64_t>& ns) {
    auto at = [&](double q) {
        auto k = static_cast<std::size_t>(q * static_cast<double>(ns.size() - 1));
        std::nth_element(ns.begin(), ns.begin() + static_cast<std::ptrdiff_t>(k), ns.end());
        return static_cast<double>(ns[k]);
    };
    return {at(0.50), at(0.99), at(0.999), static_cast<double>(*std::max_element(ns.begin(), ns.end()))};
}

template <class Book>
Book makeBook(Price lo, Price hi, std::size_t n) {
    if constexpr (std::is_same_v<Book, FlatOrderBook>) return FlatOrderBook(lo, hi, n);
    else return OrderBook{};
}

// The "before" API for Step 9: a new std::vector<Trade> per call.
template <class Book>
std::size_t processAlloc(Book& book, const Command& c) {
    switch (c.type) {
        case CmdType::Add:    return book.add(c.id, c.side, c.price, c.qty).size();
        case CmdType::Cancel: book.cancel(c.id); return 0;
        case CmdType::Modify: return book.modify(c.id, c.price, c.qty).size();
    }
    return 0;
}

template <class Book>
void benchBook(const char* name, const std::vector<Command>& cmds, const Args& a) {
    auto [lo, hi] = priceWindow(cmds);
    std::vector<Trade> trades;
    trades.reserve(256);
    std::size_t tradeCount = 0, resting = 0;

    auto oneRun = [&]() {
        Book book = makeBook<Book>(lo, hi, cmds.size());
        tradeCount = 0;
        const auto t0 = Clock::now();
        if (a.vectorApi) {
            for (const auto& c : cmds) tradeCount += processAlloc(book, c);
        } else {
            for (const auto& c : cmds) {
                trades.clear();
                process(book, c, trades);
                tradeCount += trades.size();
            }
        }
        const auto t1 = Clock::now();
        resting = book.orderCount();
        return std::chrono::duration<double>(t1 - t0).count();
    };

    oneRun();  // warm-up: page in memory, train branch predictors
    std::vector<double> rates;
    for (int r = 0; r < a.runs; ++r) rates.push_back(static_cast<double>(cmds.size()) / oneRun());

    // Latency run: time each command individually.
    std::vector<std::int64_t> ns(cmds.size());
    {
        Book book = makeBook<Book>(lo, hi, cmds.size());
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const auto t0 = Clock::now();
            if (a.vectorApi) {
                processAlloc(book, cmds[i]);
            } else {
                trades.clear();
                process(book, cmds[i], trades);
            }
            const auto t1 = Clock::now();
            ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        }
    }
    const Percentiles p = percentiles(ns);
    std::printf("%-22s %10.2fM/s   p50 %7.0f ns   p99 %7.0f ns   p99.9 %8.0f ns   "
                "(min %.2fM/s, max %.2fM/s, trades %zu, resting %zu)\n",
                name, median(rates) / 1e6, p.p50, p.p99, p.p999,
                *std::min_element(rates.begin(), rates.end()) / 1e6,
                *std::max_element(rates.begin(), rates.end()) / 1e6, tradeCount, resting);
}

// End-to-end pipeline from CSV text in memory: parse + match.
// Compares one thread doing both vs. the 2-thread SPSC pipeline, with the
// guide's queue and with the cached-index queue.
//   * full speed: the parser pushes as fast as it can -> throughput; if the
//     consumer can't keep up the queue fills, and latency becomes "time spent
//     waiting in a 65k-deep backlog" (milliseconds). That's queueing, not the hop.
//   * paced: one command every paceNs -> the queue stays nearly empty, so
//     latency = queue hop + matching. This is the number to quote.
void benchPipeline(const std::vector<Command>& cmds, const Args& a) {
    std::string csv;
    csv.reserve(cmds.size() * 24);
    for (const auto& c : cmds) { csv += formatCommand(c); csv += '\n'; }
    auto [lo, hi] = priceWindow(cmds);
    auto noop = [](const std::vector<Trade>&) {};

    auto single = [&]() {
        FlatOrderBook book(lo, hi, cmds.size());
        const auto t0 = Clock::now();
        runSingleThreaded(book, csv, noop);
        return std::chrono::duration<double>(Clock::now() - t0).count();
    };
    double s0 = 0;
    {
        single();
        std::vector<double> r;
        for (int i = 0; i < a.runs; ++i) r.push_back(static_cast<double>(cmds.size()) / single());
        s0 = median(r);
    }
    std::printf("\nPipeline (CSV text in memory -> parse -> match, v2 book, %zu commands)\n", cmds.size());
    std::printf("%-30s %8.2fM/s\n", "1 thread parse+match", s0 / 1e6);

    auto runQueue = [&]<template <class, std::size_t> class Q>(const char* label) {
        auto threaded = [&](std::vector<std::int64_t>* lat, std::int64_t pace) {
            FlatOrderBook book(lo, hi, cmds.size());
            PipelineOptions opt;
            opt.latencies = lat;
            opt.paceNs = pace;
            const auto t0 = Clock::now();
            runPipeline<Q>(book, csv, noop, opt);
            return std::chrono::duration<double>(Clock::now() - t0).count();
        };
        threaded(nullptr, 0);
        std::vector<double> r;
        for (int i = 0; i < a.runs; ++i) r.push_back(static_cast<double>(cmds.size()) / threaded(nullptr, 0));

        std::vector<std::int64_t> lat(cmds.size(), 0);
        threaded(&lat, 0);
        const Percentiles full = percentiles(lat);

        // Paced at 1M commands/sec (first 2M commands): well under what
        // either queue sustains, so the queue stays nearly empty.
        const std::size_t paced = std::min<std::size_t>(cmds.size(), 2'000'000);
        std::vector<std::int64_t> plat(paced, 0);
        {
            FlatOrderBook book(lo, hi, cmds.size());
            PipelineOptions opt;
            opt.latencies = &plat;
            opt.paceNs = 1'000;
            std::size_t end = 0;  // byte offset just past the paced-th line
            for (std::size_t i = 0; i < paced; ++i) end = csv.find('\n', end) + 1;
            runPipeline<Q>(book, std::string_view(csv.data(), end), noop, opt);
        }
        const Percentiles p = percentiles(plat);

        std::printf("%-30s %8.2fM/s   full-speed latency p50 %6.2f ms, p99 %6.2f ms (backlog)\n",
                    label, median(r) / 1e6, full.p50 / 1e6, full.p99 / 1e6);
        std::printf("%-30s %8s     paced 1M/s latency p50 %5.0f ns, p99 %5.0f ns, p99.9 %6.0f ns\n",
                    "", "", p.p50, p.p99, p.p999);
    };
    runQueue.template operator()<SpscQueue>("v2 + SPSC (guide's queue)");
    runQueue.template operator()<SpscQueueCached>("v2 + SPSC (cached indices)");
}

// How much does one steady_clock::now() pair cost? This is the floor under
// every latency number above.
void timerOverhead() {
    std::vector<std::int64_t> ns(1'000'000);
    for (auto& x : ns) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        x = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    const Percentiles p = percentiles(ns);
    std::printf("timer overhead (empty steady_clock pair): p50 %.0f ns, p99 %.0f ns\n\n", p.p50, p.p99);
}
}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--runs" && i + 1 < argc) a.runs = std::atoi(argv[++i]);
        else if (s == "--book" && i + 1 < argc) a.book = argv[++i];
        else if (s == "--api" && i + 1 < argc) a.vectorApi = std::string(argv[++i]) == "vector";
        else if (s == "--no-pipeline") a.pipeline = false;
        else a.n = std::strtoull(argv[i], nullptr, 10);
    }

    std::printf("commands: %zu   runs: %d (median reported, after 1 warm-up)   api: %s\n",
                a.n, a.runs, a.vectorApi ? "vector-per-order" : "reused buffer");
    std::printf("cpu: %s (%u hw threads)\ncompiler: %s   flags: %s\n",
                cpuModel().c_str(), std::thread::hardware_concurrency(), __VERSION__, OB_BUILD_FLAGS);

    Generator gen;  // fixed seed 42
    const std::vector<Command> cmds = gen.generate(a.n);
    timerOverhead();

    if (a.book == "all" || a.book == "v1") benchBook<OrderBook>("v1 (std::map)", cmds, a);
    if (a.book == "all" || a.book == "v2") benchBook<FlatOrderBook>("v2 (flat array)", cmds, a);
    if (a.pipeline && (a.book == "all" || a.book == "pipeline")) benchPipeline(cmds, a);
    return 0;
}
