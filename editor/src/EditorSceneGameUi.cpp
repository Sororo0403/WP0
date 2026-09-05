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

void EditorScene::ProcessGameUiInput(int width, int height, bool gameCameraAvailable) {
    if (ctx_ == nullptr || width <= 0 || height <= 0) return;
    Input* input = ctx_->systems.input;
    const bool hadPointerInteraction = pressedButton_.IsValid() || activeSlider_.IsValid();
    const bool hadTextInput = activeInputField_.IsValid();
    const bool hadSelection = focusedButton_.IsValid() || openDropdown_.IsValid() || hadTextInput;

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
    const bool pointerEnabled = gameInputFocused_ && uiEventsEnabled && input != nullptr &&
                                input->GetCursorMode() != CursorMode::Locked;
    if (pointerEnabled && (canPoint || hadPointerInteraction)) {
        HandleGameUiPointerInteractions(hoveredButton, hoveredDropdownOption,
                                        hoveredButtonInteractable, hoveredSliderInteractable, pointer,
                                        width, height);
    } else if (!pointerEnabled) {
        pressedButton_ = {};
        activeSlider_ = {};
    }
    const bool submitHeld = HandleGameUiSubmit(canNavigateUi);
    HandleGameUiKeyboardSlider(canNavigateUi, navigatedUi);
    HandleGameUiCancel(canNavigateUi);

    gameUiHoveredButton_ = hoveredButton;
    gameUiSubmitHeld_ = submitHeld;
    if (input == nullptr) return;
    const bool overUi = hoveredButton.IsValid() || hoveredDropdownOption >= 0;
    // Ownership is decided on press. Moving a gameplay drag over UI must not
    // steal it; a drag that started on UI remains consumed through release.
    const bool pointerUsed = pointerEnabled && (hadPointerInteraction ||
        (input->IsMouseTrigger(0) && (overUi || dropdownWasOpen)));
    if (pointerUsed) input->ConsumeMouseButton(0);
    if (pointerEnabled && overUi) input->ConsumeMouseWheel();
    if (canNavigateUi) {
        if (hadTextInput || activeInputField_.IsValid()) input->ConsumeKeyboard();
        if (navigatedUi) input->ConsumeKey(DIK_TAB);
        if (navigatedUi || focusedSlider) {
            for (int key : {DIK_LEFT, DIK_RIGHT, DIK_UP, DIK_DOWN}) input->ConsumeKey(key);
            input->ConsumeGamepadButtons(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
                                         XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
        }
        if (hadSelection || focusedButton_.IsValid()) {
            input->ConsumeKey(DIK_RETURN);
            input->ConsumeKey(DIK_SPACE);
            input->ConsumeGamepadButtons(XINPUT_GAMEPAD_A);
        }
        if (hadSelection) {
            input->ConsumeKey(DIK_ESCAPE);
            input->ConsumeGamepadButtons(XINPUT_GAMEPAD_B);
        }
    }
}

bool EditorScene::DrawGameUi(int width, int height, bool gameCameraAvailable) {
    (void)gameCameraAvailable;
    if (!PrepareGameUiFrame(width, height)) return false;
    DrawGameUiVisuals(width, height, gameUiHoveredButton_, gameUiSubmitHeld_);
    DrawGameUiDropdownPopup(width, height);
    return gameUiHoveredButton_.IsValid();
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
