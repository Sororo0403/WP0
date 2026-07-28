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

namespace {
size_t CountUtf8Codepoints(std::string_view text) {
    return static_cast<size_t>(std::ranges::count_if(
        text, [](char value) { return (static_cast<unsigned char>(value) & 0xc0u) != 0x80u; }));
}

void PopUtf8Codepoint(std::string& text) {
    if (text.empty()) {
        return;
    }
    size_t index = text.size() - 1u;
    while (index > 0u && (static_cast<unsigned char>(text[index]) & 0xc0u) == 0x80u) {
        --index;
    }
    text.erase(index);
}

struct UiAnchorChoice {
    UiAnchor value = UiAnchor::TopLeft;
    const char* label = "";
    DirectX::XMFLOAT2 factor{};
};

constexpr std::array<UiAnchorChoice, 9> kUiAnchorChoices = {{
    {UiAnchor::TopLeft, "Top Left", {0.0f, 0.0f}},
    {UiAnchor::TopCenter, "Top Center", {0.5f, 0.0f}},
    {UiAnchor::TopRight, "Top Right", {1.0f, 0.0f}},
    {UiAnchor::MiddleLeft, "Middle Left", {0.0f, 0.5f}},
    {UiAnchor::Center, "Center", {0.5f, 0.5f}},
    {UiAnchor::MiddleRight, "Middle Right", {1.0f, 0.5f}},
    {UiAnchor::BottomLeft, "Bottom Left", {0.0f, 1.0f}},
    {UiAnchor::BottomCenter, "Bottom Center", {0.5f, 1.0f}},
    {UiAnchor::BottomRight, "Bottom Right", {1.0f, 1.0f}},
}};

const UiAnchorChoice& GetUiAnchorChoice(UiAnchor anchor) {
    const auto choice =
        std::ranges::find_if(kUiAnchorChoices, [anchor](const UiAnchorChoice& candidate) {
            return candidate.value == anchor;
        });
    return choice != kUiAnchorChoices.end() ? *choice : kUiAnchorChoices.front();
}

const CanvasComponent* FindEnabledCanvas(const World& world, const WorldEntity& entity) {
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvas) {
            return current->canvas->enabled ? &*current->canvas : nullptr;
        }
        current = current->parent.IsValid() ? world.Find(current->parent) : nullptr;
    }
    return nullptr;
}

struct UiGroupState {
    float alpha = 1.0f;
    bool interactable = true;
    bool blocksRaycasts = true;
};

UiGroupState GetUiGroupState(const World& world, const WorldEntity& entity) {
    UiGroupState state{};
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvasGroup && current->canvasGroup->enabled) {
            const CanvasGroupComponent& group = *current->canvasGroup;
            state.alpha *= group.alpha;
            state.interactable = state.interactable && group.interactable;
            state.blocksRaycasts = state.blocksRaycasts && group.blocksRaycasts;
        }
        current = current->parent.IsValid() ? world.Find(current->parent) : nullptr;
    }
    return state;
}

const WorldEntity* FindEventSystemEntity(const World& world) {
    for (const WorldEntity& entity : world.Entities()) {
        if (entity.eventSystem) {
            return &entity;
        }
    }
    return nullptr;
}

void CalculateCanvasLayout(const CanvasComponent& canvas, float width, float height, float offsetX,
                           float offsetY, float& scale, DirectX::XMFLOAT2& origin,
                           DirectX::XMFLOAT2& layoutResolution) {
    if (canvas.scaleMode == CanvasScaleMode::ConstantPixelSize) {
        scale = 1.0f;
        origin = {offsetX, offsetY};
        layoutResolution = {width, height};
        return;
    }
    layoutResolution = canvas.referenceResolution;
    const float widthScale = width / canvas.referenceResolution.x;
    const float heightScale = height / canvas.referenceResolution.y;
    switch (canvas.screenMatchMode) {
        case CanvasScreenMatchMode::MatchWidthOrHeight:
            scale = std::exp2(std::lerp(std::log2(widthScale), std::log2(heightScale),
                                        canvas.matchWidthOrHeight));
            break;
        case CanvasScreenMatchMode::Expand:
            scale = (std::min)(widthScale, heightScale);
            break;
        case CanvasScreenMatchMode::Shrink:
            scale = (std::max)(widthScale, heightScale);
            break;
    }
    origin = {
        offsetX + (width - canvas.referenceResolution.x * scale) * 0.5f,
        offsetY + (height - canvas.referenceResolution.y * scale) * 0.5f,
    };
}

struct OrderedUiEntity {
    const WorldEntity* entity = nullptr;
    int32_t sortingOrder = 0;
};

std::vector<OrderedUiEntity> GetOrderedUiEntities(const World& world) {
    std::vector<OrderedUiEntity> result;
    for (const WorldEntity& entity : world.Entities()) {
        if ((!entity.text && !entity.image && !entity.button) ||
            !world.IsActiveInHierarchy(entity.id)) {
            continue;
        }
        const CanvasComponent* canvas = FindEnabledCanvas(world, entity);
        if (canvas != nullptr) {
            result.push_back({&entity, canvas->sortingOrder});
        }
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const OrderedUiEntity& left, const OrderedUiEntity& right) {
                         return left.sortingOrder < right.sortingOrder;
                     });
    return result;
}

} // namespace

