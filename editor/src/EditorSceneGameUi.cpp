#include "AssetImportPlanner.h"
#include "EditorScene.h"
#include "PlayerPackageBuilder.h"
#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"
#include "RuntimeSceneLoader.h"
#include "ScriptAsset.h"
#include "ScriptBuildService.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "core/WinApp.h"
#include "font/TextRenderer.h"
#include "graphics/DirectXCommon.h"
#include "graphics/LightingScene.h"
#include "graphics/RenderScene.h"
#include "graphics/ShaderPaths.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "imgui/ImguiManager.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "internal/EditorSceneGameUiUtils.h"
#include "model/MeshRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace EditorSceneGameUiUtils;

bool EditorScene::DrawGameUi(int width, int height, bool gameCameraAvailable) {
    if (!PrepareGameUiFrame(width, height)) {
        return false;
    }

    const WorldEntity* eventSystemEntity = FindEventSystemEntity(world_);
    const EventSystemComponent* eventSystem =
        eventSystemEntity != nullptr ? &*eventSystemEntity->eventSystem : nullptr;
    const bool uiEventsEnabled =
        eventSystem == nullptr ||
        (eventSystem->enabled && world_.IsActiveInHierarchy(eventSystemEntity->id));
    const ImVec2 imageScreenMin = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool canPoint = CanPointAtGameUi(imageScreenMin, mouse, uiEventsEnabled);
    const DirectX::XMFLOAT2 pointer =
        CalculateGameUiPointer(imageScreenMin, mouse, width, height);
    std::vector<EntityId> selectableButtons;
    std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash> selectableCenters;
    EntityId hoveredButton =
        CollectGameUiControls(canPoint, pointer, width, height, selectableButtons, selectableCenters);
    WorldEntity* openDropdownEntity = FindOpenGameUiDropdown();
    const int32_t hoveredDropdownOption = UpdateGameUiDropdownHover(
        openDropdownEntity, canPoint, pointer, width, height, hoveredButton);
    const bool dropdownWasOpen = openDropdownEntity != nullptr;
    WorldEntity* activeInputFieldEntity = FindActiveGameUiInputField();
    InitializeGameUiSelection(selectableButtons, eventSystem, uiEventsEnabled);
    const bool canNavigateUi = CanNavigateGameUi(eventSystem, uiEventsEnabled);
    UpdateActiveGameUiInputField(activeInputFieldEntity, canNavigateUi);
    const WorldEntity* focusedEntity = world_.Find(focusedButton_);
    const bool focusedSlider = focusedEntity != nullptr && focusedEntity->slider &&
                               focusedEntity->slider->enabled &&
                               focusedEntity->slider->interactable;
    bool navigatedUi = NavigateGameUiTab(selectableButtons, canNavigateUi);
    navigatedUi = NavigateOpenGameUiDropdown(openDropdownEntity, canNavigateUi) || navigatedUi;
    navigatedUi =
        NavigateGameUiDirection(selectableButtons, selectableCenters, canNavigateUi, focusedSlider,
                                gameCameraAvailable, openDropdownEntity != nullptr) ||
        navigatedUi;
    const bool hoveredButtonInteractable = IsGameUiButtonInteractable(hoveredButton);
    const bool hoveredSliderInteractable = IsGameUiSliderInteractable(hoveredButton);
    HandleGameUiPointerInteractions(hoveredButton, hoveredDropdownOption,
                                    hoveredButtonInteractable, hoveredSliderInteractable, pointer,
                                    width, height);
    const bool submitHeld = HandleGameUiSubmit(canNavigateUi);
    HandleGameUiKeyboardSlider(canNavigateUi, navigatedUi);
    HandleGameUiCancel(canNavigateUi);

    DrawGameUiVisuals(width, height, hoveredButton, submitHeld);
    DrawGameUiDropdownPopup(width, height);

    return hoveredButton.IsValid() || hoveredDropdownOption >= 0 ||
           (dropdownWasOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
}

bool EditorScene::NavigateGameUiDirection(
    const std::vector<EntityId>& selectableButtons,
    const std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters,
    bool canNavigateUi, bool focusedSlider, bool gameCameraAvailable, bool dropdownOpen) {
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    bool navigatedUi = false;
    enum class UiNavigationDirection : uint8_t {
        None,
        Left,
        Right,
        Up,
        Down,
    };
    UiNavigationDirection navigationDirection = UiNavigationDirection::None;
    if (canNavigateUi && !focusedSlider && !activeInputField_.IsValid() && !dropdownOpen) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
            (runtimeInput != nullptr &&
             runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_LEFT))) {
            navigationDirection = UiNavigationDirection::Left;
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_RIGHT))) {
            navigationDirection = UiNavigationDirection::Right;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_UP))) {
            navigationDirection = UiNavigationDirection::Up;
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                   (runtimeInput != nullptr &&
                    runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN))) {
            navigationDirection = UiNavigationDirection::Down;
        }
    }
    if (navigationDirection != UiNavigationDirection::None && !selectableButtons.empty() &&
        (focusedButton_.IsValid() || !gameCameraAvailable)) {
        const WorldEntity* currentControl = world_.Find(focusedButton_);
        const bool explicitNavigation =
            currentControl != nullptr && currentControl->button &&
            currentControl->button->navigation == ButtonNavigationMode::Explicit;
        if (explicitNavigation) {
            EntityId target{};
            switch (navigationDirection) {
                case UiNavigationDirection::Left:
                    target = currentControl->button->selectOnLeft;
                    break;
                case UiNavigationDirection::Right:
                    target = currentControl->button->selectOnRight;
                    break;
                case UiNavigationDirection::Up:
                    target = currentControl->button->selectOnUp;
                    break;
                case UiNavigationDirection::Down:
                    target = currentControl->button->selectOnDown;
                    break;
                case UiNavigationDirection::None:
                    break;
            }
            if (std::ranges::find(selectableButtons, target) != selectableButtons.end()) {
                focusedButton_ = target;
            }
        } else if (const auto currentCenter = selectableCenters.find(focusedButton_);
                   currentCenter == selectableCenters.end()) {
            focusedButton_ = selectableButtons.front();
        } else {
            EntityId best{};
            float bestScore = (std::numeric_limits<float>::max)();
            for (const EntityId candidate : selectableButtons) {
                if (candidate == focusedButton_) {
                    continue;
                }
                const auto candidateCenter = selectableCenters.find(candidate);
                if (candidateCenter == selectableCenters.end()) {
                    continue;
                }
                const float deltaX = candidateCenter->second.x - currentCenter->second.x;
                const float deltaY = candidateCenter->second.y - currentCenter->second.y;
                float forward = 0.0f;
                float perpendicular = 0.0f;
                switch (navigationDirection) {
                    case UiNavigationDirection::Left:
                        forward = -deltaX;
                        perpendicular = std::abs(deltaY);
                        break;
                    case UiNavigationDirection::Right:
                        forward = deltaX;
                        perpendicular = std::abs(deltaY);
                        break;
                    case UiNavigationDirection::Up:
                        forward = -deltaY;
                        perpendicular = std::abs(deltaX);
                        break;
                    case UiNavigationDirection::Down:
                        forward = deltaY;
                        perpendicular = std::abs(deltaX);
                        break;
                    case UiNavigationDirection::None:
                        break;
                }
                if (forward <= 0.0f) {
                    continue;
                }
                const float score = forward + perpendicular * perpendicular / (forward + 0.001f);
                if (score < bestScore) {
                    best = candidate;
                    bestScore = score;
                }
            }
            if (best.IsValid()) {
                focusedButton_ = best;
            }
        }
        navigatedUi = true;
    }

    return navigatedUi;
}

