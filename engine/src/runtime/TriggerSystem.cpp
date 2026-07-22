#include "runtime/TriggerSystem.h"

#include "collision/CollisionUtil.h"
#include "runtime/BehaviorSystem.h"
#include "world/World.h"
#include "world/WorldCollision.h"

#include <utility>

void TriggerSystem::Update(const World& world, BehaviorSystem& behaviors) {
    const auto makePair = [](EntityId first, EntityId second) {
        const bool secondComesFirst =
            second.high != first.high ? second.high < first.high : second.low < first.low;
        if (secondComesFirst) {
            std::swap(first, second);
        }
        return Pair{first, second};
    };

    std::set<Pair> currentPairs;
    const std::vector<WorldEntity>& entities = world.Entities();

    for (size_t firstIndex = 0u; firstIndex < entities.size(); ++firstIndex) {
        const WorldEntity& first = entities[firstIndex];
        if (!world.IsActiveInHierarchy(first.id) || !first.boxCollider ||
            !first.boxCollider->enabled) {
            continue;
        }
        for (size_t secondIndex = firstIndex + 1u; secondIndex < entities.size();
             ++secondIndex) {
            const WorldEntity& second = entities[secondIndex];
            if (!world.IsActiveInHierarchy(second.id) || !second.boxCollider ||
                !second.boxCollider->enabled ||
                (!first.boxCollider->isTrigger && !second.boxCollider->isTrigger) ||
                !world.LayersCollide(first.layer, second.layer)) {
                continue;
            }
            OBB firstBox{};
            OBB secondBox{};
            if (TryBuildWorldBoxCollider(world, first.id, firstBox) &&
                TryBuildWorldBoxCollider(world, second.id, secondBox) &&
                CollisionUtil::CheckOBB(firstBox, secondBox)) {
                currentPairs.insert(makePair(first.id, second.id));
            }
        }
    }

    for (const WorldEntity& character : entities) {
        if (!world.IsActiveInHierarchy(character.id) || !character.characterController ||
            !character.characterController->enabled) {
            continue;
        }
        for (const WorldEntity& trigger : entities) {
            if (trigger.id == character.id || !world.IsActiveInHierarchy(trigger.id) ||
                !trigger.boxCollider ||
                !trigger.boxCollider->enabled || !trigger.boxCollider->isTrigger) {
                continue;
            }
            if (CheckCharacterControllerBoxOverlap(world, character.id, trigger.id)) {
                currentPairs.insert(makePair(character.id, trigger.id));
            }
        }
    }

    for (const Pair& pair : currentPairs) {
        const BehaviorSystem::TriggerEvent event = activePairs_.contains(pair)
                                                       ? BehaviorSystem::TriggerEvent::Stay
                                                       : BehaviorSystem::TriggerEvent::Enter;
        behaviors.DispatchTriggerEvent(event, pair.first, pair.second);
        behaviors.DispatchTriggerEvent(event, pair.second, pair.first);
    }
    for (const Pair& pair : activePairs_) {
        if (currentPairs.contains(pair)) {
            continue;
        }
        behaviors.DispatchTriggerEvent(BehaviorSystem::TriggerEvent::Exit, pair.first,
                                       pair.second);
        behaviors.DispatchTriggerEvent(BehaviorSystem::TriggerEvent::Exit, pair.second,
                                       pair.first);
    }
    activePairs_ = std::move(currentPairs);
}

void TriggerSystem::Clear() {
    activePairs_.clear();
}

size_t TriggerSystem::ActivePairCount() const {
    return activePairs_.size();
}
