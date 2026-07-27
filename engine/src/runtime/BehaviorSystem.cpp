#include "runtime/BehaviorSystem.h"

#include "world/World.h"

#include <utility>

bool BehaviorSystem::Attach(EntityId entity, std::unique_ptr<Behavior> behavior) {
    if (!entity.IsValid() || behavior == nullptr ||
        (world_ != nullptr && !world_->Contains(entity))) {
        return false;
    }

    Entry entry{entity, std::move(behavior), false};
    if (world_ != nullptr && world_->IsActiveInHierarchy(entity)) {
        entry.behavior->OnStart(*world_, entity);
        entry.started = true;
    }
    entries_.push_back(std::move(entry));
    return true;
}

void BehaviorSystem::Start(World& world) {
    Stop();
    world_ = &world;
    for (Entry& entry : entries_) {
        if (!world.IsActiveInHierarchy(entry.entity)) {
            continue;
        }
        entry.behavior->OnStart(world, entry.entity);
        entry.started = true;
    }
}

void BehaviorSystem::Update(float deltaTime) {
    if (world_ == nullptr) {
        return;
    }
    for (Entry& entry : entries_) {
        const bool active = world_->IsActiveInHierarchy(entry.entity);
        if (active && !entry.started) {
            entry.behavior->OnStart(*world_, entry.entity);
            entry.started = true;
        } else if (!active && entry.started) {
            entry.behavior->OnStop(*world_, entry.entity);
            entry.started = false;
        }
        if (entry.started && active) {
            entry.behavior->OnUpdate(*world_, entry.entity, deltaTime);
        }
    }
}

void BehaviorSystem::DispatchTriggerEvent(TriggerEvent event, EntityId entity,
                                          EntityId other) {
    if (world_ == nullptr || !world_->IsActiveInHierarchy(entity)) {
        return;
    }
    for (Entry& entry : entries_) {
        if (!entry.started || entry.entity != entity) {
            continue;
        }
        switch (event) {
        case TriggerEvent::Enter:
            entry.behavior->OnTriggerEnter(*world_, entity, other);
            break;
        case TriggerEvent::Stay:
            entry.behavior->OnTriggerStay(*world_, entity, other);
            break;
        case TriggerEvent::Exit:
            entry.behavior->OnTriggerExit(*world_, entity, other);
            break;
        }
        if (!world_->Contains(entity)) {
            return;
        }
    }
}

void BehaviorSystem::DispatchButtonClick(EntityId entity) {
    if (world_ == nullptr || !world_->IsActiveInHierarchy(entity)) {
        return;
    }
    for (Entry& entry : entries_) {
        if (!entry.started || entry.entity != entity) {
            continue;
        }
        entry.behavior->OnButtonClick(*world_, entity);
        if (!world_->Contains(entity)) {
            return;
        }
    }
}

void BehaviorSystem::DispatchToggleValueChanged(EntityId entity, bool isOn) {
    if (world_ == nullptr || !world_->IsActiveInHierarchy(entity)) {
        return;
    }
    for (Entry& entry : entries_) {
        if (!entry.started || entry.entity != entity) {
            continue;
        }
        entry.behavior->OnToggleValueChanged(*world_, entity, isOn);
        if (!world_->Contains(entity)) {
            return;
        }
    }
}

void BehaviorSystem::Stop() {
    if (world_ == nullptr) {
        return;
    }
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
        if (entry->started) {
            entry->behavior->OnStop(*world_, entry->entity);
            entry->started = false;
        }
    }
    world_ = nullptr;
}

void BehaviorSystem::Clear() {
    Stop();
    entries_.clear();
}

bool BehaviorSystem::IsRunning() const {
    return world_ != nullptr;
}

size_t BehaviorSystem::Size() const {
    return entries_.size();
}
