#pragma once

#include "world/EntityId.h"

#include <cstddef>
#include <set>

class BehaviorSystem;
class World;

class TriggerSystem {
public:
    void Update(const World& world, BehaviorSystem& behaviors);
    void Clear();

    [[nodiscard]] size_t ActivePairCount() const;

private:
    struct Pair {
        EntityId first{};
        EntityId second{};

        friend bool operator<(const Pair& left, const Pair& right) noexcept {
            if (left.first.high != right.first.high) {
                return left.first.high < right.first.high;
            }
            if (left.first.low != right.first.low) {
                return left.first.low < right.first.low;
            }
            if (left.second.high != right.second.high) {
                return left.second.high < right.second.high;
            }
            return left.second.low < right.second.low;
        }
    };

    std::set<Pair> activePairs_;
};
