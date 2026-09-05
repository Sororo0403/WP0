#include "EditorScene.h"

#include "core/WinApp.h"
#include "font/TextRenderer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "input/Input.h"
#include "internal/EditorSceneGameUiUtils.h"
#include "sprite/SpriteRenderer.h"

#include <algorithm>
#include <cmath>
#include <ranges>

using namespace EditorSceneGameUiUtils;

namespace {
struct GameUiControlState {
    bool enabled = false;
    bool slider = false;
    bool interactable = false;
    bool navigationEnabled = false;
    bool blocksRaycasts = false;
};

bool IsEnabledGameUiButton(const WorldEntity& entity) {
    return entity.button && entity.button->enabled &&
           (!entity.toggle || entity.toggle->enabled) &&
           (!entity.dropdown || entity.dropdown->enabled) &&
           (!entity.inputField || entity.inputField->enabled);
}

bool IsGameUiControlInteractable(const WorldEntity& entity, bool isButton, bool isSlider,
                                 const UiGroupState& groupState) {
    if (!groupState.interactable) {
        return false;
    }
    if (isSlider) {
        return entity.slider->interactable;
    }
    return isButton && entity.button->interactable &&
           (!entity.dropdown || entity.dropdown->interactable) &&
           (!entity.inputField || entity.inputField->interactable);
}

GameUiControlState GetGameUiControlState(const World& world, const WorldEntity& entity) {
    const bool isButton = IsEnabledGameUiButton(entity);
    const bool isSlider = entity.slider && entity.slider->enabled;
    const UiGroupState groupState = GetUiGroupState(world, entity);
    GameUiControlState state{};
    state.enabled = (isButton || isSlider) && entity.image && entity.image->enabled;
    state.slider = isSlider;
    state.interactable =
        IsGameUiControlInteractable(entity, isButton, isSlider, groupState);
    state.navigationEnabled = isSlider ||
                              (isButton && entity.button->navigation !=
                                               ButtonNavigationMode::None);
    state.blocksRaycasts = groupState.blocksRaycasts;
    return state;
}

bool IsGamepadButtonTriggered(const Input* input, uint16_t button) {
    return input != nullptr && input->IsGamepadButtonTrigger(button);
}

int GetGameUiSliderInputDirection(const SliderComponent& slider, const Input* input) {
    const bool horizontal = slider.direction == SliderDirection::LeftToRight ||
                            slider.direction == SliderDirection::RightToLeft;
    const bool negative =
        horizontal ? ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
                         IsGamepadButtonTriggered(input, XINPUT_GAMEPAD_DPAD_LEFT)
                   : ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                         IsGamepadButtonTriggered(input, XINPUT_GAMEPAD_DPAD_DOWN);
    if (negative) {
        return -1;
    }
    const bool positive =
        horizontal ? ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                         IsGamepadButtonTriggered(input, XINPUT_GAMEPAD_DPAD_RIGHT)
                   : ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                         IsGamepadButtonTriggered(input, XINPUT_GAMEPAD_DPAD_UP);
    return positive ? 1 : 0;
}

struct GameUiSubmitState {
    bool gamepad = false;
    bool held = false;
    bool pressed = false;
};

GameUiSubmitState GetGameUiSubmitState(bool canNavigateUi, const Input* input) {
    GameUiSubmitState state{};
    state.gamepad = canNavigateUi && IsGamepadButtonTriggered(input, XINPUT_GAMEPAD_A);
    state.held = canNavigateUi &&
                 (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_Space) ||
                  (input != nullptr && input->IsGamepadButtonPress(XINPUT_GAMEPAD_A)));
    state.pressed = canNavigateUi &&
                    (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Space, false) || state.gamepad);
    return state;
}

bool IsInteractableGameUiInputField(const WorldEntity& entity) {
    return entity.inputField && entity.inputField->enabled && entity.inputField->interactable;
}

bool IsInteractableGameUiDropdown(const WorldEntity& entity) {
    return entity.dropdown && entity.dropdown->enabled && entity.dropdown->interactable;
}
} // namespace

