#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"

#include <cstring>

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawHierarchyPanel() {
    SynchronizeHierarchySelection();
    const bool editing = !IsInPlayMode();
    if (!editing) {
        ImGui::TextDisabled("Runtime World (Read Only)");
    }
    DrawHierarchyToolbar(editing);
    const std::string query = DrawHierarchySearch();
    RebuildVisibleHierarchyEntities(query);
    ImGui::Separator();
    (void)DrawHierarchyRootEntities(query);
    ImGui::Separator();
    DrawHierarchySceneRoot(editing);
    ClearHierarchySelectionFromEmptySpace();
}

void EditorScene::DrawHierarchyToolbar(const bool editing) {
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Create")) {
        ImGui::OpenPopup("CreateEntity");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("CreateEntity")) {
        ImGui::BeginDisabled(!editing);
        DrawCreateEntityMenu({0.0f, 0.0f, 0.0f});
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    const bool canEditSelection = editing && !hierarchySelection_.empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canEditSelection);
    if (ImGui::Button("Duplicate")) {
        DuplicateSelection();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canEditSelection);
    if (ImGui::Button("Delete")) {
        DeleteSelection();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
}

std::string EditorScene::DrawHierarchySearch() {
    ImGui::SetNextItemWidth(-58.0f);
    ImGui::InputTextWithHint("##HierarchySearch", "Search entities...", hierarchySearch_.data(),
                             hierarchySearch_.size());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        hierarchySearch_.fill('\0');
    }
    return hierarchySearch_.data();
}

void EditorScene::RebuildVisibleHierarchyEntities(const std::string& query) {
    visibleHierarchyEntities_.clear();
    if (query.empty()) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (ContainsCaseInsensitive(entity.name, query)) {
            IncludeVisibleHierarchyAncestors(entity.id);
        }
    }
}

void EditorScene::IncludeVisibleHierarchyAncestors(EntityId id) {
    for (size_t depth = 0; id.IsValid() && depth <= world_.Entities().size(); ++depth) {
        if (!visibleHierarchyEntities_.insert(id).second) {
            return;
        }
        const WorldEntity* entity = world_.Find(id);
        id = entity != nullptr ? entity->parent : EntityId{};
    }
}

bool EditorScene::DrawHierarchyRootEntities(const std::string& query) {
    bool drewEntity = false;
    for (const EntityId id : world_.GetRootEntities()) {
        if (!query.empty() && !visibleHierarchyEntities_.contains(id)) {
            continue;
        }
        DrawEntityNode(id);
        drewEntity = true;
    }
    if (!query.empty() && !drewEntity) {
        ImGui::TextDisabled("No matching entities.");
    }
    return drewEntity;
}

void EditorScene::DrawHierarchySceneRoot(const bool editing) {
    if (ImGui::Selectable(editing ? "Scene Root (drop here)" : "Scene Root", false)) {
        ClearHierarchySelection();
    }
    if (editing) {
        HandleHierarchySceneRootDrop();
    }
}

void EditorScene::HandleHierarchySceneRootDrop() {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    AcceptHierarchyEntityRootDrop();
    AcceptHierarchyPrefabRootDrop();
    ImGui::EndDragDropTarget();
}

void EditorScene::AcceptHierarchyEntityRootDrop() {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
    if (payload == nullptr || !payload->IsDelivery() ||
        payload->DataSize != sizeof(EntityId)) {
        return;
    }
    EntityId child{};
    std::memcpy(&child, payload->Data, sizeof(child));
    ReparentSelection(child, {});
}

void EditorScene::AcceptHierarchyPrefabRootDrop() {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
    if (payload == nullptr || !payload->IsDelivery() || payload->DataSize <= 1 ||
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] != '\0') {
        return;
    }
    InstantiatePrefabAsset(static_cast<const char*>(payload->Data));
}

void EditorScene::ClearHierarchySelectionFromEmptySpace() {
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered()) {
        ClearHierarchySelection();
    }
}
