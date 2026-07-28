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

void EditorScene::DrawButtonInspector(WorldEntity* entity) {
    if (entity->button) {
        ImGui::SeparatorText("Button");
        if (ImGui::Button("Remove Button")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->button.reset();
            RecordImmediateEdit("Remove Button", before, selectionBefore);
            status_ = "Removed Button.";
        } else {
            ButtonComponent& button = *entity->button;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Button", &button.enabled)) {
                RecordImmediateEdit("Toggle Button", std::move(before), selectionBefore);
                status_ = "Toggled Button.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Button", &button.interactable)) {
                RecordImmediateEdit("Toggle Button Interactable", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Button interactable.";
            }
            const char* navigation = button.navigation == ButtonNavigationMode::None ? "None"
                                     : button.navigation == ButtonNavigationMode::Explicit
                                         ? "Explicit"
                                         : "Automatic";
            if (ImGui::BeginCombo("Navigation##Button", navigation)) {
                const auto selectNavigation = [&](ButtonNavigationMode value, const char* label) {
                    if (ImGui::Selectable(label, button.navigation == value)) {
                        const std::string navigationBefore = WorldSerializer::Serialize(world_);
                        button.navigation = value;
                        RecordImmediateEdit("Change Button Navigation", std::move(navigationBefore),
                                            selectionBefore);
                        status_ = "Changed Button navigation.";
                    }
                };
                selectNavigation(ButtonNavigationMode::Automatic, "Automatic");
                selectNavigation(ButtonNavigationMode::Explicit, "Explicit");
                selectNavigation(ButtonNavigationMode::None, "None");
                ImGui::EndCombo();
            }
            if (button.navigation == ButtonNavigationMode::Explicit) {
                const auto editNavigationTarget = [&](const char* label, const char* popup,
                                                      EntityId& target) {
                    const WorldEntity* targetEntity = world_.Find(target);
                    std::string targetLabel = targetEntity != nullptr ? targetEntity->name
                                              : target.IsValid()      ? "Missing Entity"
                                                                      : "None";
                    targetLabel += "##";
                    targetLabel += label;
                    ImGui::TextUnformatted(label);
                    ImGui::SameLine();
                    if (ImGui::Button(targetLabel.c_str(), {-FLT_MIN, 0.0f})) {
                        ImGui::OpenPopup(popup);
                    }
                    const auto assignTarget = [&](EntityId value) {
                        if (target == value) {
                            return;
                        }
                        const std::string targetBefore = WorldSerializer::Serialize(world_);
                        target = value;
                        RecordImmediateEdit("Assign Button Navigation", targetBefore,
                                            selectionBefore);
                        status_ = "Assigned Button navigation target.";
                    };
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(kEntityDragPayload);
                            payload != nullptr && payload->IsDelivery() &&
                            payload->DataSize == sizeof(EntityId)) {
                            EntityId dropped{};
                            std::memcpy(&dropped, payload->Data, sizeof(dropped));
                            const WorldEntity* droppedEntity = world_.Find(dropped);
                            if (droppedEntity != nullptr && dropped != entity->id &&
                                (droppedEntity->button || droppedEntity->slider)) {
                                assignTarget(dropped);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (ImGui::BeginPopup(popup)) {
                        if (ImGui::MenuItem("None", nullptr, !target.IsValid())) {
                            assignTarget({});
                        }
                        ImGui::Separator();
                        for (const WorldEntity& candidate : world_.Entities()) {
                            if (candidate.id == entity->id ||
                                (!candidate.button && !candidate.slider)) {
                                continue;
                            }
                            const std::string candidateLabel =
                                candidate.name + "##" + candidate.id.ToString();
                            if (ImGui::MenuItem(candidateLabel.c_str(), nullptr,
                                                candidate.id == target)) {
                                assignTarget(candidate.id);
                            }
                        }
                        ImGui::EndPopup();
                    }
                };
                editNavigationTarget("Select On Left", "SelectOnLeftPicker", button.selectOnLeft);
                editNavigationTarget("Select On Right", "SelectOnRightPicker",
                                     button.selectOnRight);
                editNavigationTarget("Select On Up", "SelectOnUpPicker", button.selectOnUp);
                editNavigationTarget("Select On Down", "SelectOnDownPicker", button.selectOnDown);
            }
            const auto editButtonColor = [&](const char* label, DirectX::XMFLOAT4& color) {
                if (ImGui::ColorEdit4(label, &color.x)) {
                    RefreshDirty();
                    status_ = "Modified Button colors.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Button Color");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            editButtonColor("Normal Color##Button", button.normalColor);
            editButtonColor("Hovered Color##Button", button.hoveredColor);
            editButtonColor("Pressed Color##Button", button.pressedColor);
            editButtonColor("Disabled Color##Button", button.disabledColor);
            if (ImGui::DragFloat("Fade Duration##Button", &button.fadeDuration, 0.01f, 0.0f, 10.0f,
                                 "%.2f s")) {
                button.fadeDuration = std::clamp(button.fadeDuration, 0.0f, 10.0f);
                RefreshDirty();
                status_ = "Modified Button fade duration.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Button Fade Duration");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->image) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Button requires an Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled("Add a Script and override OnButtonClick to handle clicks.");
            }
        }
    }
}

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
    if (entity->slider) {
        ImGui::SeparatorText("Slider");
        if (ImGui::Button("Remove Slider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->slider.reset();
            RecordImmediateEdit("Remove Slider", before, selectionBefore);
            status_ = "Removed Slider.";
        } else {
            SliderComponent& slider = *entity->slider;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Slider", &slider.enabled)) {
                RecordImmediateEdit("Toggle Slider", std::move(before), selectionBefore);
                status_ = "Toggled Slider.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Slider", &slider.interactable)) {
                RecordImmediateEdit("Toggle Slider Interactable", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Slider interaction.";
            }
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
                    slider.value =
                        std::clamp(std::round(slider.value), slider.minValue, slider.maxValue);
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
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Whole Numbers##Slider", &slider.wholeNumbers)) {
                if (slider.wholeNumbers) {
                    slider.value =
                        std::clamp(std::round(slider.value), slider.minValue, slider.maxValue);
                }
                RecordImmediateEdit("Toggle Slider Whole Numbers", std::move(before),
                                    selectionBefore);
                status_ = "Changed Slider whole-number mode.";
            }
            const char* direction =
                slider.direction == SliderDirection::RightToLeft   ? "Right To Left"
                : slider.direction == SliderDirection::BottomToTop ? "Bottom To Top"
                : slider.direction == SliderDirection::TopToBottom ? "Top To Bottom"
                                                                   : "Left To Right";
            if (ImGui::BeginCombo("Direction##Slider", direction)) {
                const auto selectDirection = [&](SliderDirection value, const char* label) {
                    if (ImGui::Selectable(label, slider.direction == value)) {
                        const std::string directionBefore = WorldSerializer::Serialize(world_);
                        slider.direction = value;
                        RecordImmediateEdit("Change Slider Direction", std::move(directionBefore),
                                            selectionBefore);
                        status_ = "Changed Slider direction.";
                    }
                };
                selectDirection(SliderDirection::LeftToRight, "Left To Right");
                selectDirection(SliderDirection::RightToLeft, "Right To Left");
                selectDirection(SliderDirection::BottomToTop, "Bottom To Top");
                selectDirection(SliderDirection::TopToBottom, "Top To Bottom");
                ImGui::EndCombo();
            }
            const auto editSliderColor = [&](const char* label, DirectX::XMFLOAT4& color) {
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
            editSliderColor("Fill Color##Slider", slider.fillColor);
            editSliderColor("Handle Color##Slider", slider.handleColor);
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
            if (!entity->image) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Slider requires an Image on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled("Override OnSliderValueChanged to handle value changes.");
            }
        }
    }
}