bool EditorScene::PrepareGameUiFrame(int width, int height) {
    if (ctx_ == nullptr || ctx_->rendering.text == nullptr ||
        ctx_->rendering.spriteRenderer == nullptr || width <= 0 || height <= 0 ||
        !ctx_->rendering.text->IsReady() || !ctx_->rendering.spriteRenderer->IsReady()) {
        return false;
    }
    for (auto transition = buttonColorTransitions_.begin();
         transition != buttonColorTransitions_.end();) {
        const WorldEntity* entity = world_.Find(transition->first);
        if (entity == nullptr || !entity->button || !entity->image) {
            transition = buttonColorTransitions_.erase(transition);
        } else {
            ++transition;
        }
    }
    return true;
}

bool EditorScene::CanPointAtGameUi(const ImVec2& imageScreenMin, const ImVec2& mouse,
                                   bool uiEventsEnabled) const {
    return playModeState_ == PlayModeState::Playing && uiEventsEnabled && !gameInputCaptured_ &&
           ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
           requestedGameWidth_ > 0 && requestedGameHeight_ > 0 && mouse.x >= imageScreenMin.x &&
           mouse.y >= imageScreenMin.y && mouse.x <= imageScreenMin.x + requestedGameWidth_ &&
           mouse.y <= imageScreenMin.y + requestedGameHeight_;
}

DirectX::XMFLOAT2 EditorScene::CalculateGameUiPointer(const ImVec2& imageScreenMin,
                                                      const ImVec2& mouse, int width,
                                                      int height) const {
    return {
        (mouse.x - imageScreenMin.x) * static_cast<float>(width) /
            static_cast<float>((std::max)(1, requestedGameWidth_)),
        (mouse.y - imageScreenMin.y) * static_cast<float>(height) /
            static_cast<float>((std::max)(1, requestedGameHeight_)),
    };
}

bool EditorScene::CanNavigateGameUi(const EventSystemComponent* eventSystem,
                                    bool uiEventsEnabled) const {
    return playModeState_ == PlayModeState::Playing && uiEventsEnabled && !gameInputCaptured_ &&
           (eventSystem == nullptr || eventSystem->sendNavigationEvents) &&
           ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           !ImGui::GetIO().WantTextInput;
}

bool EditorScene::IsGameUiButtonInteractable(EntityId entity) const {
    const WorldEntity* hovered = world_.Find(entity);
    return hovered != nullptr && hovered->button && hovered->button->enabled &&
           hovered->button->interactable &&
           (!hovered->dropdown ||
            (hovered->dropdown->enabled && hovered->dropdown->interactable)) &&
           (!hovered->inputField ||
            (hovered->inputField->enabled && hovered->inputField->interactable)) &&
           GetUiGroupState(world_, *hovered).interactable;
}

bool EditorScene::IsGameUiSliderInteractable(EntityId entity) const {
    const WorldEntity* hovered = world_.Find(entity);
    return hovered != nullptr && hovered->slider && hovered->slider->enabled &&
           hovered->slider->interactable && GetUiGroupState(world_, *hovered).interactable;
}

bool EditorScene::TryCalculateRuntimeGameUiCanvasLayout(
    const WorldEntity& entity, int width, int height, float& scale, DirectX::XMFLOAT2& origin,
    DirectX::XMFLOAT2& referenceResolution) const {
    return TryCalculateGameUiCanvasLayout(entity, ImVec2{}, ImVec2{static_cast<float>(width),
                                                                   static_cast<float>(height)},
                                          scale, origin, referenceResolution);
}

bool EditorScene::TryCalculateRuntimeGameUiImageRect(const WorldEntity& entity, int width,
                                                     int height, float& left, float& top,
                                                     float& right, float& bottom) const {
    ImVec2 minimum{};
    ImVec2 maximum{};
    if (!TryCalculateGameUiImageRect(
            entity, ImVec2{}, ImVec2{static_cast<float>(width), static_cast<float>(height)}, minimum,
            maximum)) {
        return false;
    }
    left = minimum.x;
    top = minimum.y;
    right = maximum.x;
    bottom = maximum.y;
    return true;
}

