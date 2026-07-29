#include "EditorScene.h"

#include "imgui.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <array>
#include <cstddef>

void EditorScene::DrawDropdownInspector(WorldEntity* entity) {
    if (!entity->dropdown) {
        return;
    }
    ImGui::SeparatorText("Dropdown");
    if (ImGui::Button("Remove Dropdown")) {
        RemoveDropdownComponent(*entity);
        return;
    }
    DropdownComponent& dropdown = *entity->dropdown;
    DrawDropdownGeneralSettings(dropdown);
    DrawDropdownValue(dropdown);
    DrawDropdownOptions(dropdown);
    DrawDropdownAppearance(dropdown);
    DrawDropdownRequirements(*entity);
}

void EditorScene::RemoveDropdownComponent(WorldEntity& entity) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity.dropdown.reset();
    RecordImmediateEdit("Remove Dropdown", before, selectionBefore);
    status_ = "Removed Dropdown.";
}

void EditorScene::DrawDropdownGeneralSettings(DropdownComponent& dropdown) {
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
}

void EditorScene::DrawDropdownValue(DropdownComponent& dropdown) {
    const size_t selectedIndex = static_cast<size_t>(
        std::clamp(dropdown.value, 0, static_cast<int32_t>(dropdown.options.size() - 1u)));
    if (!ImGui::BeginCombo("Value##Dropdown", dropdown.options[selectedIndex].c_str())) {
        return;
    }
    for (size_t optionIndex = 0; optionIndex < dropdown.options.size(); ++optionIndex) {
        const bool selected = dropdown.value == static_cast<int32_t>(optionIndex);
        if (ImGui::Selectable(dropdown.options[optionIndex].c_str(), selected)) {
            const std::string before = WorldSerializer::Serialize(world_);
            dropdown.value = static_cast<int32_t>(optionIndex);
            RecordImmediateEdit("Change Dropdown Value", before, selection_);
            status_ = "Changed Dropdown value.";
        }
    }
    ImGui::EndCombo();
}

void EditorScene::DrawDropdownOptions(DropdownComponent& dropdown) {
    ImGui::SeparatorText("Options");
    for (size_t optionIndex = 0; optionIndex < dropdown.options.size();) {
        ImGui::PushID(static_cast<int>(optionIndex));
        EditDropdownOptionText(dropdown, optionIndex);
        const DropdownOptionAction action =
            DrawDropdownOptionActions(optionIndex, dropdown.options.size());
        ImGui::PopID();
        if (action == DropdownOptionAction::MoveUp ||
            action == DropdownOptionAction::MoveDown) {
            MoveDropdownOption(dropdown, optionIndex, action);
            break;
        }
        if (action == DropdownOptionAction::Remove) {
            RemoveDropdownOption(dropdown, optionIndex);
            continue;
        }
        ++optionIndex;
    }
    AddDropdownOption(dropdown);
}

void EditorScene::EditDropdownOptionText(DropdownComponent& dropdown,
                                         const size_t optionIndex) {
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
}

EditorScene::DropdownOptionAction EditorScene::DrawDropdownOptionActions(
    const size_t optionIndex, const size_t optionCount) {
    ImGui::SameLine();
    ImGui::BeginDisabled(optionIndex == 0u);
    const bool moveUp = ImGui::SmallButton("Up");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(optionIndex + 1u >= optionCount);
    const bool moveDown = ImGui::SmallButton("Down");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(optionCount <= 1u);
    const bool remove = ImGui::SmallButton("Remove");
    ImGui::EndDisabled();
    if (moveUp) {
        return DropdownOptionAction::MoveUp;
    }
    if (moveDown) {
        return DropdownOptionAction::MoveDown;
    }
    return remove ? DropdownOptionAction::Remove : DropdownOptionAction::None;
}

void EditorScene::MoveDropdownOption(DropdownComponent& dropdown, const size_t optionIndex,
                                     const DropdownOptionAction action) {
    const std::string before = WorldSerializer::Serialize(world_);
    const size_t destination =
        action == DropdownOptionAction::MoveUp ? optionIndex - 1u : optionIndex + 1u;
    std::swap(dropdown.options[optionIndex], dropdown.options[destination]);
    if (dropdown.value == static_cast<int32_t>(optionIndex)) {
        dropdown.value = static_cast<int32_t>(destination);
    } else if (dropdown.value == static_cast<int32_t>(destination)) {
        dropdown.value = static_cast<int32_t>(optionIndex);
    }
    RecordImmediateEdit("Move Dropdown Option", before, selection_);
    status_ = "Moved Dropdown option.";
}

void EditorScene::RemoveDropdownOption(DropdownComponent& dropdown,
                                       const size_t optionIndex) {
    const std::string before = WorldSerializer::Serialize(world_);
    dropdown.options.erase(dropdown.options.begin() +
                           static_cast<std::ptrdiff_t>(optionIndex));
    dropdown.value =
        std::clamp(dropdown.value, 0, static_cast<int32_t>(dropdown.options.size() - 1u));
    RecordImmediateEdit("Remove Dropdown Option", before, selection_);
    status_ = "Removed Dropdown option.";
}

void EditorScene::AddDropdownOption(DropdownComponent& dropdown) {
    if (dropdown.options.size() >= 256u || !ImGui::Button("Add Option##Dropdown")) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    dropdown.options.push_back("Option");
    RecordImmediateEdit("Add Dropdown Option", before, selection_);
    status_ = "Added Dropdown option.";
}

void EditorScene::DrawDropdownAppearance(DropdownComponent& dropdown) {
    EditDropdownColor("Item Color##Dropdown", dropdown.itemColor);
    EditDropdownColor("Highlighted Color##Dropdown", dropdown.highlightedColor);
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
}

void EditorScene::EditDropdownColor(const char* label, DirectX::XMFLOAT4& color) {
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
}

void EditorScene::DrawDropdownRequirements(const WorldEntity& entity) const {
    if (!entity.button || !entity.image || !entity.text) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                           "Dropdown requires Button, Image, and Text on the same Entity.");
    }
    if (entity.scripts.empty()) {
        ImGui::TextDisabled("Override OnDropdownValueChanged to handle value changes.");
    }
}