void EditorScene::DrawGameUiVisuals(int width, int height, EntityId hoveredButton,
                                    bool submitHeld) {
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    const std::vector<OrderedUiEntity> orderedUiEntities = GetOrderedUiEntities(world_);
    spriteRenderer.UpdateProjection(width, height);
    spriteRenderer.PreDraw(true);
    for (const OrderedUiEntity& entry : orderedUiEntities) {
        const WorldEntity& entity = *entry.entity;
        const bool drawsText = entity.text && entity.text->enabled;
        const bool drawsImage = entity.image && entity.image->enabled;
        if (!drawsText && !drawsImage) {
            continue;
        }

        float scale = 1.0f;
        DirectX::XMFLOAT2 canvasOrigin{};
        DirectX::XMFLOAT2 referenceResolution{};
        if (!TryCalculateRuntimeGameUiCanvasLayout(entity, width, height, scale, canvasOrigin,
                                                   referenceResolution)) {
            continue;
        }
        const UiGroupState groupState = GetUiGroupState(world_, entity);
        if (drawsImage) {
            const ImageComponent& image = *entity.image;
            const auto loaded = loadedTextures_.find(image.texturePath);
            const TextureHandle texture =
                loaded != loadedTextures_.end() && loaded->second.IsValid() ? loaded->second
                                                                            : TextureHandle{};
            const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
            Sprite sprite{};
            sprite.position = {
                canvasOrigin.x + (referenceResolution.x * anchor.x + image.position.x -
                                  image.size.x * image.pivot.x) *
                                     scale,
                canvasOrigin.y + (referenceResolution.y * anchor.y + image.position.y -
                                  image.size.y * image.pivot.y) *
                                     scale,
            };
            sprite.size = {
                image.size.x * scale,
                image.size.y * scale,
            };
            PreserveGameUiImageAspect(sprite, image, texture);
            const Sprite sliderTrack = sprite;
            ApplyGameUiImageFill(sprite, image);
            const DirectX::XMFLOAT4 stateColor =
                UpdateGameUiButtonColor(entity, groupState.interactable, hoveredButton, submitHeld);
            sprite.color = {
                image.color.x * stateColor.x,
                image.color.y * stateColor.y,
                image.color.z * stateColor.z,
                image.color.w * stateColor.w * groupState.alpha,
            };
            sprite.textureId = texture.IsValid() ? texture.Get() : kInvalidResourceId;
            spriteRenderer.Draw(sprite);
            DrawGameUiToggle(entity, sprite, groupState.alpha);
            DrawGameUiSlider(entity, sliderTrack, scale, groupState.alpha,
                             groupState.interactable);
        }
        if (!drawsText) {
            continue;
        }
        DrawGameUiText(entity, scale, canvasOrigin, referenceResolution, groupState.alpha);
    }
}