void EditorScene::SetGameUiSliderValue(WorldEntity& entity, float requestedValue) {
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
}

void EditorScene::SetGameUiSliderValueFromPointer(WorldEntity& entity, int width, int height,
                                                  const DirectX::XMFLOAT2& pointer) {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!TryCalculateRuntimeGameUiImageRect(entity, width, height, left, top, right, bottom)) {
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
    SetGameUiSliderValue(entity, std::lerp(slider.minValue, slider.maxValue, normalized));
}

void EditorScene::QueueGameUiInputFieldEvent(EntityId entity, const std::string& text,
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
}

EntityId EditorScene::CollectGameUiControls(
    bool canPoint, const DirectX::XMFLOAT2& pointer, int width, int height,
    std::vector<EntityId>& selectableButtons,
    std::unordered_map<EntityId, DirectX::XMFLOAT2, EntityIdHash>& selectableCenters) const {
    EntityId hoveredButton{};
    for (const OrderedUiEntity& entry : GetOrderedUiEntities(world_)) {
        const WorldEntity& entity = *entry.entity;
        const GameUiControlState state = GetGameUiControlState(world_, entity);
        if (!state.enabled) {
            continue;
        }
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (state.interactable && state.navigationEnabled) {
            selectableButtons.push_back(entity.id);
            if (TryCalculateRuntimeGameUiImageRect(entity, width, height, left, top, right,
                                                   bottom)) {
                selectableCenters.emplace(
                    entity.id, DirectX::XMFLOAT2{(left + right) * 0.5f, (top + bottom) * 0.5f});
            }
        }
        if (canPoint && state.blocksRaycasts &&
            TryCalculateRuntimeGameUiImageRect(entity, width, height, left, top, right, bottom) &&
            pointer.x >= left && pointer.x <= right && pointer.y >= top && pointer.y <= bottom) {
            hoveredButton = entity.id;
        }
    }
    return hoveredButton;
}

WorldEntity* EditorScene::FindOpenGameUiDropdown() {
    WorldEntity* entity = world_.Find(openDropdown_);
    if (entity == nullptr || !entity->dropdown || !entity->dropdown->enabled ||
        !entity->dropdown->interactable || !entity->button || !entity->button->enabled ||
        !entity->button->interactable || !entity->image ||
        !GetUiGroupState(world_, *entity).interactable) {
        openDropdown_ = {};
        return nullptr;
    }
    return entity;
}

int32_t EditorScene::UpdateGameUiDropdownHover(WorldEntity* openDropdownEntity, bool canPoint,
                                               const DirectX::XMFLOAT2& pointer, int width,
                                               int height, EntityId& hoveredButton) {
    if (openDropdownEntity == nullptr) {
        return -1;
    }
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    if (!TryCalculateRuntimeGameUiImageRect(*openDropdownEntity, width, height, left, top, right,
                                            bottom)) {
        return -1;
    }
    float scale = 1.0f;
    DirectX::XMFLOAT2 origin{};
    DirectX::XMFLOAT2 resolution{};
    TryCalculateRuntimeGameUiCanvasLayout(*openDropdownEntity, width, height, scale, origin,
                                          resolution);
    const DropdownComponent& dropdown = *openDropdownEntity->dropdown;
    const float rowHeight = dropdown.itemHeight * scale;
    if (!canPoint || pointer.x < left || pointer.x > right || pointer.y < bottom ||
        pointer.y >= bottom + rowHeight * static_cast<float>(dropdown.options.size())) {
        return -1;
    }
    const int32_t hoveredOption = static_cast<int32_t>((pointer.y - bottom) / rowHeight);
    dropdownHighlightedIndex_ = hoveredOption;
    hoveredButton = {};
    return hoveredOption;
}

WorldEntity* EditorScene::FindActiveGameUiInputField() {
    WorldEntity* entity = world_.Find(activeInputField_);
    if (entity == nullptr || !entity->inputField || !entity->inputField->enabled ||
        !entity->inputField->interactable || !entity->button || !entity->button->enabled ||
        !entity->button->interactable || !GetUiGroupState(world_, *entity).interactable) {
        activeInputField_ = {};
        return nullptr;
    }
    return entity;
}

