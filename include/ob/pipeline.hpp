#pragma once
#include "ob/engine.hpp"
#include "ob/parser.hpp"
#include "ob/spsc_queue.hpp"
#include "ob/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ob {

inline std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Pin a thread to one CPU core so the OS doesn't migrate it mid-run (steadier
// numbers, warm caches). Best effort: silently does nothing if it fails.
inline void pinThread(std::thread& t, int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu) % std::thread::hardware_concurrency(), &set);
    pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
#else
    (void)t; (void)cpu;
#endif
}

struct PipelineOptions {
    int producerCpu = 0;           // -1 = don't pin
    int consumerCpu = 1;
    std::vector<std::int64_t>* latencies = nullptr;  // push -> matched, ns (pre-sized)
    // 0 = push as fast as the parser can go. Otherwise send one command every
    // paceNs nanoseconds, so the queue is NOT full and latency measures the
    // queue hop + matching instead of time spent waiting in a backlog.
    std::int64_t paceNs = 0;
};

struct PipelineMsg { Command cmd; std::int64_t pushedNs; };
inline constexpr std::size_t kPipelineQueueSize = 1 << 16;

struct PipelineResult {
    std::uint64_t commands = 0;
    std::uint64_t trades = 0;
};

// Thread 1 (producer): parse CSV lines into Commands and push them.
// Thread 2 (consumer): pop and run the book.
// Shutdown: producer sets `done` (release) after its last push; the consumer
// sees it (acquire), drains whatever is left, and exits. Because the last
// push happens-before `done`, nothing can be lost.
//
// onTrades(const std::vector<Trade>&) is called on the consumer thread after
// every command that traded.
//
// Queue is SpscQueue (the guide's version) by default, or SpscQueueCached.
template <template <class, std::size_t> class Queue = SpscQueue, class Book, class OnTrades>
PipelineResult runPipeline(Book& book, std::string_view csv, OnTrades&& onTrades,
                           const PipelineOptions& opt = {}) {
    using Msg = PipelineMsg;
    using Q = Queue<Msg, kPipelineQueueSize>;
    auto q = std::make_unique<Q>();       // ~2.5 MB: keep it off the stack
    std::atomic<bool> done{false};
    PipelineResult res;

    std::thread consumer([&] {
        std::vector<Trade> trades;
        trades.reserve(64);
        Msg m;
        std::uint64_t n = 0;
        auto handle = [&](const Msg& msg) {
            trades.clear();
            process(book, msg.cmd, trades);
            if (opt.latencies && n < opt.latencies->size())
                (*opt.latencies)[n] = nowNs() - msg.pushedNs;
            ++n;
            if (!trades.empty()) {
                res.trades += trades.size();
                onTrades(trades);
            }
        };
        while (true) {
            if (q->pop(m)) { handle(m); continue; }
            if (done.load(std::memory_order_acquire)) {
                while (q->pop(m)) handle(m);   // drain the tail
                break;
            }
            cpuRelax();                         // busy-wait: lowest latency, burns a core
        }
        res.commands = n;
    });

    std::thread producer([&] {
        const std::int64_t start = nowNs();
        std::int64_t sent = 0;
        forEachLine(csv, [&](std::string_view line) {
            Msg m;
            if (!parseCommand(line, m.cmd)) return;
            if (opt.paceNs) {
                const std::int64_t due = start + sent * opt.paceNs;
                while (nowNs() < due) cpuRelax();
            }
            ++sent;
            // Stamp just before each push attempt; if the queue is full we
            // spin and re-stamp, so latency = time spent inside the pipeline.
            while (true) {
                m.pushedNs = opt.latencies ? nowNs() : 0;
                if (q->push(m)) break;
                cpuRelax();
            }
        });
        done.store(true, std::memory_order_release);
    });

    pinThread(producer, opt.producerCpu);
    pinThread(consumer, opt.consumerCpu);
    producer.join();
    consumer.join();
    return res;
}

// The same work on one thread, for comparison: parse a line, run it, repeat.
template <class Book, class OnTrades>
PipelineResult runSingleThreaded(Book& book, std::string_view csv, OnTrades&& onTrades) {
    PipelineResult res;
    std::vector<Trade> trades;
    trades.reserve(64);
    forEachLine(csv, [&](std::string_view line) {
        Command c;
        if (!parseCommand(line, c)) return;
        trades.clear();
        process(book, c, trades);
        ++res.commands;
        if (!trades.empty()) {
            res.trades += trades.size();
            onTrades(trades);
        }
    });
    return res;
}
}  // namespace ob
