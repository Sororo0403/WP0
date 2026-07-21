#pragma once

#include "runtime/Behavior.h"

#include <cstddef>
#include <memory>
#include <vector>

class World;

class BehaviorSystem {
public:
    bool Attach(EntityId entity, std::unique_ptr<Behavior> behavior);
    void Start(World& world);
    void Update(float deltaTime);
    void Stop();
    void Clear();

    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] size_t Size() const;

private:
    struct Entry {
        EntityId entity{};
        std::unique_ptr<Behavior> behavior;
        bool started = false;
    };

    World* world_ = nullptr;
    std::vector<Entry> entries_;
};
