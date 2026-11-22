#pragma once
#include "ob/types.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ob {

// OrderId -> pool slot.
//
// Step 9 change, driven by profiling: with std::unordered_map, every insert
// allocated a hash node (malloc/free showed up near the top of the profile)
// and every lookup chased a pointer into a cold node (unordered_map ops
// accounted for ~30% of all L1 data-cache misses).
//
// Most ids are small and dense (our generator counts 1, 2, 3, ...), so they
// go in a flat vector: lookup = one array read, no allocation per order.
// Ids too large for the vector (e.g. 64-bit exchange order ids) fall back to
// the hash map, so behaviour never changes, only speed.
class IdIndex {
public:
    static constexpr std::uint32_t NIL = UINT32_MAX;
    // Largest id kept in the dense vector. 2^25 ids * 4 bytes = 128 MB at
    // most, and memory is only used up to the largest id actually seen.
    static constexpr OrderId kDenseLimit = OrderId{1} << 25;

    void reserve(std::size_t n) {
        dense_.reserve(std::min<std::size_t>(n, kDenseLimit));
    }

    std::uint32_t find(OrderId id) const {
        if (id < kDenseLimit) return id < dense_.size() ? dense_[id] : NIL;
        auto it = sparse_.find(id);
        return it == sparse_.end() ? NIL : it->second;
    }

    bool contains(OrderId id) const { return find(id) != NIL; }

    // Caller guarantees id is not already present.
    void insert(OrderId id, std::uint32_t slot) {
        if (id < kDenseLimit) {
            if (id >= dense_.size()) {
                const std::size_t grow = std::max<std::size_t>(id + 1, dense_.size() * 2);
                dense_.resize(std::min<std::size_t>(grow, kDenseLimit), NIL);
            }
            dense_[id] = slot;
        } else {
            sparse_.emplace(id, slot);
        }
        ++size_;
    }

    void erase(OrderId id) {
        if (id < kDenseLimit) {
            if (id < dense_.size() && dense_[id] != NIL) { dense_[id] = NIL; --size_; }
        } else {
            size_ -= sparse_.erase(id);
        }
    }

    std::size_t size() const { return size_; }

    void clear() {
        std::fill(dense_.begin(), dense_.end(), NIL);
        sparse_.clear();
        size_ = 0;
    }

private:
    std::vector<std::uint32_t> dense_;
    std::unordered_map<OrderId, std::uint32_t> sparse_;
    std::size_t size_ = 0;
};
}  // namespace ob