void EditorScene::InitializeGameUiSelection(const std::vector<EntityId>& selectableButtons,
                                            const EventSystemComponent* eventSystem,
                                            bool uiEventsEnabled) {
    if (focusedButton_.IsValid() &&
        std::ranges::find(selectableButtons, focusedButton_) == selectableButtons.end()) {
        focusedButton_ = {};
    }
    if (playModeState_ != PlayModeState::Playing || !uiEventsEnabled ||
        runtimeInitialUiSelectionApplied_) {
        return;
    }
    runtimeInitialUiSelectionApplied_ = true;
    if (eventSystem != nullptr &&
        std::ranges::find(selectableButtons, eventSystem->firstSelected) !=
            selectableButtons.end()) {
        focusedButton_ = eventSystem->firstSelected;
    }
}

void EditorScene::UpdateActiveGameUiInputField(WorldEntity* activeInputFieldEntity,
                                               bool canNavigateUi) {
    if (!canNavigateUi || activeInputFieldEntity == nullptr) {
        return;
    }
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
        QueueGameUiInputFieldEvent(activeInputField_, inputField.text, false);
    }
}

bool EditorScene::NavigateGameUiTab(const std::vector<EntityId>& selectableButtons,
                                    bool canNavigateUi) {
    if (!canNavigateUi || activeInputField_.IsValid() ||
        !ImGui::IsKeyPressed(ImGuiKey_Tab, false) || selectableButtons.empty()) {
        return false;
    }
    const bool selectPrevious = ImGui::GetIO().KeyShift;
    const auto focused = std::ranges::find(selectableButtons, focusedButton_);
    if (focused == selectableButtons.end()) {
        focusedButton_ = selectPrevious ? selectableButtons.back() : selectableButtons.front();
    } else {
        const size_t index = static_cast<size_t>(focused - selectableButtons.begin());
        focusedButton_ =
            selectPrevious
                ? selectableButtons[(index + selectableButtons.size() - 1u) %
                                    selectableButtons.size()]
                : selectableButtons[(index + 1u) % selectableButtons.size()];
    }
    return true;
}

bool EditorScene::NavigateOpenGameUiDropdown(WorldEntity* openDropdownEntity,
                                             bool canNavigateUi) {
    if (!canNavigateUi || openDropdownEntity == nullptr) {
        return false;
    }
    const int32_t optionCount =
        static_cast<int32_t>(openDropdownEntity->dropdown->options.size());
    if (optionCount <= 0) {
        return false;
    }
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    const bool selectPrevious = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                                (runtimeInput != nullptr &&
                                 runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_UP));
    const bool selectNext = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                            (runtimeInput != nullptr &&
                             runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_DPAD_DOWN));
    if (selectPrevious) {
        dropdownHighlightedIndex_ = (dropdownHighlightedIndex_ + optionCount - 1) % optionCount;
        return true;
    }
    if (selectNext) {
        dropdownHighlightedIndex_ = (dropdownHighlightedIndex_ + 1) % optionCount;
        return true;
    }
    return false;
}

void EditorScene::HandleGameUiPointerInteractions(EntityId hoveredButton,
                                                  int32_t hoveredDropdownOption,
                                                  bool hoveredButtonInteractable,
                                                  bool hoveredSliderInteractable,
                                                  const DirectX::XMFLOAT2& pointer, int width,
                                                  int height) {
    HandleGameUiPointerPress(hoveredButton, hoveredDropdownOption, hoveredButtonInteractable,
                             hoveredSliderInteractable, pointer, width, height);
    UpdateActiveGameUiSlider(pointer, width, height);
    HandleGameUiPointerRelease(hoveredButton, hoveredButtonInteractable);
}

