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

#include "internal/EditorSceneHierarchyUtils.h"

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

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawToggleInspector(WorldEntity* entity) {
    if (entity->toggle) {
        ImGui::SeparatorText("Toggle");
        if (ImGui::Button("Remove Toggle")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->toggle.reset();
            RecordImmediateEdit("Remove Toggle", before, selectionBefore);
            status_ = "Removed Toggle.";
        } else {
            ToggleComponent& toggle = *entity->toggle;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Toggle", &toggle.enabled)) {
                RecordImmediateEdit("Toggle Toggle", std::move(before), selectionBefore);
                status_ = "Toggled Toggle.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Is On##Toggle", &toggle.isOn)) {
                RecordImmediateEdit("Change Toggle Value", std::move(before), selectionBefore);
                status_ = "Changed Toggle value.";
            }
            if (ImGui::ColorEdit4("Checkmark Color##Toggle", &toggle.checkmarkColor.x)) {
                RefreshDirty();
                status_ = "Modified Toggle checkmark color.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Toggle Checkmark Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Checkmark Scale##Toggle", &toggle.checkmarkScale, 0.01f, 0.0f,
                                 1.0f, "%.2f")) {
                toggle.checkmarkScale = std::clamp(toggle.checkmarkScale, 0.0f, 1.0f);
                RefreshDirty();
                status_ = "Modified Toggle checkmark scale.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Toggle Checkmark Scale");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->button || !entity->image) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Toggle requires Button and Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled("Override OnToggleValueChanged to handle value changes.");
            }
        }
    }
}

void EditorScene::DrawSliderInspector(WorldEntity* entity) {
    if (!entity->slider) {
        return;
    }
    ImGui::SeparatorText("Slider");
    if (ImGui::Button("Remove Slider")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->slider.reset();
        RecordImmediateEdit("Remove Slider", before, selectionBefore);
        status_ = "Removed Slider.";
        return;
    }
    SliderComponent& slider = *entity->slider;
    const EntityId selectionBefore = selection_;
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Enabled##Slider", &slider.enabled)) {
        RecordImmediateEdit("Toggle Slider", std::move(before), selectionBefore);
        status_ = "Toggled Slider.";
    }
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Interactable##Slider", &slider.interactable)) {
        RecordImmediateEdit("Toggle Slider Interactable", std::move(before), selectionBefore);
        status_ = "Toggled Slider interaction.";
    }
    DrawSliderRangeInspector(slider, selectionBefore);
    DrawSliderDirectionInspector(slider, selectionBefore);
    DrawSliderAppearanceInspector(slider);
    if (!entity->image) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                           "Slider requires an Image on the same Entity.");
    }
    if (entity->scripts.empty()) {
        ImGui::TextDisabled("Override OnSliderValueChanged to handle value changes.");
    }
}

