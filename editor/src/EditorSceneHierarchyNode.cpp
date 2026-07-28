#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"

#include <cstring>
#include <iterator>
#include <ranges>

using namespace EditorSceneHierarchyUtils;

std::vector<EntityId> EditorScene::GetVisibleHierarchyChildren(EntityId id) const {
    std::vector<EntityId> children = world_.GetChildren(id);
    if (hierarchySearch_[0] != '\0') {
        std::erase_if(children, [this](EntityId child) {
            return !visibleHierarchyEntities_.contains(child);
        });
    }
    return children;
}

int EditorScene::BuildHierarchyNodeFlags(EntityId id, bool filtering, bool hasChildren) const {
    int flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (filtering) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (IsHierarchyEntitySelected(id)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    return flags;
}

bool EditorScene::DrawHierarchyNodeHeader(EntityId id, int flags, bool editing, ImVec2& nodeMin,
                                          ImVec2& nodeMax) {
    const WorldEntity* entity = world_.Find(id);
    bool active = entity->active;
    ImGui::BeginDisabled(!editing);
    if (ImGui::Checkbox("##EntityActive", &active)) {
        SetSelectedEntitiesActive(id, active);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(editing ? (active ? "Deactivate Entity" : "Activate Entity")
                                  : "Entity active state (read-only in Play Mode)");
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const bool activeInHierarchy = world_.IsActiveInHierarchy(id);
    if (!activeInHierarchy) {
        const ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              {textColor.x, textColor.y, textColor.z, textColor.w * 0.45f});
    }
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    nodeMin = ImGui::GetItemRectMin();
    nodeMax = ImGui::GetItemRectMax();
    if (!activeInHierarchy) {
        ImGui::PopStyleColor();
    }
    return open;
}

void EditorScene::HandleHierarchyNodeSelection(EntityId id) {
    if (ImGui::IsItemClicked()) {
        const ImGuiIO& io = ImGui::GetIO();
        SelectHierarchyEntity(id, io.KeyCtrl, io.KeyShift);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        SelectHierarchyEntity(id, false, false);
        FocusSceneCameraOnSelection();
    }
}

bool EditorScene::DrawHierarchyEntityContextMenu(EntityId id, bool editing) {
    if (!ImGui::BeginPopupContextItem("EntityContext")) {
        return false;
    }
    if (!IsHierarchyEntitySelected(id)) {
        SelectHierarchyEntity(id, false, false);
    }
    bool hierarchyChanged = false;
    bool deleteRequested = false;
    if (ImGui::BeginMenu("Create Child", editing)) {
        hierarchyChanged = DrawCreateEntityMenu({0.0f, 0.0f, 0.0f}, id);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, editing)) {
        RequestEntityRename(id);
    }
    if (ImGui::MenuItem("Focus in Scene", "F")) {
        SelectHierarchyEntity(id, false, false);
        FocusSceneCameraOnSelection();
    }
    const WorldEntity* entity = world_.Find(id);
    if (ImGui::MenuItem("Active", nullptr, entity->active, editing)) {
        SetSelectedEntitiesActive(id, !entity->active);
    }
    const std::vector<EntityId> siblings =
        entity->parent.IsValid() ? world_.GetChildren(entity->parent) : world_.GetRootEntities();
    const auto siblingPosition = std::ranges::find(siblings, id);
    const bool canMoveUp = siblingPosition != siblings.end() && siblingPosition != siblings.begin();
    const bool canMoveDown =
        siblingPosition != siblings.end() && std::next(siblingPosition) != siblings.end();
    if (ImGui::MenuItem("Move Up", "Alt+Up", false, editing && canMoveUp)) {
        hierarchyChanged = MoveEntityInHierarchy(id, -1);
    }
    if (ImGui::MenuItem("Move Down", "Alt+Down", false, editing && canMoveDown)) {
        hierarchyChanged = MoveEntityInHierarchy(id, 1);
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editing)) {
        DuplicateSelection();
        hierarchyChanged = true;
    }
    if (ImGui::MenuItem("Save as Prefab...", nullptr, false,
                        editing && GetTopLevelSelectedEntities().size() == 1u)) {
        SaveSelectionAsPrefab();
    }
    if (ImGui::MenuItem("Copy", "Ctrl+C")) {
        CopySelection();
    }
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, editing)) {
        CutSelection();
        hierarchyChanged = true;
    }
    if (ImGui::MenuItem("Paste as Child", nullptr, false, editing && !entityClipboard_.empty())) {
        hierarchyChanged = PasteEntityClipboard(id);
    }
    if (ImGui::MenuItem("Delete", "Delete", false, editing)) {
        deleteRequested = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Entity ID")) {
        const std::string idText = id.ToString();
        ImGui::SetClipboardText(idText.c_str());
        status_ = "Copied entity ID: " + idText;
    }
    ImGui::EndPopup();
    if (deleteRequested) {
        DeleteSelection();
        hierarchyChanged = true;
    }
    return hierarchyChanged;
}

void EditorScene::DrawHierarchyEntityDragSource(EntityId id, bool editing) {
    if (!editing || !ImGui::BeginDragDropSource()) {
        return;
    }
    ImGui::SetDragDropPayload(kEntityDragPayload, &id, sizeof(id));
    const WorldEntity* entity = world_.Find(id);
    if (IsHierarchyEntitySelected(id) && hierarchySelection_.size() > 1u) {
        ImGui::Text("Move %zu selected entities", hierarchySelection_.size());
    } else {
        ImGui::TextUnformatted(entity->name.c_str());
    }
    ImGui::EndDragDropSource();
}

void EditorScene::DrawHierarchyEntityDropTarget(EntityId id, bool editing, const ImVec2& nodeMin,
                                                const ImVec2& nodeMax) {
    if (!editing || !ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
            kEntityDragPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        payload != nullptr && payload->DataSize == sizeof(EntityId)) {
        const float rowHeight = nodeMax.y - nodeMin.y;
        const float mouseY = ImGui::GetIO().MousePos.y;
        const bool insertBefore = mouseY < nodeMin.y + rowHeight * 0.25f;
        const bool insertAfter = mouseY > nodeMax.y - rowHeight * 0.25f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 targetColor = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
        if (insertBefore || insertAfter) {
            const float lineY = insertBefore ? nodeMin.y : nodeMax.y;
            drawList->AddLine({nodeMin.x, lineY}, {nodeMax.x, lineY}, targetColor, 2.0f);
        } else {
            drawList->AddRect(nodeMin, nodeMax, targetColor, 2.0f, 0, 2.0f);
        }
        if (payload->IsDelivery()) {
            EntityId dragged{};
            std::memcpy(&dragged, payload->Data, sizeof(dragged));
            if (insertBefore || insertAfter) {
                MoveSelectionAdjacentTo(dragged, id, insertAfter);
            } else {
                ReparentSelection(dragged, id);
            }
        }
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        AssignModelAsset(id, static_cast<const char*>(payload->Data));
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAudioAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        AssignAudioAsset(id, static_cast<const char*>(payload->Data));
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        AssignScriptAsset(id, static_cast<const char*>(payload->Data));
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        InstantiatePrefabAsset(static_cast<const char*>(payload->Data), id);
    }
    ImGui::EndDragDropTarget();
}