void EditorScene::DrawDropdownInspector(WorldEntity* entity) {
    if (entity->dropdown) {
        ImGui::SeparatorText("Dropdown");
        if (ImGui::Button("Remove Dropdown")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->dropdown.reset();
            RecordImmediateEdit("Remove Dropdown", before, selectionBefore);
            status_ = "Removed Dropdown.";
        } else {
            DropdownComponent& dropdown = *entity->dropdown;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Dropdown", &dropdown.enabled)) {
                RecordImmediateEdit("Toggle Dropdown", std::move(before), selectionBefore);
                status_ = "Toggled Dropdown.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##Dropdown", &dropdown.interactable)) {
                RecordImmediateEdit("Toggle Dropdown Interactable", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Dropdown interaction.";
            }
            const char* selectedOption =
                dropdown
                    .options[static_cast<size_t>(std::clamp(
                        dropdown.value, 0, static_cast<int32_t>(dropdown.options.size() - 1u)))]
                    .c_str();
            if (ImGui::BeginCombo("Value##Dropdown", selectedOption)) {
                for (size_t optionIndex = 0; optionIndex < dropdown.options.size(); ++optionIndex) {
                    const bool selected = dropdown.value == static_cast<int32_t>(optionIndex);
                    if (ImGui::Selectable(dropdown.options[optionIndex].c_str(), selected)) {
                        const std::string valueBefore = WorldSerializer::Serialize(world_);
                        dropdown.value = static_cast<int32_t>(optionIndex);
                        RecordImmediateEdit("Change Dropdown Value", std::move(valueBefore),
                                            selectionBefore);
                        status_ = "Changed Dropdown value.";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SeparatorText("Options");
            for (size_t optionIndex = 0; optionIndex < dropdown.options.size();) {
                ImGui::PushID(static_cast<int>(optionIndex));
                std::array<char, 257> optionBuffer{};
                const std::string& option = dropdown.options[optionIndex];
                std::copy_n(option.data(), (std::min)(option.size(), optionBuffer.size() - 1u),
                            optionBuffer.data());
                ImGui::SetNextItemWidth(-150.0f);
                if (ImGui::InputText("##Option", optionBuffer.data(), optionBuffer.size()) &&
                    optionBuffer[0] != '\0') {
                    dropdown.options[optionIndex] = optionBuffer.data();
                    RefreshDirty();
                    status_ = "Modified Dropdown option.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Dropdown Option");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
                ImGui::SameLine();
                if (optionIndex == 0u) {
                    ImGui::BeginDisabled();
                }
                const bool moveUp = ImGui::SmallButton("Up");
                if (optionIndex == 0u) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (optionIndex + 1u >= dropdown.options.size()) {
                    ImGui::BeginDisabled();
                }
                const bool moveDown = ImGui::SmallButton("Down");
                if (optionIndex + 1u >= dropdown.options.size()) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                const bool canRemove = dropdown.options.size() > 1u;
                if (!canRemove) {
                    ImGui::BeginDisabled();
                }
                const bool remove = ImGui::SmallButton("Remove");
                if (!canRemove) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
                if (moveUp || moveDown) {
                    const std::string moveBefore = WorldSerializer::Serialize(world_);
                    const size_t destination = moveUp ? optionIndex - 1u : optionIndex + 1u;
                    std::swap(dropdown.options[optionIndex], dropdown.options[destination]);
                    if (dropdown.value == static_cast<int32_t>(optionIndex)) {
                        dropdown.value = static_cast<int32_t>(destination);
                    } else if (dropdown.value == static_cast<int32_t>(destination)) {
                        dropdown.value = static_cast<int32_t>(optionIndex);
                    }
                    RecordImmediateEdit("Move Dropdown Option", std::move(moveBefore),
                                        selectionBefore);
                    status_ = "Moved Dropdown option.";
                    break;
                }
                if (remove) {
                    const std::string removeBefore = WorldSerializer::Serialize(world_);
                    dropdown.options.erase(dropdown.options.begin() +
                                           static_cast<std::ptrdiff_t>(optionIndex));
                    dropdown.value = std::clamp(dropdown.value, 0,
                                                static_cast<int32_t>(dropdown.options.size() - 1u));
                    RecordImmediateEdit("Remove Dropdown Option", std::move(removeBefore),
                                        selectionBefore);
                    status_ = "Removed Dropdown option.";
                    continue;
                }
                ++optionIndex;
            }
            if (dropdown.options.size() < 256u && ImGui::Button("Add Option##Dropdown")) {
                const std::string addBefore = WorldSerializer::Serialize(world_);
                dropdown.options.push_back("Option");
                RecordImmediateEdit("Add Dropdown Option", std::move(addBefore), selectionBefore);
                status_ = "Added Dropdown option.";
            }
            const auto editDropdownColor = [&](const char* label, DirectX::XMFLOAT4& color) {
                if (ImGui::ColorEdit4(label, &color.x)) {
                    RefreshDirty();
                    status_ = "Modified Dropdown colors.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Dropdown Color");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            editDropdownColor("Item Color##Dropdown", dropdown.itemColor);
            editDropdownColor("Highlighted Color##Dropdown", dropdown.highlightedColor);
            if (ImGui::DragFloat("Item Height##Dropdown", &dropdown.itemHeight, 1.0f, 1.0f,
                                 1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Dropdown item height.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Dropdown Item Height");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (!entity->button || !entity->image || !entity->text) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Dropdown requires Button, Image, and Text on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled("Override OnDropdownValueChanged to handle value changes.");
            }
        }
    }
}

void EditorScene::DrawInputFieldInspector(WorldEntity* entity) {
    if (entity->inputField) {
        ImGui::SeparatorText("Input Field");
        if (ImGui::Button("Remove Input Field")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->inputField.reset();
            RecordImmediateEdit("Remove InputField", before, selectionBefore);
            status_ = "Removed Input Field.";
        } else {
            InputFieldComponent& inputField = *entity->inputField;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##InputField", &inputField.enabled)) {
                RecordImmediateEdit("Toggle InputField", std::move(before), selectionBefore);
                status_ = "Toggled Input Field.";
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Interactable##InputField", &inputField.interactable)) {
                RecordImmediateEdit("Toggle InputField Interactable", std::move(before),
                                    selectionBefore);
                status_ = "Toggled Input Field interaction.";
            }
            std::array<char, 4097> textBuffer{};
            std::copy_n(inputField.text.data(),
                        (std::min)(inputField.text.size(), textBuffer.size() - 1u),
                        textBuffer.data());
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
            if (ImGui::DragInt("Character Limit##InputField", &inputField.characterLimit, 1.0f, 0,
                               4096, "%d", ImGuiSliderFlags_AlwaysClamp)) {
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
            if (ImGui::BeginCombo("Content Type##InputField", contentType)) {
                const auto selectContentType = [&](InputFieldContentType value, const char* label) {
                    if (ImGui::Selectable(label, inputField.contentType == value)) {
                        const std::string typeBefore = WorldSerializer::Serialize(world_);
                        inputField.contentType = value;
                        RecordImmediateEdit("Change InputField Content Type", std::move(typeBefore),
                                            selectionBefore);
                        status_ = "Changed Input Field content type.";
                    }
                };
                selectContentType(InputFieldContentType::Standard, "Standard");
                selectContentType(InputFieldContentType::Password, "Password");
                ImGui::EndCombo();
            }
            if (!entity->button || !entity->image || !entity->text) {
                ImGui::TextColored(
                    {1.0f, 0.72f, 0.25f, 1.0f},
                    "Input Field requires Button, Image, and Text on the same Entity.");
            }
            if (entity->scripts.empty()) {
                ImGui::TextDisabled(
                    "Override OnInputFieldValueChanged or OnInputFieldSubmit to handle input.");
            }
        }
    }
}