bool EditorScene::DrawGameUi(int width, int height, bool gameCameraAvailable) {
    if (ctx_ == nullptr || ctx_->rendering.text == nullptr ||
        ctx_->rendering.spriteRenderer == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    TextRenderer& textRenderer = *ctx_->rendering.text;
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    if (!textRenderer.IsReady() || !spriteRenderer.IsReady()) {
        return false;
    }

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
    const WorldEntity* eventSystemEntity = FindEventSystemEntity(world_);
    const EventSystemComponent* eventSystem =
        eventSystemEntity != nullptr ? &*eventSystemEntity->eventSystem : nullptr;
    const bool uiEventsEnabled =
        eventSystem == nullptr ||
        (eventSystem->enabled && world_.IsActiveInHierarchy(eventSystemEntity->id));
    for (auto transition = buttonColorTransitions_.begin();
         transition != buttonColorTransitions_.end();) {
        const WorldEntity* entity = world_.Find(transition->first);
        if (entity == nullptr || !entity->button || !entity->image) {
            transition = buttonColorTransitions_.erase(transition);
        } else {
            ++transition;
        }
    }

    const ImVec2 imageScreenMin = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool canPoint =
        playModeState_ == PlayModeState::Playing && uiEventsEnabled && !gameInputCaptured_ &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && requestedGameWidth_ > 0 &&
        requestedGameHeight_ > 0 && mouse.x >= imageScreenMin.x && mouse.y >= imageScreenMin.y &&
        mouse.x <= imageScreenMin.x + requestedGameWidth_ &&
        mouse.y <= imageScreenMin.y + requestedGameHeight_;
    const DirectX::XMFLOAT2 pointer{
        (mouse.x - imageScreenMin.x) * static_cast<float>(width) /
            static_cast<float>((std::max)(1, requestedGameWidth_)),
        (mouse.y - imageScreenMin.y) * static_cast<float>(height) /
            static_cast<float>((std::max)(1, requestedGameHeight_)),
    };
    const auto calculateImageRect = [&](const WorldEntity& entity, float& left, float& top,
                                        float& right, float& bottom) {
        if (!entity.image || !entity.image->enabled) {
            return false;
        }
        float scale = 1.0f;
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT2 referenceResolution{};
        if (!resolveCanvasLayout(entity, scale, origin, referenceResolution)) {
            return false;
        }
        const ImageComponent& image = *entity.image;
        const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
        left = origin.x + (referenceResolution.x * anchor.x + image.position.x -
                           image.size.x * image.pivot.x) *
                              scale;
        top = origin.y +
              (referenceResolution.y * anchor.y + image.position.y - image.size.y * image.pivot.y) *
                  scale;
        right = left + image.size.x * scale;
        bottom = top + image.size.y * scale;
        return true;
    };
    const auto setSliderValue = [&](WorldEntity& entity, float requestedValue) {
        SliderComponent& slider = *entity.slider;
        float value = std::clamp(requestedValue, slider.minValue, slider.maxValue);
        if (slider.wholeNumbers) {
            value = std::clamp(std::round(value), slider.minValue, slider.maxValue);
        }
        if (value == slider.value) {
            return;
        }
        slider.value = value;
        const auto pending =
            std::ranges::find(pendingSliderValueChanges_, entity.id, &SliderValueChange::entity);
        if (pending != pendingSliderValueChanges_.end()) {
            pending->value = value;
        } else {
            pendingSliderValueChanges_.push_back({entity.id, value});
        }
    };
    const auto setSliderValueFromPointer = [&](WorldEntity& entity) {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (!calculateImageRect(entity, left, top, right, bottom)) {
            return;
        }
        SliderComponent& slider = *entity.slider;
        float normalized = 0.0f;
        if (slider.direction == SliderDirection::LeftToRight ||
            slider.direction == SliderDirection::RightToLeft) {
            normalized = (pointer.x - left) / (std::max)(right - left, 0.0001f);
            if (slider.direction == SliderDirection::RightToLeft) {
                normalized = 1.0f - normalized;
            }
        } else {
            normalized = (pointer.y - top) / (std::max)(bottom - top, 0.0001f);
            if (slider.direction == SliderDirection::BottomToTop) {
                normalized = 1.0f - normalized;
            }
        }
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        setSliderValue(entity, std::lerp(slider.minValue, slider.maxValue, normalized));
    };
    const auto queueInputFieldEvent = [&](EntityId entity, const std::string& text,
                                          bool submitted) {
        if (!submitted) {
            const auto pending =
                std::ranges::find_if(pendingInputFieldEvents_, [&](const InputFieldEvent& event) {
                    return event.entity == entity && !event.submitted;
                });
            if (pending != pendingInputFieldEvents_.end()) {
                pending->text = text;
                return;
            }
        }
        pendingInputFieldEvents_.push_back({entity, text, submitted});
    };

    EntityId hoveredButton{};
    std::vector<EntityId> selectableButtons;
    std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash> selectableCenters;
    for (const OrderedUiEntity& entry : orderedUiEntities) {
        const WorldEntity& entity = *entry.entity;
        const bool isButton = entity.button && entity.button->enabled &&
                              (!entity.toggle || entity.toggle->enabled) &&
                              (!entity.dropdown || entity.dropdown->enabled) &&
                              (!entity.inputField || entity.inputField->enabled);
        const bool isSlider = entity.slider && entity.slider->enabled;
        if ((!isButton && !isSlider) || !entity.image || !entity.image->enabled) {
            continue;
        }
        const UiGroupState groupState = GetUiGroupState(world_, entity);
        const bool controlInteractable =
            groupState.interactable &&
            (isSlider ? entity.slider->interactable
                      : entity.button->interactable &&
                            (!entity.dropdown || entity.dropdown->interactable) &&
                            (!entity.inputField || entity.inputField->interactable));
        const bool navigationEnabled =
            isSlider || entity.button->navigation != ButtonNavigationMode::None;
        if (controlInteractable && navigationEnabled) {
            selectableButtons.push_back(entity.id);
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (calculateImageRect(entity, left, top, right, bottom)) {
                selectableCenters.emplace(
                    entity.id, DirectX::XMFLOAT2{(left + right) * 0.5f, (top + bottom) * 0.5f});
            }
        }
        if (canPoint && groupState.blocksRaycasts) {
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (calculateImageRect(entity, left, top, right, bottom) && pointer.x >= left &&
                pointer.x <= right && pointer.y >= top && pointer.y <= bottom) {
                hoveredButton = entity.id;
            }
        }
    }

    WorldEntity* openDropdownEntity = world_.Find(openDropdown_);
    if (openDropdownEntity == nullptr || !openDropdownEntity->dropdown ||
        !openDropdownEntity->dropdown->enabled || !openDropdownEntity->dropdown->interactable ||
        !openDropdownEntity->button || !openDropdownEntity->button->enabled ||
        !openDropdownEntity->button->interactable || !openDropdownEntity->image ||
        !GetUiGroupState(world_, *openDropdownEntity).interactable) {
        openDropdown_ = {};
        openDropdownEntity = nullptr;
    }
    int32_t hoveredDropdownOption = -1;
    float dropdownLeft = 0.0f;
    float dropdownTop = 0.0f;
    float dropdownRight = 0.0f;
    float dropdownBottom = 0.0f;
    float dropdownScale = 1.0f;
    if (openDropdownEntity != nullptr &&
        calculateImageRect(*openDropdownEntity, dropdownLeft, dropdownTop, dropdownRight,
                           dropdownBottom)) {
        DirectX::XMFLOAT2 dropdownOrigin{};
        DirectX::XMFLOAT2 dropdownResolution{};
        resolveCanvasLayout(*openDropdownEntity, dropdownScale, dropdownOrigin, dropdownResolution);
        const DropdownComponent& dropdown = *openDropdownEntity->dropdown;
        const float rowHeight = dropdown.itemHeight * dropdownScale;
        if (canPoint && pointer.x >= dropdownLeft && pointer.x <= dropdownRight &&
            pointer.y >= dropdownBottom &&
            pointer.y < dropdownBottom + rowHeight * static_cast<float>(dropdown.options.size())) {
            hoveredDropdownOption = static_cast<int32_t>((pointer.y - dropdownBottom) / rowHeight);
            dropdownHighlightedIndex_ = hoveredDropdownOption;
            hoveredButton = {};
        }
    }
    const bool dropdownWasOpen = openDropdownEntity != nullptr;
    WorldEntity* activeInputFieldEntity = world_.Find(activeInputField_);
    if (activeInputFieldEntity == nullptr || !activeInputFieldEntity->inputField ||
        !activeInputFieldEntity->inputField->enabled ||
        !activeInputFieldEntity->inputField->interactable || !activeInputFieldEntity->button ||
        !activeInputFieldEntity->button->enabled || !activeInputFieldEntity->button->interactable ||
        !GetUiGroupState(world_, *activeInputFieldEntity).interactable) {
        activeInputField_ = {};
        activeInputFieldEntity = nullptr;
    }

    if (focusedButton_.IsValid() &&
        std::ranges::find(selectableButtons, focusedButton_) == selectableButtons.end()) {
        focusedButton_ = {};
    }
    if (playModeState_ == PlayModeState::Playing && uiEventsEnabled &&
        !runtimeInitialUiSelectionApplied_) {
        runtimeInitialUiSelectionApplied_ = true;
        if (eventSystem != nullptr &&
            std::ranges::find(selectableButtons, eventSystem->firstSelected) !=
                selectableButtons.end()) {
            focusedButton_ = eventSystem->firstSelected;
        }
    }
    const bool canNavigateUi = playModeState_ == PlayModeState::Playing && uiEventsEnabled &&
                               !gameInputCaptured_ &&
                               (eventSystem == nullptr || eventSystem->sendNavigationEvents) &&
                               ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                               !ImGui::GetIO().WantTextInput;
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (canNavigateUi && activeInputFieldEntity != nullptr) {
        InputFieldComponent& inputField = *activeInputFieldEntity->inputField;
        bool changed = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !inputField.text.empty()) {
            PopUtf8Codepoint(inputField.text);
            changed = true;
        }
        for (const ImWchar character : ImGui::GetIO().InputQueueCharacters) {
            if (character < 0x20u || character == 0x7fu) {
                continue;
            }
            if (inputField.characterLimit > 0 &&
                CountUtf8Codepoints(inputField.text) >=
                    static_cast<size_t>(inputField.characterLimit)) {
                break;
            }
            char encoded[5]{};
            const int length = ImTextCharToUtf8(encoded, static_cast<unsigned int>(character));
            if (length <= 0 || inputField.text.size() + static_cast<size_t>(length) > 4096u) {
                continue;
            }
            inputField.text.append(encoded, static_cast<size_t>(length));
            changed = true;
        }
        if (changed) {
            queueInputFieldEvent(activeInputField_, inputField.text, false);
        }
    }
    const WorldEntity* focusedEntity = world_.Find(focusedButton_);
    const bool focusedSlider = focusedEntity != nullptr && focusedEntity->slider &&
                               focusedEntity->slider->enabled &&
                               focusedEntity->slider->interactable;
    const bool keyboardNavigation =
        canNavigateUi && !activeInputField_.IsValid() && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    bool navigatedUi = false;
    if (keyboardNavigation && !selectableButtons.empty()) {
        const bool selectPrevious = ImGui::GetIO().KeyShift;
        const auto focused = std::ranges::find(selectableButtons, focusedButton_);
        if (focused == selectableButtons.end()) {
            focusedButton_ = selectPrevious ? selectableButtons.back() : selectableButtons.front();
        } else {
            const size_t index = static_cast<size_t>(focused - selectableButtons.begin());
            focusedButton_ = selectPrevious
                                 ? selectableButtons[(index + selectableButtons.size() - 1u) %
                                                     selectableButtons.size()]
                                 : selectableButtons[(index + 1u) % selectableButtons.size()];
        }
        navigatedUi = true;
    }
    if (canNavigateUi && openDropdownEntity != nullptr) {
        const int32_t optionCount =
            static_cast<int32_t>(openDropdownEntity->dropdown->options.size());
        const bool selectPrevious = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                                    (runtimeInput != nullptr &&
                                     runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_UP));
        const bool selectNext = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                                (runtimeInput != nullptr &&
                                 runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN));
        if (selectPrevious) {
            dropdownHighlightedIndex_ = (dropdownHighlightedIndex_ + optionCount - 1) % optionCount;
            navigatedUi = true;
        } else if (selectNext) {
            dropdownHighlightedIndex_ = (dropdownHighlightedIndex_ + 1) % optionCount;
            navigatedUi = true;
        }
    }
    navigatedUi =
        NavigateGameUiDirection(selectableButtons, selectableCenters, canNavigateUi, focusedSlider,
                                gameCameraAvailable, openDropdownEntity != nullptr) ||
        navigatedUi;
    const WorldEntity* hoveredEntity = world_.Find(hoveredButton);
    const bool hoveredButtonInteractable =
        hoveredEntity != nullptr && hoveredEntity->button && hoveredEntity->button->enabled &&
        hoveredEntity->button->interactable &&
        (!hoveredEntity->dropdown ||
         (hoveredEntity->dropdown->enabled && hoveredEntity->dropdown->interactable)) &&
        (!hoveredEntity->inputField ||
         (hoveredEntity->inputField->enabled && hoveredEntity->inputField->interactable)) &&
        GetUiGroupState(world_, *hoveredEntity).interactable;
    const bool hoveredSliderInteractable =
        hoveredEntity != nullptr && hoveredEntity->slider && hoveredEntity->slider->enabled &&
        hoveredEntity->slider->interactable && GetUiGroupState(world_, *hoveredEntity).interactable;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (activeInputField_.IsValid() && hoveredButton != activeInputField_) {
            activeInputField_ = {};
            activeInputFieldEntity = nullptr;
        }
        if (openDropdownEntity != nullptr && hoveredDropdownOption >= 0) {
            DropdownComponent& dropdown = *openDropdownEntity->dropdown;
            if (dropdown.value != hoveredDropdownOption) {
                dropdown.value = hoveredDropdownOption;
                pendingDropdownValueChanges_.push_back({openDropdown_, dropdown.value});
            }
            focusedButton_ = openDropdown_;
            openDropdown_ = {};
            openDropdownEntity = nullptr;
            pressedButton_ = {};
            activeSlider_ = {};
        } else if (hoveredSliderInteractable) {
            activeSlider_ = hoveredButton;
            pressedButton_ = {};
            focusedButton_ = hoveredButton;
            setSliderValueFromPointer(*world_.Find(activeSlider_));
        } else {
            activeSlider_ = {};
            if (openDropdownEntity != nullptr && hoveredButton != openDropdown_) {
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            }
            pressedButton_ = hoveredButtonInteractable ? hoveredButton : EntityId{};
            focusedButton_ = hoveredButtonInteractable ? hoveredButton : EntityId{};
        }
    }
    if (activeSlider_.IsValid() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* sliderEntity = world_.Find(activeSlider_);
        if (sliderEntity != nullptr && sliderEntity->slider && sliderEntity->slider->enabled &&
            sliderEntity->slider->interactable &&
            GetUiGroupState(world_, *sliderEntity).interactable) {
            setSliderValueFromPointer(*sliderEntity);
        } else {
            activeSlider_ = {};
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (pressedButton_.IsValid() && pressedButton_ == hoveredButton &&
            hoveredButtonInteractable) {
            WorldEntity* clicked = world_.Find(pressedButton_);
            if (clicked != nullptr && clicked->dropdown && clicked->dropdown->enabled &&
                clicked->dropdown->interactable) {
                if (openDropdown_ == pressedButton_) {
                    openDropdown_ = {};
                    openDropdownEntity = nullptr;
                } else {
                    openDropdown_ = pressedButton_;
                    openDropdownEntity = clicked;
                    dropdownHighlightedIndex_ = clicked->dropdown->value;
                }
            } else if (clicked != nullptr && clicked->inputField && clicked->inputField->enabled &&
                       clicked->inputField->interactable) {
                activeInputField_ = clicked->id;
                activeInputFieldEntity = clicked;
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            } else {
                pendingButtonClicks_.push_back(pressedButton_);
            }
        }
        pressedButton_ = {};
        activeSlider_ = {};
    }
    const bool gamepadSubmit = canNavigateUi && runtimeInput != nullptr &&
                               runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_A);
    const bool submitHeld =
        canNavigateUi &&
        (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_Space) ||
         (runtimeInput != nullptr && runtimeInput->IsGamepadButtonPress(XINPUT_GAMEPAD_A)));
    const WorldEntity* submitEntity = world_.Find(focusedButton_);
    if (canNavigateUi && submitEntity != nullptr && submitEntity->button &&
        submitEntity->button->enabled &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
         gamepadSubmit)) {
        const bool inputFieldSubmit = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || gamepadSubmit;
        if (submitEntity->inputField && submitEntity->inputField->enabled &&
            submitEntity->inputField->interactable) {
            if (inputFieldSubmit) {
                if (activeInputField_ == focusedButton_) {
                    queueInputFieldEvent(focusedButton_, submitEntity->inputField->text, true);
                    activeInputField_ = {};
                    activeInputFieldEntity = nullptr;
                } else {
                    activeInputField_ = focusedButton_;
                    activeInputFieldEntity = world_.Find(activeInputField_);
                    openDropdown_ = {};
                    openDropdownEntity = nullptr;
                }
            }
        } else if (submitEntity->dropdown && submitEntity->dropdown->enabled &&
                   submitEntity->dropdown->interactable) {
            if (openDropdown_ == focusedButton_) {
                DropdownComponent& dropdown = *world_.Find(openDropdown_)->dropdown;
                if (dropdown.value != dropdownHighlightedIndex_) {
                    dropdown.value = dropdownHighlightedIndex_;
                    pendingDropdownValueChanges_.push_back({openDropdown_, dropdown.value});
                }
                openDropdown_ = {};
                openDropdownEntity = nullptr;
            } else {
                openDropdown_ = focusedButton_;
                openDropdownEntity = world_.Find(openDropdown_);
                dropdownHighlightedIndex_ = submitEntity->dropdown->value;
            }
        } else {
            pendingButtonClicks_.push_back(focusedButton_);
        }
    }
    WorldEntity* keyboardSlider = world_.Find(focusedButton_);
    if (canNavigateUi && !navigatedUi && !activeInputField_.IsValid() &&
        keyboardSlider != nullptr && keyboardSlider->slider && keyboardSlider->slider->enabled &&
        keyboardSlider->slider->interactable &&
        GetUiGroupState(world_, *keyboardSlider).interactable) {
        SliderComponent& slider = *keyboardSlider->slider;
        const bool horizontal = slider.direction == SliderDirection::LeftToRight ||
                                slider.direction == SliderDirection::RightToLeft;
        const bool negative =
            horizontal ? (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
                          (runtimeInput != nullptr &&
                           runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_LEFT)))
                       : (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                          (runtimeInput != nullptr &&
                           runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN)));
        const bool positive =
            horizontal ? (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                          (runtimeInput != nullptr &&
                           runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_RIGHT)))
                       : (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                          (runtimeInput != nullptr &&
                           runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_UP)));
        if (negative || positive) {
            const bool reverse = slider.direction == SliderDirection::RightToLeft ||
                                 slider.direction == SliderDirection::TopToBottom;
            const float step =
                slider.wholeNumbers ? 1.0f : (slider.maxValue - slider.minValue) * 0.1f;
            const float visualDirection = positive ? 1.0f : -1.0f;
            setSliderValue(*keyboardSlider,
                           slider.value + visualDirection * (reverse ? -step : step));
        }
    }
    if (canNavigateUi &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         (runtimeInput != nullptr && runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_B)))) {
        if (activeInputField_.IsValid()) {
            activeInputField_ = {};
            activeInputFieldEntity = nullptr;
        } else if (openDropdown_.IsValid()) {
            openDropdown_ = {};
        } else {
            focusedButton_ = {};
        }
    }

    DrawGameUiVisuals(width, height, hoveredButton, submitHeld);
    openDropdownEntity = world_.Find(openDropdown_);
    if (openDropdownEntity != nullptr && openDropdownEntity->dropdown && openDropdownEntity->text &&
        calculateImageRect(*openDropdownEntity, dropdownLeft, dropdownTop, dropdownRight,
                           dropdownBottom)) {
        const DropdownComponent& dropdown = *openDropdownEntity->dropdown;
        const TextComponent& text = *openDropdownEntity->text;
        const UiGroupState groupState = GetUiGroupState(world_, *openDropdownEntity);
        float popupScale = 1.0f;
        DirectX::XMFLOAT2 popupOrigin{};
        DirectX::XMFLOAT2 popupResolution{};
        resolveCanvasLayout(*openDropdownEntity, popupScale, popupOrigin, popupResolution);
        TextStyle popupStyle{};
        if (const auto loadedFont = loadedFonts_.find(text.fontPath);
            loadedFont != loadedFonts_.end()) {
            popupStyle.font = loadedFont->second;
        }
        popupStyle.pixelSize = text.fontSize * popupScale;
        popupStyle.horizontalAlignment = TextHorizontalAlignment::Center;
        popupStyle.color = text.color;
        popupStyle.color.w *= groupState.alpha;
        const float rowHeight = dropdown.itemHeight * popupScale;
        for (size_t optionIndex = 0; optionIndex < dropdown.options.size(); ++optionIndex) {
            Sprite item{};
            item.position = {
                dropdownLeft,
                dropdownBottom + rowHeight * static_cast<float>(optionIndex),
            };
            item.size = {
                dropdownRight - dropdownLeft,
                rowHeight,
            };
            item.color = static_cast<int32_t>(optionIndex) == dropdownHighlightedIndex_
                             ? dropdown.highlightedColor
                             : dropdown.itemColor;
            item.color.w *= groupState.alpha;
            item.textureId = kInvalidResourceId;
            spriteRenderer.Draw(item);

            const TextLayoutMetrics metrics =
                textRenderer.MeasureText(dropdown.options[optionIndex], popupStyle);
            const DirectX::XMFLOAT2 labelPosition{
                (dropdownLeft + dropdownRight - metrics.size.x) * 0.5f,
                item.position.y + (rowHeight - metrics.size.y) * 0.5f,
            };
            textRenderer.DrawText(dropdown.options[optionIndex], labelPosition, popupStyle);
        }
    }
    spriteRenderer.PostDraw();

    if (ctx_->systems.winApp != nullptr) {
        spriteRenderer.UpdateProjection(ctx_->systems.winApp->GetWidth(),
                                        ctx_->systems.winApp->GetHeight());
    }

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

