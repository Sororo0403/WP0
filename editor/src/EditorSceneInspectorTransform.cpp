#include "EditorScene.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "world/WorldSerializer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <ranges>

namespace {
float ApplyTransformComponent(float current, float oldActive, float newActive,
                              float additiveDelta, bool scale) {
    constexpr float epsilon = 1.0e-6f;
    if (scale && std::abs(oldActive) > epsilon) {
        return current * (newActive / oldActive);
    }
    return current + additiveDelta;
}
} // namespace

void EditorScene::DrawSelectedEntitiesActive(
    const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities) {
    bool displayedActive = entity.active;
    const bool mixedActive =
        std::ranges::any_of(inspectedEntities, [&](EntityId inspected) {
            const WorldEntity* target = world_.Find(inspected);
            return target != nullptr && target->active != displayedActive;
        });
    if (mixedActive) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    if (ImGui::Checkbox("Active", &displayedActive)) {
        SetSelectedEntitiesActive(selection_, displayedActive);
    }
    if (mixedActive) {
        ImGui::PopItemFlag();
    }
}

void EditorScene::DrawEntitySelectionIdentity(
    WorldEntity& entity, const std::vector<EntityId>& inspectedEntities) {
    if (inspectedEntities.size() > 1u) {
        ImGui::Text("%zu Entities Selected", inspectedEntities.size());
        ImGui::TextDisabled("Transform changes preserve relative offsets and scale ratios.");
        return;
    }
    std::array<char, 256> nameBuffer{};
    strncpy_s(nameBuffer.data(), nameBuffer.size(), entity.name.c_str(), _TRUNCATE);
    const bool nameChanged = ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size());
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Rename Entity");
    }
    if (nameChanged) {
        entity.name = nameBuffer.data();
        if (entity.name.empty()) {
            entity.name = "Entity";
        }
        RefreshDirty();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    ImGui::TextDisabled("ID: %s", entity.id.ToString().c_str());
}

