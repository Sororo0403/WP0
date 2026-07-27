#pragma once

#include "runtime/Behavior.h"

#include <cstddef>
#include <memory>
#include <vector>

class World;
class TriggerSystem;

class BehaviorSystem {
public:
    bool Attach(EntityId entity, std::unique_ptr<Behavior> behavior);
    void Start(World& world);
    void Update(float deltaTime);
    void DispatchButtonClick(EntityId entity);
    void DispatchToggleValueChanged(EntityId entity, bool isOn);
    void Stop();
    void Clear();

    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] size_t Size() const;

private:
    friend class TriggerSystem;

    enum class TriggerEvent {
        Enter,
        Stay,
        Exit,
    };

    void DispatchTriggerEvent(TriggerEvent event, EntityId entity, EntityId other);

    struct Entry {
        EntityId entity{};
        std::unique_ptr<Behavior> behavior;
        bool started = false;
    };

    World* world_ = nullptr;
    std::vector<Entry> entries_;
};
