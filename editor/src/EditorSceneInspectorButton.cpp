#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawButtonInspector(WorldEntity* entity) {
    if (!entity->button) {
        return;
    }
    ImGui::SeparatorText("Button");
    if (ImGui::Button("Remove Button")) {
        RemoveButtonComponent(*entity);
        return;
    }
    ButtonComponent& button = *entity->button;
    DrawButtonGeneralSettings(button);
    DrawButtonNavigation(*entity, button);
    DrawButtonAppearance(button);
    DrawButtonRequirements(*entity);
}

void EditorScene::RemoveButtonComponent(WorldEntity& entity) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity.button.reset();
    RecordImmediateEdit("Remove Button", before, selectionBefore);
    status_ = "Removed Button.";
}

void EditorScene::DrawButtonGeneralSettings(ButtonComponent& button) {
    const EntityId selectionBefore = selection_;
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Enabled##Button", &button.enabled)) {
        RecordImmediateEdit("Toggle Button", std::move(before), selectionBefore);
        status_ = "Toggled Button.";
    }
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Interactable##Button", &button.interactable)) {
        RecordImmediateEdit("Toggle Button Interactable", std::move(before), selectionBefore);
        status_ = "Toggled Button interactable.";
    }
}

void EditorScene::DrawButtonNavigation(const WorldEntity& entity, ButtonComponent& button) {
    const char* preview = button.navigation == ButtonNavigationMode::None         ? "None"
                          : button.navigation == ButtonNavigationMode::Explicit ? "Explicit"
                                                                                : "Automatic";
    if (ImGui::BeginCombo("Navigation##Button", preview)) {
        SelectButtonNavigationMode(button, ButtonNavigationMode::Automatic, "Automatic");
        SelectButtonNavigationMode(button, ButtonNavigationMode::Explicit, "Explicit");
        SelectButtonNavigationMode(button, ButtonNavigationMode::None, "None");
        ImGui::EndCombo();
    }
    if (button.navigation == ButtonNavigationMode::Explicit) {
        DrawButtonExplicitNavigation(entity, button);
    }
}

void EditorScene::SelectButtonNavigationMode(ButtonComponent& button,
                                             const ButtonNavigationMode mode,
                                             const char* label) {
    if (!ImGui::Selectable(label, button.navigation == mode)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    button.navigation = mode;
    RecordImmediateEdit("Change Button Navigation", before, selection_);
    status_ = "Changed Button navigation.";
}

void EditorScene::DrawButtonExplicitNavigation(const WorldEntity& entity,
                                               ButtonComponent& button) {
    DrawButtonNavigationTarget(entity, "Select On Left", "SelectOnLeftPicker",
                               button.selectOnLeft);
    DrawButtonNavigationTarget(entity, "Select On Right", "SelectOnRightPicker",
                               button.selectOnRight);
    DrawButtonNavigationTarget(entity, "Select On Up", "SelectOnUpPicker",
                               button.selectOnUp);
    DrawButtonNavigationTarget(entity, "Select On Down", "SelectOnDownPicker",
                               button.selectOnDown);
}

void EditorScene::DrawButtonNavigationTarget(const WorldEntity& source, const char* label,
                                             const char* popup, EntityId& target) {
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
    HandleButtonNavigationDrop(source, target);
    DrawButtonNavigationPicker(source, popup, target);
}

void EditorScene::HandleButtonNavigationDrop(const WorldEntity& source, EntityId& target) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
        payload != nullptr && payload->IsDelivery() &&
        payload->DataSize == sizeof(EntityId)) {
        EntityId dropped{};
        std::memcpy(&dropped, payload->Data, sizeof(dropped));
        const WorldEntity* candidate = world_.Find(dropped);
        if (candidate != nullptr && IsButtonNavigationCandidate(source, *candidate)) {
            AssignButtonNavigationTarget(target, dropped);
        }
    }
    ImGui::EndDragDropTarget();
}

void EditorScene::DrawButtonNavigationPicker(const WorldEntity& source, const char* popup,
                                             EntityId& target) {
    if (!ImGui::BeginPopup(popup)) {
        return;
    }
    if (ImGui::MenuItem("None", nullptr, !target.IsValid())) {
        AssignButtonNavigationTarget(target, {});
    }
    ImGui::Separator();
    for (const WorldEntity& candidate : world_.Entities()) {
        if (!IsButtonNavigationCandidate(source, candidate)) {
            continue;
        }
        const std::string label = candidate.name + "##" + candidate.id.ToString();
        if (ImGui::MenuItem(label.c_str(), nullptr, candidate.id == target)) {
            AssignButtonNavigationTarget(target, candidate.id);
        }
    }
    ImGui::EndPopup();
}

void EditorScene::AssignButtonNavigationTarget(EntityId& target, const EntityId value) {
    if (target == value) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    target = value;
    RecordImmediateEdit("Assign Button Navigation", before, selection_);
    status_ = "Assigned Button navigation target.";
}

bool EditorScene::IsButtonNavigationCandidate(const WorldEntity& source,
                                              const WorldEntity& candidate) const {
    return candidate.id != source.id && (candidate.button || candidate.slider);
}

void EditorScene::DrawButtonAppearance(ButtonComponent& button) {
    EditButtonColor("Normal Color##Button", button.normalColor);
    EditButtonColor("Hovered Color##Button", button.hoveredColor);
    EditButtonColor("Pressed Color##Button", button.pressedColor);
    EditButtonColor("Disabled Color##Button", button.disabledColor);
    if (ImGui::DragFloat("Fade Duration##Button", &button.fadeDuration, 0.01f, 0.0f,
                         10.0f, "%.2f s")) {
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
}

void EditorScene::EditButtonColor(const char* label, DirectX::XMFLOAT4& color) {
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
}

void EditorScene::DrawButtonRequirements(const WorldEntity& entity) const {
    if (!entity.image) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                           "Button requires an Image on the same Entity.");
    }
    if (entity.scripts.empty()) {
        ImGui::TextDisabled("Add a Script and override OnButtonClick to handle clicks.");
    }
}