void EditorScene::DrawSelectedEntitiesLayer(
    const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities) {
    const uint8_t displayedLayer = entity.layer;
    const bool mixedLayers =
        std::ranges::any_of(inspectedEntities, [&](EntityId inspected) {
            const WorldEntity* target = world_.Find(inspected);
            return target != nullptr && target->layer != displayedLayer;
        });
    std::string layerPreview = mixedLayers ? "Mixed" : physicsSettings_.layerNames[displayedLayer];
    if (layerPreview.empty()) {
        layerPreview = "Layer " + std::to_string(displayedLayer) + " (Undefined)";
    }
    if (!ImGui::BeginCombo("Layer", layerPreview.c_str())) {
        return;
    }
    for (size_t layer = 0u; layer < PhysicsSettings::kLayerCount; ++layer) {
        if (physicsSettings_.layerNames[layer].empty()) {
            continue;
        }
        const bool selected = !mixedLayers && displayedLayer == layer;
        const std::string label =
            std::to_string(layer) + ": " + physicsSettings_.layerNames[layer];
        if (ImGui::Selectable(label.c_str(), selected)) {
            CommitHistoryEdit();
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            for (EntityId inspected : inspectedEntities) {
                if (WorldEntity* target = world_.Find(inspected)) {
                    target->layer = static_cast<uint8_t>(layer);
                }
            }
            RecordImmediateEdit(inspectedEntities.size() > 1u ? "Set Entity Layers"
                                                              : "Set Entity Layer",
                                before, selectionBefore);
            status_ = inspectedEntities.size() > 1u ? "Changed the selected Entity Layers."
                                                    : "Changed the Entity Layer.";
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void EditorScene::ResetSelectedTransforms(
    const std::vector<EntityId>& inspectedEntities) {
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    for (EntityId inspected : inspectedEntities) {
        if (WorldEntity* target = world_.Find(inspected)) {
            target->transform = TransformComponent{};
        }
    }
    const bool multipleEntities = inspectedEntities.size() > 1u;
    RecordImmediateEdit(multipleEntities ? "Reset Transforms" : "Reset Transform",
                        before, selectionBefore);
    status_ = multipleEntities ? "Reset selected Transforms." : "Reset Transform.";
}

void EditorScene::PasteSelectedTransforms(
    const std::vector<EntityId>& inspectedEntities) {
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    for (EntityId inspected : inspectedEntities) {
        if (WorldEntity* target = world_.Find(inspected)) {
            target->transform = *transformClipboard_;
        }
    }
    const bool multipleEntities = inspectedEntities.size() > 1u;
    RecordImmediateEdit(multipleEntities ? "Paste Transforms" : "Paste Transform",
                        before, selectionBefore);
    status_ = multipleEntities ? "Pasted Transform to selected entities."
                               : "Pasted Transform.";
}

void EditorScene::DrawTransformToolbar(
    const WorldEntity& entity, const std::vector<EntityId>& inspectedEntities) {
    const bool multipleEntities = inspectedEntities.size() > 1u;
    ImGui::Separator();
    ImGui::TextUnformatted(multipleEntities ? "Transforms" : "Transform");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##Transform")) {
        ResetSelectedTransforms(inspectedEntities);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##Transform")) {
        CommitHistoryEdit();
        transformClipboard_ = entity.transform;
        status_ = "Copied Transform.";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!transformClipboard_.has_value());
    if (ImGui::SmallButton("Paste##Transform")) {
        PasteSelectedTransforms(inspectedEntities);
    }
    ImGui::EndDisabled();
}

void EditorScene::DrawTransformField(
    WorldEntity& entity, const std::vector<EntityId>& inspectedEntities,
    const char* label, DirectX::XMFLOAT3 TransformComponent::* member,
    float speed, bool scale) {
    const bool multipleEntities = inspectedEntities.size() > 1u;
    const DirectX::XMFLOAT3 previous = entity.transform.*member;
    DirectX::XMFLOAT3 edited = previous;
    const bool changed = ImGui::DragFloat3(label, &edited.x, speed);
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit(
            std::string(multipleEntities ? "Modify Transforms " : "Modify Transform ") + label);
    }
    if (changed) {
        const DirectX::XMFLOAT3 delta{
            edited.x - previous.x,
            edited.y - previous.y,
            edited.z - previous.z,
        };
        for (EntityId inspected : inspectedEntities) {
            WorldEntity* target = world_.Find(inspected);
            if (target == nullptr) {
                continue;
            }
            DirectX::XMFLOAT3& value = target->transform.*member;
            if (inspected == selection_) {
                value = edited;
            } else {
                value = {
                    ApplyTransformComponent(
                        value.x, previous.x, edited.x, delta.x, scale),
                    ApplyTransformComponent(
                        value.y, previous.y, edited.y, delta.y, scale),
                    ApplyTransformComponent(
                        value.z, previous.z, edited.z, delta.z, scale),
                };
            }
        }
        RefreshDirty();
        status_ = multipleEntities ? "Modified selected Transforms." : "Modified Transform.";
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}

void EditorScene::DrawEntityHeaderAndTransformInspector(
    WorldEntity* entity, const std::vector<EntityId>& inspectedEntities) {
    DrawSelectedEntitiesActive(*entity, inspectedEntities);
    DrawEntitySelectionIdentity(*entity, inspectedEntities);
    DrawSelectedEntitiesLayer(*entity, inspectedEntities);
    DrawTransformToolbar(*entity, inspectedEntities);
    DrawTransformField(
        *entity, inspectedEntities, "Position", &TransformComponent::position, 0.05f, false);
    DrawTransformField(*entity, inspectedEntities, "Rotation",
                       &TransformComponent::rotationDegrees, 0.25f, false);
    DrawTransformField(
        *entity, inspectedEntities, "Scale", &TransformComponent::scale, 0.02f, true);
    if (inspectedEntities.size() > 1u) {
        ImGui::Separator();
        ImGui::TextDisabled("Component editing is available when one Entity is selected.");
    }
}