void EditorScene::HandleGameUiPointerPress(EntityId hoveredButton,
                                           int32_t hoveredDropdownOption,
                                           bool hoveredButtonInteractable,
                                           bool hoveredSliderInteractable,
                                           const DirectX::XMFLOAT2& pointer, int width,
                                           int height) {
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }
    WorldEntity* openDropdownEntity = world_.Find(openDropdown_);
    if (activeInputField_.IsValid() && hoveredButton != activeInputField_) {
        activeInputField_ = {};
    }
    if (openDropdownEntity != nullptr && hoveredDropdownOption >= 0) {
        DropdownComponent& dropdown = *openDropdownEntity->dropdown;
        if (dropdown.value != hoveredDropdownOption) {
            dropdown.value = hoveredDropdownOption;
            pendingDropdownValueChanges_.push_back({openDropdown_, dropdown.value});
        }
        focusedButton_ = openDropdown_;
        openDropdown_ = {};
        pressedButton_ = {};
        activeSlider_ = {};
        return;
    }
    if (hoveredSliderInteractable) {
        activeSlider_ = hoveredButton;
        pressedButton_ = {};
        focusedButton_ = hoveredButton;
        if (WorldEntity* slider = world_.Find(activeSlider_); slider != nullptr) {
            SetGameUiSliderValueFromPointer(*slider, width, height, pointer);
        }
        return;
    }
    activeSlider_ = {};
    if (openDropdownEntity != nullptr && hoveredButton != openDropdown_) {
        openDropdown_ = {};
    }
    pressedButton_ = hoveredButtonInteractable ? hoveredButton : EntityId{};
    focusedButton_ = hoveredButtonInteractable ? hoveredButton : EntityId{};
}

void EditorScene::UpdateActiveGameUiSlider(const DirectX::XMFLOAT2& pointer, int width,
                                           int height) {
    if (!activeSlider_.IsValid() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return;
    }
    WorldEntity* sliderEntity = world_.Find(activeSlider_);
    if (sliderEntity != nullptr && sliderEntity->slider && sliderEntity->slider->enabled &&
        sliderEntity->slider->interactable &&
        GetUiGroupState(world_, *sliderEntity).interactable) {
        SetGameUiSliderValueFromPointer(*sliderEntity, width, height, pointer);
    } else {
        activeSlider_ = {};
    }
}

void EditorScene::ActivatePressedGameUiControl(WorldEntity& clicked) {
    if (clicked.dropdown && clicked.dropdown->enabled && clicked.dropdown->interactable) {
        if (openDropdown_ == clicked.id) {
            openDropdown_ = {};
        } else {
            openDropdown_ = clicked.id;
            dropdownHighlightedIndex_ = clicked.dropdown->value;
        }
        return;
    }
    if (clicked.inputField && clicked.inputField->enabled && clicked.inputField->interactable) {
        activeInputField_ = clicked.id;
        openDropdown_ = {};
        return;
    }
    pendingButtonClicks_.push_back(clicked.id);
}

void EditorScene::HandleGameUiPointerRelease(EntityId hoveredButton,
                                             bool hoveredButtonInteractable) {
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        return;
    }
    if (pressedButton_.IsValid() && pressedButton_ == hoveredButton &&
        hoveredButtonInteractable) {
        WorldEntity* clicked = world_.Find(pressedButton_);
        if (clicked != nullptr) {
            ActivatePressedGameUiControl(*clicked);
        }
    }
    pressedButton_ = {};
    activeSlider_ = {};
}

bool EditorScene::HandleGameUiSubmit(bool canNavigateUi) {
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    const GameUiSubmitState submit = GetGameUiSubmitState(canNavigateUi, runtimeInput);
    WorldEntity* submitEntity = world_.Find(focusedButton_);
    if (!submit.pressed || submitEntity == nullptr || !submitEntity->button ||
        !submitEntity->button->enabled) {
        return submit.held;
    }
    const bool inputFieldSubmit = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || submit.gamepad;
    if (IsInteractableGameUiInputField(*submitEntity)) {
        HandleFocusedGameUiInputFieldSubmit(*submitEntity, inputFieldSubmit);
    } else if (IsInteractableGameUiDropdown(*submitEntity)) {
        HandleFocusedGameUiDropdownSubmit(*submitEntity);
    } else {
        pendingButtonClicks_.push_back(focusedButton_);
    }
    return submit.held;
}

