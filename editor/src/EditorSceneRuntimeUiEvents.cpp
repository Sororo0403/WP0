#include "EditorScene.h"

#include "internal/EditorSceneGameUiUtils.h"

#include <utility>

using namespace EditorSceneGameUiUtils;

bool EditorScene::IsRuntimeUiEntityInteractable(const WorldEntity& entity) const {
    return world_.IsActiveInHierarchy(entity.id) &&
           GetUiGroupState(world_, entity).interactable;
}

bool EditorScene::DispatchPendingInputFieldEvents() {
    std::vector<InputFieldEvent> events = std::exchange(pendingInputFieldEvents_, {});
    for (const InputFieldEvent& event : events) {
        WorldEntity* entity = world_.Find(event.entity);
        if (entity == nullptr || !entity->inputField || !entity->inputField->enabled ||
            !entity->inputField->interactable || !entity->button ||
            !entity->button->enabled || !entity->button->interactable ||
            !IsRuntimeUiEntityInteractable(*entity)) {
            continue;
        }
        if (event.submitted) {
            runtimeBehaviors_.DispatchInputFieldSubmit(event.entity, event.text);
        } else {
            runtimeBehaviors_.DispatchInputFieldValueChanged(event.entity, event.text);
        }
        if (ApplyPendingRuntimeSceneLoad()) {
            return true;
        }
    }
    return false;
}

bool EditorScene::DispatchPendingDropdownChanges() {
    std::vector<DropdownValueChange> changes =
        std::exchange(pendingDropdownValueChanges_, {});
    for (const DropdownValueChange& change : changes) {
        WorldEntity* entity = world_.Find(change.entity);
        if (entity == nullptr || !entity->dropdown || !entity->dropdown->enabled ||
            !entity->dropdown->interactable || !entity->button ||
            !entity->button->enabled || !entity->button->interactable ||
            !IsRuntimeUiEntityInteractable(*entity)) {
            continue;
        }
        runtimeBehaviors_.DispatchDropdownValueChanged(change.entity, change.value);
        if (ApplyPendingRuntimeSceneLoad()) {
            return true;
        }
    }
    return false;
}

bool EditorScene::DispatchPendingSliderChanges() {
    std::vector<SliderValueChange> changes =
        std::exchange(pendingSliderValueChanges_, {});
    for (const SliderValueChange& change : changes) {
        WorldEntity* entity = world_.Find(change.entity);
        if (entity == nullptr || !entity->slider || !entity->slider->enabled ||
            !entity->slider->interactable || !IsRuntimeUiEntityInteractable(*entity)) {
            continue;
        }
        runtimeBehaviors_.DispatchSliderValueChanged(change.entity, change.value);
        if (ApplyPendingRuntimeSceneLoad()) {
            return true;
        }
    }
    return false;
}

bool EditorScene::DispatchPendingButtonClicks() {
    std::vector<EntityId> clicks = std::exchange(pendingButtonClicks_, {});
    for (const EntityId entityId : clicks) {
        WorldEntity* entity = world_.Find(entityId);
        if (entity == nullptr || !entity->button || !entity->button->enabled ||
            !entity->button->interactable ||
            (entity->toggle && !entity->toggle->enabled) ||
            !IsRuntimeUiEntityInteractable(*entity)) {
            continue;
        }
        if (entity->toggle) {
            entity->toggle->isOn = !entity->toggle->isOn;
            runtimeBehaviors_.DispatchToggleValueChanged(
                entityId, entity->toggle->isOn);
            if (ApplyPendingRuntimeSceneLoad()) {
                return true;
            }
        }
        runtimeBehaviors_.DispatchButtonClick(entityId);
        if (ApplyPendingRuntimeSceneLoad()) {
            return true;
        }
    }
    return false;
}

bool EditorScene::DispatchPendingRuntimeUiEvents() {
    return DispatchPendingInputFieldEvents() || DispatchPendingDropdownChanges() ||
           DispatchPendingSliderChanges() || DispatchPendingButtonClicks();
}