void EditorScene::DrawSliderRangeInspector(SliderComponent& slider, EntityId selectionBefore) {
    if (ImGui::DragFloat("Min Value##Slider", &slider.minValue, 0.1f, -1000000.0f,
                         999999.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
        slider.minValue = (std::min)(slider.minValue, slider.maxValue - 0.001f);
        slider.value = std::clamp(slider.value, slider.minValue, slider.maxValue);
        RefreshDirty();
        status_ = "Modified Slider range.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Slider Range");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    if (ImGui::DragFloat("Max Value##Slider", &slider.maxValue, 0.1f, -999999.0f,
                         1000000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
        slider.maxValue = (std::max)(slider.maxValue, slider.minValue + 0.001f);
        slider.value = std::clamp(slider.value, slider.minValue, slider.maxValue);
        RefreshDirty();
        status_ = "Modified Slider range.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Slider Range");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    if (ImGui::SliderFloat("Value##Slider", &slider.value, slider.minValue, slider.maxValue,
                           "%.3f")) {
        if (slider.wholeNumbers) {
            slider.value = std::clamp(std::round(slider.value), slider.minValue, slider.maxValue);
        }
        RefreshDirty();
        status_ = "Modified Slider value.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Slider Value");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Whole Numbers##Slider", &slider.wholeNumbers)) {
        if (slider.wholeNumbers) {
            slider.value = std::clamp(std::round(slider.value), slider.minValue, slider.maxValue);
        }
        RecordImmediateEdit("Toggle Slider Whole Numbers", std::move(before), selectionBefore);
        status_ = "Changed Slider whole-number mode.";
    }
}

void EditorScene::DrawSliderDirectionInspector(SliderComponent& slider,
                                               EntityId selectionBefore) {
    const char* direction =
        slider.direction == SliderDirection::RightToLeft   ? "Right To Left"
        : slider.direction == SliderDirection::BottomToTop ? "Bottom To Top"
        : slider.direction == SliderDirection::TopToBottom ? "Top To Bottom"
                                                           : "Left To Right";
    if (!ImGui::BeginCombo("Direction##Slider", direction)) {
        return;
    }
    const auto selectDirection = [&](SliderDirection value, const char* label) {
        if (ImGui::Selectable(label, slider.direction == value)) {
            const std::string before = WorldSerializer::Serialize(world_);
            slider.direction = value;
            RecordImmediateEdit("Change Slider Direction", before, selectionBefore);
            status_ = "Changed Slider direction.";
        }
    };
    selectDirection(SliderDirection::LeftToRight, "Left To Right");
    selectDirection(SliderDirection::RightToLeft, "Right To Left");
    selectDirection(SliderDirection::BottomToTop, "Bottom To Top");
    selectDirection(SliderDirection::TopToBottom, "Top To Bottom");
    ImGui::EndCombo();
}

void EditorScene::DrawSliderAppearanceInspector(SliderComponent& slider) {
    const auto editColor = [&](const char* label, DirectX::XMFLOAT4& color) {
        if (ImGui::ColorEdit4(label, &color.x)) {
            RefreshDirty();
            status_ = "Modified Slider colors.";
        }
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit("Modify Slider Color");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    };
    editColor("Fill Color##Slider", slider.fillColor);
    editColor("Handle Color##Slider", slider.handleColor);
    if (ImGui::DragFloat("Handle Size##Slider", &slider.handleSize, 1.0f, 0.0f, 1000000.0f,
                         "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Slider handle size.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Slider Handle Size");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}

void EditorScene::DrawInputFieldInspector(WorldEntity* entity) {
    if (!entity->inputField) {
        return;
    }
    ImGui::SeparatorText("Input Field");
    if (ImGui::Button("Remove Input Field")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->inputField.reset();
        RecordImmediateEdit("Remove InputField", before, selectionBefore);
        status_ = "Removed Input Field.";
        return;
    }
    InputFieldComponent& inputField = *entity->inputField;
    const EntityId selectionBefore = selection_;
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Enabled##InputField", &inputField.enabled)) {
        RecordImmediateEdit("Toggle InputField", std::move(before), selectionBefore);
        status_ = "Toggled Input Field.";
    }
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Interactable##InputField", &inputField.interactable)) {
        RecordImmediateEdit("Toggle InputField Interactable", std::move(before), selectionBefore);
        status_ = "Toggled Input Field interaction.";
    }
    DrawInputFieldTextInspector(inputField);
    DrawInputFieldSettingsInspector(inputField, selectionBefore);
    if (!entity->button || !entity->image || !entity->text) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                           "Input Field requires Button, Image, and Text on the same Entity.");
    }
    if (entity->scripts.empty()) {
        ImGui::TextDisabled(
            "Override OnInputFieldValueChanged or OnInputFieldSubmit to handle input.");
    }
}

void EditorScene::DrawInputFieldTextInspector(InputFieldComponent& inputField) {
    std::array<char, 4097> textBuffer{};
    std::copy_n(inputField.text.data(),
                (std::min)(inputField.text.size(), textBuffer.size() - 1u), textBuffer.data());
    if (ImGui::InputText("Text##InputField", textBuffer.data(), textBuffer.size())) {
        inputField.text = textBuffer.data();
        RefreshDirty();
        status_ = "Modified Input Field text.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify InputField Text");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    std::array<char, 1025> placeholderBuffer{};
    std::copy_n(inputField.placeholder.data(),
                (std::min)(inputField.placeholder.size(), placeholderBuffer.size() - 1u),
                placeholderBuffer.data());
    if (ImGui::InputText("Placeholder##InputField", placeholderBuffer.data(),
                         placeholderBuffer.size())) {
        inputField.placeholder = placeholderBuffer.data();
        RefreshDirty();
        status_ = "Modified Input Field placeholder.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify InputField Placeholder");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}

void EditorScene::DrawInputFieldSettingsInspector(InputFieldComponent& inputField,
                                                  EntityId selectionBefore) {
    if (ImGui::DragInt("Character Limit##InputField", &inputField.characterLimit, 1.0f, 0, 4096,
                       "%d", ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Input Field character limit.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify InputField Character Limit");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    const char* contentType =
        inputField.contentType == InputFieldContentType::Password ? "Password" : "Standard";
    if (!ImGui::BeginCombo("Content Type##InputField", contentType)) {
        return;
    }
    const auto selectContentType = [&](InputFieldContentType value, const char* label) {
        if (ImGui::Selectable(label, inputField.contentType == value)) {
            const std::string before = WorldSerializer::Serialize(world_);
            inputField.contentType = value;
            RecordImmediateEdit("Change InputField Content Type", before, selectionBefore);
            status_ = "Changed Input Field content type.";
        }
    };
    selectContentType(InputFieldContentType::Standard, "Standard");
    selectContentType(InputFieldContentType::Password, "Password");
    ImGui::EndCombo();
}
