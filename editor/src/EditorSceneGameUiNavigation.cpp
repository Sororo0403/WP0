#include "EditorScene.h"

#include "imgui.h"
#include "input/Input.h"

#include <Windows.h>

#include <cmath>
#include <limits>
#include <ranges>

namespace {
bool IsNavigationPressed(const Input* input, const ImGuiKey key, const WORD gamepadButton) {
    return ImGui::IsKeyPressed(key, false) ||
           (input != nullptr && input->IsGamepadButtonTrigger(gamepadButton));
}

struct NavigationOffsets {
    float forward = 0.0f;
    float perpendicular = 0.0f;
};
}  // namespace

bool EditorScene::NavigateGameUiDirection(
    const std::vector<EntityId>& selectableButtons,
    const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters,
    const bool canNavigateUi, const bool focusedSlider, const bool gameCameraAvailable,
    const bool dropdownOpen) {
    const UiNavigationDirection direction =
        ReadGameUiNavigationDirection(canNavigateUi, focusedSlider, dropdownOpen);
    if (direction == UiNavigationDirection::None || selectableButtons.empty() ||
        (!focusedButton_.IsValid() && gameCameraAvailable)) {
        return false;
    }
    const WorldEntity* currentControl = world_.Find(focusedButton_);
    const bool explicitNavigation =
        currentControl != nullptr && currentControl->button &&
        currentControl->button->navigation == ButtonNavigationMode::Explicit;
    if (explicitNavigation) {
        NavigateGameUiExplicit(*currentControl, direction, selectableButtons);
    } else {
        NavigateGameUiAutomatic(direction, selectableButtons, selectableCenters);
    }
    return true;
}

EditorScene::UiNavigationDirection EditorScene::ReadGameUiNavigationDirection(
    const bool canNavigateUi, const bool focusedSlider, const bool dropdownOpen) const {
    if (!IsGameUiDirectionalNavigationEnabled(canNavigateUi, focusedSlider, dropdownOpen)) {
        return UiNavigationDirection::None;
    }
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (IsNavigationPressed(runtimeInput, ImGuiKey_LeftArrow, XINPUT_GAMEPAD_DPAD_LEFT)) {
        return UiNavigationDirection::Left;
    }
    if (IsNavigationPressed(runtimeInput, ImGuiKey_RightArrow, XINPUT_GAMEPAD_DPAD_RIGHT)) {
        return UiNavigationDirection::Right;
    }
    if (IsNavigationPressed(runtimeInput, ImGuiKey_UpArrow, XINPUT_GAMEPAD_DPAD_UP)) {
        return UiNavigationDirection::Up;
    }
    if (IsNavigationPressed(runtimeInput, ImGuiKey_DownArrow, XINPUT_GAMEPAD_DPAD_DOWN)) {
        return UiNavigationDirection::Down;
    }
    return UiNavigationDirection::None;
}

bool EditorScene::IsGameUiDirectionalNavigationEnabled(
    const bool canNavigateUi, const bool focusedSlider, const bool dropdownOpen) const {
    return canNavigateUi && !focusedSlider && !activeInputField_.IsValid() && !dropdownOpen;
}

EntityId EditorScene::ResolveExplicitGameUiNavigationTarget(
    const WorldEntity& control, const UiNavigationDirection direction) const {
    switch (direction) {
        case UiNavigationDirection::Left:
            return control.button->selectOnLeft;
        case UiNavigationDirection::Right:
            return control.button->selectOnRight;
        case UiNavigationDirection::Up:
            return control.button->selectOnUp;
        case UiNavigationDirection::Down:
            return control.button->selectOnDown;
        case UiNavigationDirection::None:
            return {};
    }
    return {};
}

void EditorScene::NavigateGameUiExplicit(
    const WorldEntity& control, const UiNavigationDirection direction,
    const std::vector<EntityId>& selectableButtons) {
    const EntityId target = ResolveExplicitGameUiNavigationTarget(control, direction);
    if (std::ranges::find(selectableButtons, target) != selectableButtons.end()) {
        focusedButton_ = target;
    }
}

void EditorScene::NavigateGameUiAutomatic(
    const UiNavigationDirection direction, const std::vector<EntityId>& selectableButtons,
    const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters) {
    const auto currentCenter = selectableCenters.find(focusedButton_);
    if (currentCenter == selectableCenters.end()) {
        focusedButton_ = selectableButtons.front();
        return;
    }
    const EntityId target =
        FindBestGameUiNavigationTarget(direction, currentCenter->second, selectableButtons,
                                       selectableCenters);
    if (target.IsValid()) {
        focusedButton_ = target;
    }
}

EntityId EditorScene::FindBestGameUiNavigationTarget(
    const UiNavigationDirection direction, const DirectX::XMFLOAT2& currentCenter,
    const std::vector<EntityId>& selectableButtons,
    const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters) const {
    EntityId best{};
    float bestScore = (std::numeric_limits<float>::max)();
    for (const EntityId candidate : selectableButtons) {
        const auto candidateCenter = selectableCenters.find(candidate);
        if (candidate == focusedButton_ || candidateCenter == selectableCenters.end()) {
            continue;
        }
        const float score =
            CalculateGameUiNavigationScore(direction, currentCenter, candidateCenter->second);
        if (score < bestScore) {
            best = candidate;
            bestScore = score;
        }
    }
    return best;
}

float EditorScene::CalculateGameUiNavigationScore(
    const UiNavigationDirection direction, const DirectX::XMFLOAT2& currentCenter,
    const DirectX::XMFLOAT2& candidateCenter) const {
    const float deltaX = candidateCenter.x - currentCenter.x;
    const float deltaY = candidateCenter.y - currentCenter.y;
    NavigationOffsets offsets{};
    switch (direction) {
        case UiNavigationDirection::Left:
            offsets = {-deltaX, std::abs(deltaY)};
            break;
        case UiNavigationDirection::Right:
            offsets = {deltaX, std::abs(deltaY)};
            break;
        case UiNavigationDirection::Up:
            offsets = {-deltaY, std::abs(deltaX)};
            break;
        case UiNavigationDirection::Down:
            offsets = {deltaY, std::abs(deltaX)};
            break;
        case UiNavigationDirection::None:
            break;
    }
    if (offsets.forward <= 0.0f) {
        return (std::numeric_limits<float>::max)();
    }
    return offsets.forward +
           offsets.perpendicular * offsets.perpendicular / (offsets.forward + 0.001f);
}
