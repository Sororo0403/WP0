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
    TextRenderer& textRenderer = *ctx_->rendering.text;
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    const auto resolveCanvasLayout = [&](const WorldEntity& entity, float& scale,
                                         DirectX::XMFLOAT2& origin,
                                         DirectX::XMFLOAT2& referenceResolution) {
        const CanvasComponent* canvas = FindEnabledCanvas(world_, entity);
        if (canvas == nullptr) {
            return false;
        }
        CalculateCanvasLayout(*canvas, static_cast<float>(width), static_cast<float>(height), 0.0f,
                              0.0f, scale, origin, referenceResolution);
        return true;
    };
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
        if (!resolveCanvasLayout(entity, scale, canvasOrigin, referenceResolution)) {
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
            if (image.preserveAspect && texture.IsValid() && ctx_->rendering.texture != nullptr &&
                ctx_->rendering.texture->IsValidTexture(texture)) {
                const float textureWidth =
                    static_cast<float>(ctx_->rendering.texture->GetWidth(texture));
                const float textureHeight =
                    static_cast<float>(ctx_->rendering.texture->GetHeight(texture));
                if (textureWidth > 0.0f && textureHeight > 0.0f && sprite.size.x > 0.0f &&
                    sprite.size.y > 0.0f) {
                    const DirectX::XMFLOAT2 availableSize = sprite.size;
                    const float textureAspect = textureWidth / textureHeight;
                    const float availableAspect = availableSize.x / availableSize.y;
                    if (availableAspect > textureAspect) {
                        sprite.size.x = availableSize.y * textureAspect;
                    } else {
                        sprite.size.y = availableSize.x / textureAspect;
                    }
                    sprite.position.x += (availableSize.x - sprite.size.x) * image.pivot.x;
                    sprite.position.y += (availableSize.y - sprite.size.y) * image.pivot.y;
                }
            }
            const Sprite sliderTrack = sprite;
            if (image.type == ImageType::Filled) {
                const float fillAmount = std::clamp(image.fillAmount, 0.0f, 1.0f);
                if (image.fillMethod == ImageFillMethod::Horizontal) {
                    const float fullWidth = sprite.size.x;
                    const float fullUvWidth = sprite.uvSize.x;
                    sprite.size.x = fullWidth * fillAmount;
                    sprite.uvSize.x = fullUvWidth * fillAmount;
                    if (image.fillReverse) {
                        sprite.position.x += fullWidth * (1.0f - fillAmount);
                        sprite.uvLeftTop.x += fullUvWidth * (1.0f - fillAmount);
                    }
                } else {
                    const float fullHeight = sprite.size.y;
                    const float fullUvHeight = sprite.uvSize.y;
                    sprite.size.y = fullHeight * fillAmount;
                    sprite.uvSize.y = fullUvHeight * fillAmount;
                    if (!image.fillReverse) {
                        sprite.position.y += fullHeight * (1.0f - fillAmount);
                        sprite.uvLeftTop.y += fullUvHeight * (1.0f - fillAmount);
                    }
                }
            }
            DirectX::XMFLOAT4 stateColor{1.0f, 1.0f, 1.0f, 1.0f};
            if (entity.button && entity.button->enabled) {
                const ButtonComponent& button = *entity.button;
                const bool interactable = button.interactable && groupState.interactable;
                DirectX::XMFLOAT4 targetColor =
                    interactable ? button.normalColor : button.disabledColor;
                if (interactable && (entity.id == hoveredButton || entity.id == focusedButton_)) {
                    const bool pointerPressed = entity.id == hoveredButton &&
                                                entity.id == pressedButton_ &&
                                                ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    const bool navigationPressed = entity.id == focusedButton_ && submitHeld;
                    targetColor = pointerPressed || navigationPressed ? button.pressedColor
                                                                      : button.hoveredColor;
                }
                ButtonColorTransition& transition = buttonColorTransitions_[entity.id];
                const auto colorsEqual = [](const DirectX::XMFLOAT4& lhs,
                                            const DirectX::XMFLOAT4& rhs) {
                    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
                };
                if (!transition.initialized) {
                    transition.current = targetColor;
                    transition.start = targetColor;
                    transition.target = targetColor;
                    transition.initialized = true;
                } else if (!colorsEqual(transition.target, targetColor)) {
                    transition.start = transition.current;
                    transition.target = targetColor;
                    transition.elapsed = 0.0f;
                }
                if (button.fadeDuration <= 0.0f) {
                    transition.current = targetColor;
                } else if (!colorsEqual(transition.current, transition.target)) {
                    const float deltaTime = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
                    transition.elapsed += deltaTime;
                    const float amount =
                        std::clamp(transition.elapsed / button.fadeDuration, 0.0f, 1.0f);
                    transition.current = {
                        std::lerp(transition.start.x, transition.target.x, amount),
                        std::lerp(transition.start.y, transition.target.y, amount),
                        std::lerp(transition.start.z, transition.target.z, amount),
                        std::lerp(transition.start.w, transition.target.w, amount),
                    };
                }
                stateColor = transition.current;
            }
            sprite.color = {
                image.color.x * stateColor.x,
                image.color.y * stateColor.y,
                image.color.z * stateColor.z,
                image.color.w * stateColor.w * groupState.alpha,
            };
            sprite.textureId = texture.IsValid() ? texture.Get() : kInvalidResourceId;
            spriteRenderer.Draw(sprite);
            if (entity.toggle && entity.toggle->enabled && entity.toggle->isOn) {
                const ToggleComponent& toggle = *entity.toggle;
                Sprite checkmark{};
                checkmark.size = {
                    sprite.size.x * toggle.checkmarkScale,
                    sprite.size.y * toggle.checkmarkScale,
                };
                checkmark.position = {
                    sprite.position.x + (sprite.size.x - checkmark.size.x) * 0.5f,
                    sprite.position.y + (sprite.size.y - checkmark.size.y) * 0.5f,
                };
                checkmark.color = toggle.checkmarkColor;
                checkmark.color.w *= groupState.alpha;
                checkmark.textureId = kInvalidResourceId;
                spriteRenderer.Draw(checkmark);
            }
            if (entity.slider && entity.slider->enabled) {
                const SliderComponent& slider = *entity.slider;
                const float normalized = std::clamp((slider.value - slider.minValue) /
                                                        (slider.maxValue - slider.minValue),
                                                    0.0f, 1.0f);
                const float interactionAlpha =
                    slider.interactable && groupState.interactable ? 1.0f : 0.5f;
                Sprite fill = sliderTrack;
                fill.textureId = kInvalidResourceId;
                fill.color = slider.fillColor;
                fill.color.w *= groupState.alpha * interactionAlpha;
                if (slider.direction == SliderDirection::LeftToRight ||
                    slider.direction == SliderDirection::RightToLeft) {
                    const float fullWidth = fill.size.x;
                    fill.size.x *= normalized;
                    if (slider.direction == SliderDirection::RightToLeft) {
                        fill.position.x += fullWidth - fill.size.x;
                    }
                } else {
                    const float fullHeight = fill.size.y;
                    fill.size.y *= normalized;
                    if (slider.direction == SliderDirection::BottomToTop) {
                        fill.position.y += fullHeight - fill.size.y;
                    }
                }
                if (fill.size.x > 0.0f && fill.size.y > 0.0f) {
                    spriteRenderer.Draw(fill);
                }

                Sprite handle{};
                const float handleSize = slider.handleSize * scale;
                handle.size = {handleSize, handleSize};
                if (slider.direction == SliderDirection::LeftToRight ||
                    slider.direction == SliderDirection::RightToLeft) {
                    float position = normalized;
                    if (slider.direction == SliderDirection::RightToLeft) {
                        position = 1.0f - position;
                    }
                    handle.position = {
                        sliderTrack.position.x + sliderTrack.size.x * position - handleSize * 0.5f,
                        sliderTrack.position.y + (sliderTrack.size.y - handleSize) * 0.5f,
                    };
                } else {
                    float position = normalized;
                    if (slider.direction == SliderDirection::BottomToTop) {
                        position = 1.0f - position;
                    }
                    handle.position = {
                        sliderTrack.position.x + (sliderTrack.size.x - handleSize) * 0.5f,
                        sliderTrack.position.y + sliderTrack.size.y * position - handleSize * 0.5f,
                    };
                }
                handle.color = slider.handleColor;
                handle.color.w *= groupState.alpha * interactionAlpha;
                handle.textureId = kInvalidResourceId;
                if (handleSize > 0.0f) {
                    spriteRenderer.Draw(handle);
                }
            }
        }
        if (!drawsText) {
            continue;
        }
        const TextComponent& text = *entity.text;
        std::string displayText = text.text;
        if (entity.inputField && entity.inputField->enabled) {
            const InputFieldComponent& inputField = *entity.inputField;
            if (inputField.text.empty()) {
                displayText =
                    entity.id == activeInputField_ ? std::string{} : inputField.placeholder;
            } else if (inputField.contentType == InputFieldContentType::Password) {
                displayText.assign(CountUtf8Codepoints(inputField.text), '*');
            } else {
                displayText = inputField.text;
            }
            if (entity.id == activeInputField_) {
                displayText.push_back('|');
            }
        } else if (entity.dropdown && entity.dropdown->enabled &&
                   !entity.dropdown->options.empty() && entity.dropdown->value >= 0 &&
                   static_cast<size_t>(entity.dropdown->value) < entity.dropdown->options.size()) {
            displayText = entity.dropdown->options[static_cast<size_t>(entity.dropdown->value)];
        }
        TextStyle style{};
        if (const auto loadedFont = loadedFonts_.find(text.fontPath);
            loadedFont != loadedFonts_.end()) {
            style.font = loadedFont->second;
        }
        style.pixelSize = text.fontSize * scale;
        style.lineSpacing = text.lineSpacing * scale;
        style.wrapWidth = text.wrapWidth * scale;
        style.horizontalAlignment =
            text.alignment == TextAlignment::Center  ? TextHorizontalAlignment::Center
            : text.alignment == TextAlignment::Right ? TextHorizontalAlignment::Right
                                                     : TextHorizontalAlignment::Left;
        style.color = text.color;
        style.color.w *= groupState.alpha;
        const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(text.anchor).factor;
        DirectX::XMFLOAT2 position{
            canvasOrigin.x + (referenceResolution.x * anchor.x + text.position.x) * scale,
            canvasOrigin.y + (referenceResolution.y * anchor.y + text.position.y) * scale,
        };
        if (text.alignment != TextAlignment::Left || anchor.y > 0.0f) {
            const TextLayoutMetrics metrics = textRenderer.MeasureText(displayText, style);
            if (text.alignment != TextAlignment::Left) {
                position.x -= text.alignment == TextAlignment::Center ? metrics.size.x * 0.5f
                                                                      : metrics.size.x;
            }
            position.y -= metrics.size.y * anchor.y;
        }
        textRenderer.DrawText(displayText, position, style);
    }
}