void EditorScene::HandleGameUiEditing(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (playModeState_ != PlayModeState::Edit || ctx_ == nullptr || imageMax.x <= imageMin.x ||
        imageMax.y <= imageMin.y) {
        gameUiDragEntity_ = {};
        gameUiResizeEntity_ = {};
        gameUiResizeHandle_ = UiResizeHandle::None;
        return;
    }

    TextRenderer* textRenderer = ctx_->rendering.text;
    const auto resolveCanvasLayout = [&](const WorldEntity& entity, float& scale,
                                         DirectX::XMFLOAT2& origin,
                                         DirectX::XMFLOAT2& referenceResolution) {
        const CanvasComponent* canvas = FindEnabledCanvas(world_, entity);
        if (canvas == nullptr) {
            return false;
        }
        const float width = imageMax.x - imageMin.x;
        const float height = imageMax.y - imageMin.y;
        CalculateCanvasLayout(*canvas, width, height, imageMin.x, imageMin.y, scale, origin,
                              referenceResolution);
        return true;
    };

    const auto calculateRect = [&](const WorldEntity& entity, ImVec2& minimum, ImVec2& maximum) {
        if (!world_.IsActiveInHierarchy(entity.id)) {
            return false;
        }
        float scale = 1.0f;
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT2 referenceResolution{};
        if (!resolveCanvasLayout(entity, scale, origin, referenceResolution)) {
            return false;
        }
        bool hasRect = false;
        const auto includeRect = [&](const ImVec2& rectMin, const ImVec2& rectMax) {
            if (!hasRect) {
                minimum = rectMin;
                maximum = rectMax;
                hasRect = true;
                return;
            }
            minimum.x = (std::min)(minimum.x, rectMin.x);
            minimum.y = (std::min)(minimum.y, rectMin.y);
            maximum.x = (std::max)(maximum.x, rectMax.x);
            maximum.y = (std::max)(maximum.y, rectMax.y);
        };
        if (entity.image && entity.image->enabled) {
            const ImageComponent& image = *entity.image;
            const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
            const ImVec2 rectMin{
                origin.x + (referenceResolution.x * anchor.x + image.position.x -
                            image.size.x * image.pivot.x) *
                               scale,
                origin.y + (referenceResolution.y * anchor.y + image.position.y -
                            image.size.y * image.pivot.y) *
                               scale,
            };
            includeRect(rectMin,
                        {rectMin.x + image.size.x * scale, rectMin.y + image.size.y * scale});
        }
        if (entity.text && entity.text->enabled && textRenderer != nullptr &&
            textRenderer->IsReady()) {
            const TextComponent& text = *entity.text;
            TextStyle style{};
            if (const auto loadedFont = loadedFonts_.find(text.fontPath);
                loadedFont != loadedFonts_.end()) {
                style.font = loadedFont->second;
            }
            style.pixelSize = text.fontSize * scale;
            style.lineSpacing = text.lineSpacing * scale;
            style.wrapWidth = text.wrapWidth * scale;
            const TextLayoutMetrics metrics = textRenderer->MeasureText(text.text, style);
            const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(text.anchor).factor;
            ImVec2 rectMin{
                origin.x + (referenceResolution.x * anchor.x + text.position.x) * scale,
                origin.y + (referenceResolution.y * anchor.y + text.position.y) * scale -
                    metrics.size.y * anchor.y,
            };
            if (text.alignment == TextAlignment::Center) {
                rectMin.x -= metrics.size.x * 0.5f;
            } else if (text.alignment == TextAlignment::Right) {
                rectMin.x -= metrics.size.x;
            }
            includeRect(rectMin, {rectMin.x + metrics.size.x, rectMin.y + metrics.size.y});
        }
        return hasRect;
    };

    const auto calculateImageRect = [&](const WorldEntity& entity, ImVec2& minimum, ImVec2& maximum,
                                        float* canvasScale = nullptr) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.image || !entity.image->enabled) {
            return false;
        }
        float scale = 1.0f;
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT2 referenceResolution{};
        if (!resolveCanvasLayout(entity, scale, origin, referenceResolution)) {
            return false;
        }
        const ImageComponent& image = *entity.image;
        const DirectX::XMFLOAT2 anchor = GetUiAnchorChoice(image.anchor).factor;
        minimum = {
            origin.x + (referenceResolution.x * anchor.x + image.position.x -
                        image.size.x * image.pivot.x) *
                           scale,
            origin.y + (referenceResolution.y * anchor.y + image.position.y -
                        image.size.y * image.pivot.y) *
                           scale,
        };
        maximum = {
            minimum.x + image.size.x * scale,
            minimum.y + image.size.y * scale,
        };
        if (canvasScale != nullptr) {
            *canvasScale = scale;
        }
        return true;
    };

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool imageHovered = ImGui::IsItemHovered();
    EntityId hovered{};
    if (imageHovered) {
        for (const OrderedUiEntity& entry : GetOrderedUiEntities(world_)) {
            const WorldEntity& entity = *entry.entity;
            ImVec2 minimum{};
            ImVec2 maximum{};
            if (calculateRect(entity, minimum, maximum) && mouse.x >= minimum.x &&
                mouse.x <= maximum.x && mouse.y >= minimum.y && mouse.y <= maximum.y) {
                hovered = entity.id;
            }
        }
    }

    const WorldEntity* selectedBeforeInput = world_.Find(selection_);
    ImVec2 selectedImageMin{};
    ImVec2 selectedImageMax{};
    const bool selectedHasImage =
        selectedBeforeInput != nullptr &&
        calculateImageRect(*selectedBeforeInput, selectedImageMin, selectedImageMax);
    constexpr float kResizeHandleRadius = 6.0f;
    constexpr float kResizeHitRadius = 9.0f;
    UiResizeHandle hoveredResizeHandle = UiResizeHandle::None;
    if (imageHovered && selectedHasImage) {
        const auto hitHandle = [&](const ImVec2& position) {
            return std::abs(mouse.x - position.x) <= kResizeHitRadius &&
                   std::abs(mouse.y - position.y) <= kResizeHitRadius;
        };
        if (hitHandle(selectedImageMin)) {
            hoveredResizeHandle = UiResizeHandle::TopLeft;
        } else if (hitHandle({selectedImageMax.x, selectedImageMin.y})) {
            hoveredResizeHandle = UiResizeHandle::TopRight;
        } else if (hitHandle({selectedImageMin.x, selectedImageMax.y})) {
            hoveredResizeHandle = UiResizeHandle::BottomLeft;
        } else if (hitHandle(selectedImageMax)) {
            hoveredResizeHandle = UiResizeHandle::BottomRight;
        }
    }

    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredResizeHandle != UiResizeHandle::None) {
            gameUiResizeEntity_ = selection_;
            gameUiResizeHandle_ = hoveredResizeHandle;
            gameUiDragEntity_ = {};
            BeginHistoryEdit("Resize UI Image");
        } else if (hovered.IsValid()) {
            SelectHierarchyEntity(hovered, false, false);
            gameUiDragEntity_ = hovered;
            gameUiResizeEntity_ = {};
            gameUiResizeHandle_ = UiResizeHandle::None;
            BeginHistoryEdit("Move UI Element");
        } else {
            ClearHierarchySelection();
            gameUiDragEntity_ = {};
            gameUiResizeEntity_ = {};
            gameUiResizeHandle_ = UiResizeHandle::None;
        }
    }

    if (gameUiResizeEntity_.IsValid() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiResizeEntity_);
        if (entity != nullptr && entity->image &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            ImVec2 rectMin{};
            ImVec2 rectMax{};
            if (calculateImageRect(*entity, rectMin, rectMax, &scale) && scale > 0.0f) {
                const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
                ImageComponent& image = *entity->image;
                const DirectX::XMFLOAT2 previousSize = image.size;
                const float horizontalDelta = mouseDelta.x / scale;
                const float verticalDelta = mouseDelta.y / scale;
                const bool resizesLeft = gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                                         gameUiResizeHandle_ == UiResizeHandle::BottomLeft;
                const bool resizesTop = gameUiResizeHandle_ == UiResizeHandle::TopLeft ||
                                        gameUiResizeHandle_ == UiResizeHandle::TopRight;
                image.size.x =
                    std::clamp(image.size.x + (resizesLeft ? -horizontalDelta : horizontalDelta),
                               1.0f, 1000000.0f);
                image.size.y = std::clamp(
                    image.size.y + (resizesTop ? -verticalDelta : verticalDelta), 1.0f, 1000000.0f);
                const DirectX::XMFLOAT2 sizeDelta{
                    image.size.x - previousSize.x,
                    image.size.y - previousSize.y,
                };
                const DirectX::XMFLOAT2 minimumDelta{
                    resizesLeft ? -sizeDelta.x : 0.0f,
                    resizesTop ? -sizeDelta.y : 0.0f,
                };
                const DirectX::XMFLOAT2 positionDelta{
                    minimumDelta.x + sizeDelta.x * image.pivot.x,
                    minimumDelta.y + sizeDelta.y * image.pivot.y,
                };
                image.position.x =
                    std::clamp(image.position.x + positionDelta.x, -1000000.0f, 1000000.0f);
                image.position.y =
                    std::clamp(image.position.y + positionDelta.y, -1000000.0f, 1000000.0f);
                if (entity->text) {
                    const DirectX::XMFLOAT2 centerDelta{
                        minimumDelta.x + sizeDelta.x * 0.5f,
                        minimumDelta.y + sizeDelta.y * 0.5f,
                    };
                    entity->text->position.x = std::clamp(entity->text->position.x + centerDelta.x,
                                                          -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(entity->text->position.y + centerDelta.y,
                                                          -1000000.0f, 1000000.0f);
                }
                RefreshDirty();
                status_ = "Resized UI Image in the Game View.";
            }
        }
    } else if (gameUiResizeEntity_.IsValid()) {
        CommitHistoryEdit();
        gameUiResizeEntity_ = {};
        gameUiResizeHandle_ = UiResizeHandle::None;
    } else if (gameUiDragEntity_.IsValid() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WorldEntity* entity = world_.Find(gameUiDragEntity_);
        if (entity != nullptr && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            float scale = 1.0f;
            DirectX::XMFLOAT2 origin{};
            DirectX::XMFLOAT2 referenceResolution{};
            if (resolveCanvasLayout(*entity, scale, origin, referenceResolution) && scale > 0.0f) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                if (entity->image) {
                    entity->image->position.x = std::clamp(
                        entity->image->position.x + delta.x / scale, -1000000.0f, 1000000.0f);
                    entity->image->position.y = std::clamp(
                        entity->image->position.y + delta.y / scale, -1000000.0f, 1000000.0f);
                }
                if (entity->text) {
                    entity->text->position.x = std::clamp(
                        entity->text->position.x + delta.x / scale, -1000000.0f, 1000000.0f);
                    entity->text->position.y = std::clamp(
                        entity->text->position.y + delta.y / scale, -1000000.0f, 1000000.0f);
                }
                RefreshDirty();
                status_ = "Moved UI element in the Game View.";
            }
        }
    } else if (gameUiDragEntity_.IsValid()) {
        CommitHistoryEdit();
        gameUiDragEntity_ = {};
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!gameUiDragEntity_.IsValid() && !gameUiResizeEntity_.IsValid() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
        !io.KeyCtrl && !io.KeyAlt) {
        DirectX::XMFLOAT2 nudge{};
        const float distance = io.KeyShift ? 10.0f : 1.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            nudge.x -= distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            nudge.x += distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            nudge.y -= distance;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            nudge.y += distance;
        }
        WorldEntity* entity = world_.Find(selection_);
        ImVec2 selectedRectMin{};
        ImVec2 selectedRectMax{};
        if ((nudge.x != 0.0f || nudge.y != 0.0f) && entity != nullptr &&
            calculateRect(*entity, selectedRectMin, selectedRectMax)) {
            const std::string before = WorldSerializer::Serialize(world_);
            const auto movePosition = [&nudge](DirectX::XMFLOAT2& position) {
                position.x = std::clamp(position.x + nudge.x, -1000000.0f, 1000000.0f);
                position.y = std::clamp(position.y + nudge.y, -1000000.0f, 1000000.0f);
            };
            if (entity->image) {
                movePosition(entity->image->position);
            }
            if (entity->text) {
                movePosition(entity->text->position);
            }
            RecordImmediateEdit("Nudge UI Element", before, selection_);
            status_ =
                io.KeyShift ? "Moved UI element by 10 pixels." : "Moved UI element by 1 pixel.";
        }
    }

    const WorldEntity* selected = world_.Find(selection_);
    ImVec2 selectedMin{};
    ImVec2 selectedMax{};
    if (selected != nullptr && calculateRect(*selected, selectedMin, selectedMax)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(imageMin, imageMax, true);
        drawList->AddRect(selectedMin, selectedMax, IM_COL32(255, 190, 60, 255), 0.0f, 0, 2.0f);
        ImVec2 imageRectMin{};
        ImVec2 imageRectMax{};
        if (calculateImageRect(*selected, imageRectMin, imageRectMax)) {
            const std::array<ImVec2, 4> handles{
                imageRectMin,
                ImVec2{imageRectMax.x, imageRectMin.y},
                ImVec2{imageRectMin.x, imageRectMax.y},
                imageRectMax,
            };
            for (const ImVec2& handle : handles) {
                drawList->AddRectFilled(
                    {handle.x - kResizeHandleRadius, handle.y - kResizeHandleRadius},
                    {handle.x + kResizeHandleRadius, handle.y + kResizeHandleRadius},
                    IM_COL32(255, 190, 60, 255));
            }
        }
        drawList->PopClipRect();
    }
    const UiResizeHandle cursorHandle =
        gameUiResizeEntity_.IsValid() ? gameUiResizeHandle_ : hoveredResizeHandle;
    if (cursorHandle == UiResizeHandle::TopLeft || cursorHandle == UiResizeHandle::BottomRight) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if (cursorHandle == UiResizeHandle::TopRight ||
               cursorHandle == UiResizeHandle::BottomLeft) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if (gameUiDragEntity_.IsValid()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (hovered.IsValid()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}