void EditorScene::HandleFocusedGameUiInputFieldSubmit(WorldEntity& entity, bool submit) {
    if (!submit) {
        return;
    }
    if (activeInputField_ == entity.id) {
        QueueGameUiInputFieldEvent(entity.id, entity.inputField->text, true);
        activeInputField_ = {};
    } else {
        activeInputField_ = entity.id;
        openDropdown_ = {};
    }
}

void EditorScene::HandleFocusedGameUiDropdownSubmit(WorldEntity& entity) {
    DropdownComponent& dropdown = *entity.dropdown;
    if (openDropdown_ != entity.id) {
        openDropdown_ = entity.id;
        dropdownHighlightedIndex_ = dropdown.value;
        return;
    }
    if (dropdown.value != dropdownHighlightedIndex_) {
        dropdown.value = dropdownHighlightedIndex_;
        pendingDropdownValueChanges_.push_back({entity.id, dropdown.value});
    }
    openDropdown_ = {};
}

void EditorScene::HandleGameUiKeyboardSlider(bool canNavigateUi, bool navigatedUi) {
    WorldEntity* keyboardSlider = world_.Find(focusedButton_);
    if (!canNavigateUi || navigatedUi || activeInputField_.IsValid() ||
        keyboardSlider == nullptr || !keyboardSlider->slider || !keyboardSlider->slider->enabled ||
        !keyboardSlider->slider->interactable ||
        !GetUiGroupState(world_, *keyboardSlider).interactable) {
        return;
    }
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    const SliderComponent& slider = *keyboardSlider->slider;
    const int inputDirection = GetGameUiSliderInputDirection(slider, runtimeInput);
    if (inputDirection == 0) {
        return;
    }
    const bool reverse = slider.direction == SliderDirection::RightToLeft ||
                         slider.direction == SliderDirection::TopToBottom;
    const float step = slider.wholeNumbers ? 1.0f : (slider.maxValue - slider.minValue) * 0.1f;
    const float visualDirection = static_cast<float>(inputDirection);
    SetGameUiSliderValue(*keyboardSlider,
                         slider.value + visualDirection * (reverse ? -step : step));
}

void EditorScene::HandleGameUiCancel(bool canNavigateUi) {
    const Input* runtimeInput = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (!canNavigateUi ||
        (!ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
         (runtimeInput == nullptr ||
          !runtimeInput->IsGamepadButtonTrigger(XINPUT_GAMEPAD_B)))) {
        return;
    }
    if (activeInputField_.IsValid()) {
        activeInputField_ = {};
    } else if (openDropdown_.IsValid()) {
        openDropdown_ = {};
    } else {
        focusedButton_ = {};
    }
}

void EditorScene::DrawGameUiDropdownPopup(int width, int height) {
    SpriteRenderer& spriteRenderer = *ctx_->rendering.spriteRenderer;
    WorldEntity* openDropdownEntity = world_.Find(openDropdown_);
    float dropdownLeft = 0.0f;
    float dropdownTop = 0.0f;
    float dropdownRight = 0.0f;
    float dropdownBottom = 0.0f;
    if (openDropdownEntity != nullptr && openDropdownEntity->dropdown && openDropdownEntity->text &&
        TryCalculateRuntimeGameUiImageRect(*openDropdownEntity, width, height, dropdownLeft,
                                           dropdownTop, dropdownRight, dropdownBottom)) {
        const DropdownComponent& dropdown = *openDropdownEntity->dropdown;
        const TextComponent& text = *openDropdownEntity->text;
        TextRenderer& textRenderer = *ctx_->rendering.text;
        const UiGroupState groupState = GetUiGroupState(world_, *openDropdownEntity);
        float popupScale = 1.0f;
        DirectX::XMFLOAT2 popupOrigin{};
        DirectX::XMFLOAT2 popupResolution{};
        TryCalculateRuntimeGameUiCanvasLayout(*openDropdownEntity, width, height, popupScale,
                                              popupOrigin, popupResolution);
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
            item.size = {dropdownRight - dropdownLeft, rowHeight};
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
}
