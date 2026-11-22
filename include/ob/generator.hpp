#pragma once
#include "ob/types.hpp"
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace ob {

struct GenConfig {
    std::uint64_t seed      = 42;
    Price  startMid         = 10'000;   // $100.00 in cents
    Price  minMid           = 9'000;    // mid price random-walks inside this band
    Price  maxMid           = 11'000;
    int    addPct           = 70;       // command mix (percent)
    int    cancelPct        = 25;       // the remaining 5% are modifies
    int    aggressivePct    = 30;       // share of adds priced through the mid
    int    maxOffset        = 20;       // passive orders rest up to this many ticks away
    Quantity maxQty         = 500;
};

// Synthetic order flow: ~70% adds, 25% cancels, 5% modifies around a
// random-walking mid price. A fixed seed gives the same stream every run.
//
// Note: we draw from mt19937_64 directly (rng() % n) instead of
// std::uniform_int_distribution, because the distributions are allowed to
// differ between standard libraries. This way GCC and Clang builds generate
// byte-identical files.
class Generator {
public:
    explicit Generator(GenConfig cfg = {}) : cfg_(cfg), rng_(cfg.seed), mid_(cfg.startMid) {}

    Command next() {
        walkMid();
        const int roll = static_cast<int>(below(100));
        if (roll < cfg_.addPct || live_.empty()) return makeAdd();
        if (roll < cfg_.addPct + cfg_.cancelPct) return makeCancel();
        return makeModify();
    }

    std::vector<Command> generate(std::size_t n) {
        std::vector<Command> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(next());
        return out;
    }

private:
    struct Live { OrderId id; Side side; Price price; Quantity qty; };

    GenConfig cfg_;
    std::mt19937_64 rng_;
    Price mid_;
    OrderId nextId_ = 1;
    // Orders we've sent and not cancelled. Some will have been filled by the
    // engine already; cancelling those just returns false, like real life.
    std::vector<Live> live_;

    std::uint64_t below(std::uint64_t n) { return rng_() % n; }

    void walkMid() {
        if (below(10) == 0) {  // move ~1 in 10 commands
            mid_ += below(2) ? 1 : -1;
            mid_ = std::clamp(mid_, cfg_.minMid, cfg_.maxMid);
        }
    }

    Price pickPrice(Side side) {
        const bool aggressive = static_cast<int>(below(100)) < cfg_.aggressivePct;
        const Price off = aggressive ? -static_cast<Price>(below(5))          // through the mid
                                     : static_cast<Price>(1 + below(cfg_.maxOffset));
        return side == Side::Buy ? mid_ - off : mid_ + off;
    }

    Command makeAdd() {
        const Side side = below(2) ? Side::Buy : Side::Sell;
        const Price px = pickPrice(side);
        const auto qty = static_cast<Quantity>(1 + below(cfg_.maxQty));
        const OrderId id = nextId_++;
        live_.push_back({id, side, px, qty});
        return {CmdType::Add, id, side, px, qty};
    }

    Command makeCancel() {
        const std::size_t i = below(live_.size());
        const Live o = live_[i];
        live_[i] = live_.back();   // swap-remove: O(1)
        live_.pop_back();
        return {CmdType::Cancel, o.id, o.side, 0, 0};
    }

    Command makeModify() {
        Live& o = live_[below(live_.size())];
        if (below(2) && o.qty > 1) {                      // shrink: keeps priority
            o.qty = static_cast<Quantity>(1 + below(o.qty - 1));
        } else {                                          // reprice: back of queue
            o.price = pickPrice(o.side);
            o.qty = static_cast<Quantity>(1 + below(cfg_.maxQty));
        }
        return {CmdType::Modify, o.id, o.side, o.price, o.qty};
    }
};
}  // namespace ob
