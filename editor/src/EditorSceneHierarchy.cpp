#include "EditorScene.h"

#include "AssetImportPlanner.h"
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
#include "imgui/ImguiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"
#include "world/WorldCollision.h"

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

#include "internal/EditorSceneHierarchyUtils.h"

using namespace EditorSceneHierarchyUtils;

bool EditorScene::DrawCreateEntityMenu(const DirectX::XMFLOAT3& position, EntityId parent) {
    bool created = false;
    if (ImGui::MenuItem("Empty Entity")) {
        CreateEmptyEntity(position, parent);
        created = true;
    }
    if (ImGui::BeginMenu("3D Primitive")) {
        for (size_t index = 0; index < std::size(kPrimitiveNames); ++index) {
            if (ImGui::MenuItem(kPrimitiveNames[index])) {
                CreatePrimitiveEntity(static_cast<MeshPrimitive>(index), position, parent);
                created = true;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("UI")) {
        if (ImGui::MenuItem("Canvas")) {
            CreateUiEntity(UiEntityPreset::Canvas, parent);
            created = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Text")) {
            CreateUiEntity(UiEntityPreset::Text, parent);
            created = true;
        }
        if (ImGui::MenuItem("Image")) {
            CreateUiEntity(UiEntityPreset::Image, parent);
            created = true;
        }
        if (ImGui::MenuItem("Button")) {
            CreateUiEntity(UiEntityPreset::Button, parent);
            created = true;
        }
        if (ImGui::MenuItem("Toggle")) {
            CreateUiEntity(UiEntityPreset::Toggle, parent);
            created = true;
        }
        if (ImGui::MenuItem("Slider")) {
            CreateUiEntity(UiEntityPreset::Slider, parent);
            created = true;
        }
        if (ImGui::MenuItem("Dropdown")) {
            CreateUiEntity(UiEntityPreset::Dropdown, parent);
            created = true;
        }
        if (ImGui::MenuItem("Input Field")) {
            CreateUiEntity(UiEntityPreset::InputField,
                           parent);
            created = true;
        }
        ImGui::EndMenu();
    }
    return created;
}

void EditorScene::CreateEmptyEntity(const DirectX::XMFLOAT3& position, EntityId parent) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const EntityId entityId = world_.CreateEntity();
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "Could not create an entity.";
        return;
    }
    entity->transform.position = position;
    if (parent.IsValid() && !world_.SetParent(entityId, parent)) {
        world_.DestroyEntity(entityId);
        status_ = "Could not parent the new entity.";
        return;
    }
    selection_ = entityId;
    RecordImmediateEdit("Create Entity", before, selectionBefore);
    status_ = "Created a new entity.";
}

void EditorScene::CreatePrimitiveEntity(MeshPrimitive primitive,
                                        const DirectX::XMFLOAT3& position, EntityId parent) {
    const size_t primitiveIndex = static_cast<size_t>(primitive);
    if (primitiveIndex >= std::size(kPrimitiveNames)) {
        status_ = "Could not create an invalid primitive.";
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const EntityId entityId = world_.CreateEntity(kPrimitiveNames[primitiveIndex]);
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "Could not create the primitive entity.";
        return;
    }
    entity->transform.position = position;
    entity->meshRenderer = MeshRendererComponent{};
    entity->meshRenderer->sourceType = MeshSourceType::Primitive;
    entity->meshRenderer->primitive = primitive;
    entity->materialOverride = MaterialOverrideComponent{};
    if (parent.IsValid() && !world_.SetParent(entityId, parent)) {
        world_.DestroyEntity(entityId);
        status_ = "Could not parent the primitive entity.";
        return;
    }
    selection_ = entityId;
    RecordImmediateEdit("Create Primitive Entity", before, selectionBefore);
    status_ = std::string("Created primitive: ") + kPrimitiveNames[primitiveIndex];
}

void EditorScene::DeleteSelection() {
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    if (roots.empty()) {
        return;
    }
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    size_t deletedCount = 0;
    for (EntityId root : roots) {
        if (world_.DestroyEntity(root)) {
            ++deletedCount;
        }
    }
    if (deletedCount == 0u) {
        status_ = "Could not delete the selected entity hierarchies.";
        return;
    }
    selection_ = {};
    hierarchySelection_.clear();
    hierarchySelectionAnchor_ = {};
    RecordImmediateEdit("Delete Entities", before, selectionBefore);
    status_ = deletedCount == 1u ? "Deleted the selected entity hierarchy."
                                 : "Deleted the selected entity hierarchies.";
}

void EditorScene::DrawEntityNode(EntityId id) {
    const WorldEntity* entity = world_.Find(id);
    const bool filtering = hierarchySearch_[0] != '\0';
    if (entity == nullptr || (filtering && !visibleHierarchyEntities_.contains(id))) {
        return;
    }
    const std::vector<EntityId> children = GetVisibleHierarchyChildren(id);
    const int flags = BuildHierarchyNodeFlags(id, filtering, !children.empty());
    const std::string idText = id.ToString();
    ImGui::PushID(idText.c_str());
    const bool editing = !IsInPlayMode();
    ImVec2 nodeMin{};
    ImVec2 nodeMax{};
    const bool open = DrawHierarchyNodeHeader(id, flags, editing, nodeMin, nodeMax);
    HandleHierarchyNodeSelection(id);
    const bool hierarchyChanged = DrawHierarchyEntityContextMenu(id, editing);
    if (hierarchyChanged) {
        if (open && !children.empty()) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }
    DrawHierarchyEntityDragSource(id, editing);
    DrawHierarchyEntityDropTarget(id, editing, nodeMin, nodeMax);
    if (open && !children.empty()) {
        for (EntityId child : children) {
            DrawEntityNode(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorScene::RequestEntityRename(EntityId entity) {
    const WorldEntity* target = world_.Find(entity);
    if (target == nullptr) {
        return;
    }
    renameEntity_ = entity;
    selection_ = entity;
    renameBuffer_.fill('\0');
    strncpy_s(renameBuffer_.data(), renameBuffer_.size(), target->name.c_str(), _TRUNCATE);
    showEntityRenameDialog_ = true;
}

void EditorScene::SynchronizeHierarchySelection() {
    std::erase_if(hierarchySelection_, [this](EntityId entity) {
        return !world_.Contains(entity);
    });
    if (!world_.Contains(hierarchySelectionAnchor_)) {
        hierarchySelectionAnchor_ = {};
    }
    if (!world_.Contains(selection_)) {
        selection_ = {};
        hierarchySelection_.clear();
        hierarchySelectionAnchor_ = {};
        return;
    }
    if (!hierarchySelection_.contains(selection_)) {
        hierarchySelection_.clear();
        hierarchySelection_.insert(selection_);
        hierarchySelectionAnchor_ = selection_;
    }
}

void EditorScene::SelectHierarchyEntity(EntityId entity, bool toggle, bool range) {
    const WorldEntity* target = world_.Find(entity);
    if (target == nullptr) {
        return;
    }
    if (range && world_.Contains(hierarchySelectionAnchor_)) {
        const WorldEntity* anchor = world_.Find(hierarchySelectionAnchor_);
        if (anchor != nullptr && anchor->parent == target->parent) {
            const std::vector<EntityId> siblings = target->parent.IsValid()
                                                       ? world_.GetChildren(target->parent)
                                                       : world_.GetRootEntities();
            const auto anchorPosition = std::ranges::find(siblings, hierarchySelectionAnchor_);
            const auto targetPosition = std::ranges::find(siblings, entity);
            if (anchorPosition != siblings.end() && targetPosition != siblings.end()) {
                if (!toggle) {
                    hierarchySelection_.clear();
                }
                auto first = anchorPosition;
                auto last = targetPosition;
                if (last < first) {
                    std::swap(first, last);
                }
                hierarchySelection_.insert(first, std::next(last));
                selection_ = entity;
                return;
            }
        }
    }
    if (toggle) {
        if (hierarchySelection_.contains(entity)) {
            hierarchySelection_.erase(entity);
            if (selection_ == entity) {
                selection_ = {};
                for (const WorldEntity& candidate : world_.Entities()) {
                    if (hierarchySelection_.contains(candidate.id)) {
                        selection_ = candidate.id;
                        break;
                    }
                }
            }
            if (hierarchySelection_.empty()) {
                hierarchySelectionAnchor_ = {};
            }
            return;
        }
        hierarchySelection_.insert(entity);
    } else {
        hierarchySelection_.clear();
        hierarchySelection_.insert(entity);
    }
    selection_ = entity;
    hierarchySelectionAnchor_ = entity;
}

void EditorScene::SelectAllHierarchyEntities() {
    hierarchySelection_.clear();
    for (const WorldEntity& entity : world_.Entities()) {
        hierarchySelection_.insert(entity.id);
    }
    if (!world_.Contains(selection_)) {
        selection_ = world_.Empty() ? EntityId{} : world_.Entities().front().id;
    }
    hierarchySelectionAnchor_ = selection_;
    status_ = hierarchySelection_.empty() ? "There are no entities to select."
                                          : "Selected all entities.";
}

void EditorScene::ClearHierarchySelection() {
    selection_ = {};
    hierarchySelection_.clear();
    hierarchySelectionAnchor_ = {};
    status_ = "Cleared the entity selection.";
}

bool EditorScene::IsHierarchyEntitySelected(EntityId entity) const {
    return hierarchySelection_.contains(entity);
}

std::vector<EntityId> EditorScene::GetTopLevelSelectedEntities() const {
    std::vector<EntityId> roots;
    roots.reserve(hierarchySelection_.size());
    for (const WorldEntity& entity : world_.Entities()) {
        if (!hierarchySelection_.contains(entity.id)) {
            continue;
        }
        bool hasSelectedAncestor = false;
        EntityId ancestor = entity.parent;
        for (size_t depth = 0; ancestor.IsValid() && depth < world_.Entities().size(); ++depth) {
            if (hierarchySelection_.contains(ancestor)) {
                hasSelectedAncestor = true;
                break;
            }
            const WorldEntity* parent = world_.Find(ancestor);
            ancestor = parent != nullptr ? parent->parent : EntityId{};
        }
        if (!hasSelectedAncestor) {
            roots.push_back(entity.id);
        }
    }
    return roots;
}

void EditorScene::SetSelectedEntitiesActive(EntityId source, bool active) {
    if (!world_.Contains(source)) {
        return;
    }
    SynchronizeHierarchySelection();
    std::vector<EntityId> targets;
    if (hierarchySelection_.contains(source)) {
        targets.reserve(hierarchySelection_.size());
        for (const WorldEntity& entity : world_.Entities()) {
            if (hierarchySelection_.contains(entity.id) && entity.active != active) {
                targets.push_back(entity.id);
            }
        }
    } else {
        const WorldEntity* entity = world_.Find(source);
        if (entity != nullptr && entity->active != active) {
            targets.push_back(source);
        }
    }
    if (targets.empty()) {
        return;
    }
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    for (EntityId target : targets) {
        WorldEntity* entity = world_.Find(target);
        if (entity != nullptr) {
            entity->active = active;
        }
    }
    RecordImmediateEdit(active ? "Activate Entities" : "Deactivate Entities", before,
                        selectionBefore);
    if (targets.size() == 1u) {
        status_ = active ? "Activated the Entity." : "Deactivated the Entity.";
    } else {
        status_ = active ? "Activated the selected Entities."
                         : "Deactivated the selected Entities.";
    }
}

bool EditorScene::MoveEntityInHierarchy(EntityId entity, int direction) {
    const WorldEntity* target = world_.Find(entity);
    if (target == nullptr || direction == 0) {
        return false;
    }
    const std::vector<EntityId> siblings =
        target->parent.IsValid() ? world_.GetChildren(target->parent) : world_.GetRootEntities();
    const auto position = std::ranges::find(siblings, entity);
    if (position == siblings.end()) {
        return false;
    }
    EntityId adjacent{};
    if (direction < 0) {
        if (position == siblings.begin()) {
            return false;
        }
        adjacent = *std::prev(position);
    } else {
        const auto next = std::next(position);
        if (next == siblings.end()) {
            return false;
        }
        adjacent = *next;
    }
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const bool moved = direction < 0 ? world_.MoveEntityBefore(entity, adjacent)
                                     : world_.MoveEntityAfter(entity, adjacent);
    if (!moved) {
        return false;
    }
    selection_ = entity;
    RecordImmediateEdit("Reorder Entity", before, selectionBefore);
    status_ = direction < 0 ? "Moved the entity up." : "Moved the entity down.";
    return true;
}

bool EditorScene::MoveSelectionAdjacentTo(EntityId draggedEntity, EntityId sibling, bool after) {
    const WorldEntity* dragged = world_.Find(draggedEntity);
    const WorldEntity* target = world_.Find(sibling);
    if (dragged == nullptr || target == nullptr || draggedEntity == sibling) {
        return false;
    }
    SynchronizeHierarchySelection();
    std::vector<EntityId> roots;
    if (hierarchySelection_.contains(draggedEntity)) {
        roots = GetTopLevelSelectedEntities();
    } else {
        roots.push_back(draggedEntity);
    }
    if (roots.empty() || std::ranges::find(roots, sibling) != roots.end()) {
        return false;
    }
    for (EntityId root : roots) {
        const WorldEntity* entity = world_.Find(root);
        if (entity == nullptr || entity->parent != target->parent) {
            status_ = "Sibling reordering requires entities with the same parent.";
            return false;
        }
    }

    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (after) {
        for (auto iterator = roots.rbegin(); iterator != roots.rend(); ++iterator) {
            world_.MoveEntityAfter(*iterator, sibling);
        }
    } else {
        for (EntityId root : roots) {
            world_.MoveEntityBefore(root, sibling);
        }
    }
    if (WorldSerializer::Serialize(world_) == before) {
        return false;
    }
    selection_ = world_.Contains(draggedEntity) ? draggedEntity : roots.front();
    RecordImmediateEdit(roots.size() == 1u ? "Reorder Entity" : "Reorder Entities", before,
                        selectionBefore);
    status_ = roots.size() == 1u ? "Reordered the entity."
                                 : "Reordered the selected entities.";
    return true;
}

bool EditorScene::CopySelection() {
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    if (roots.empty()) {
        return false;
    }
    const std::unordered_set<EntityId, EntityIdHash> rootIds(roots.begin(), roots.end());
    std::unordered_set<EntityId, EntityIdHash> copiedIds;
    std::vector<EntityId> pending = roots;
    while (!pending.empty()) {
        const EntityId current = pending.back();
        pending.pop_back();
        if (!copiedIds.insert(current).second) {
            continue;
        }
        const std::vector<EntityId> children = world_.GetChildren(current);
        pending.insert(pending.end(), children.begin(), children.end());
    }

    std::vector<WorldEntity> copiedEntities;
    copiedEntities.reserve(copiedIds.size());
    for (const WorldEntity& entity : world_.Entities()) {
        if (!copiedIds.contains(entity.id)) {
            continue;
        }
        copiedEntities.push_back(entity);
        if (rootIds.contains(entity.id)) {
            copiedEntities.back().parent = {};
        }
    }
    World clipboardWorld;
    std::string error;
    if (!clipboardWorld.ReplaceEntities(std::move(copiedEntities), &error)) {
        status_ = "Copy failed: " + error;
        return false;
    }
    entityClipboard_ = WorldSerializer::Serialize(clipboardWorld);
    status_ = roots.size() == 1u ? "Copied the selected entity hierarchy."
                                 : "Copied the selected entity hierarchies.";
    return true;
}

void EditorScene::CutSelection() {
    if (!CopySelection()) {
        return;
    }
    const size_t cutCount = GetTopLevelSelectedEntities().size();
    DeleteSelection();
    status_ = cutCount == 1u ? "Cut the selected entity hierarchy."
                             : "Cut the selected entity hierarchies.";
}

bool EditorScene::PasteEntityClipboard(EntityId parent) {
    if (entityClipboard_.empty() || (parent.IsValid() && !world_.Contains(parent))) {
        return false;
    }
    World clipboardWorld;
    std::string error;
    if (!WorldSerializer::Deserialize(entityClipboard_, clipboardWorld, &error) ||
        clipboardWorld.Empty()) {
        status_ = "Paste failed: " + (error.empty() ? std::string("clipboard is empty.") : error);
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::vector<EntityId> pastedRoots;
    if (!world_.InstantiateEntityHierarchies(clipboardWorld, parent, pastedRoots,
                                              &error)) {
        status_ = "Paste failed: " + error;
        return false;
    }
    for (EntityId root : pastedRoots) {
        if (WorldEntity* pastedRoot = world_.Find(root)) {
            pastedRoot->name += " Copy";
        }
    }
    const size_t rootCount = pastedRoots.size();
    hierarchySelection_.clear();
    hierarchySelection_.insert(pastedRoots.begin(), pastedRoots.end());
    selection_ = pastedRoots.front();
    hierarchySelectionAnchor_ = selection_;
    RecordImmediateEdit(rootCount == 1u ? "Paste Entity Hierarchy" : "Paste Entity Hierarchies",
                        before, selectionBefore);
    if (rootCount == 1u) {
        status_ = parent.IsValid() ? "Pasted the entity hierarchy as a child."
                                   : "Pasted the entity hierarchy.";
    } else {
        status_ = parent.IsValid() ? "Pasted the entity hierarchies as children."
                                   : "Pasted the entity hierarchies.";
    }
    return true;
}

void EditorScene::DuplicateSelection() {
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    if (roots.empty()) {
        return;
    }
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::vector<EntityId> duplicates;
    duplicates.reserve(roots.size());
    for (EntityId root : roots) {
        const EntityId duplicate = world_.DuplicateEntityHierarchy(root);
        if (!duplicate.IsValid()) {
            World restored;
            if (WorldSerializer::Deserialize(before, restored, nullptr)) {
                world_ = std::move(restored);
                world_.SetPhysicsSettings(physicsSettings_);
            }
            selection_ = selectionBefore;
            hierarchySelection_.clear();
            if (world_.Contains(selectionBefore)) {
                hierarchySelection_.insert(selectionBefore);
                hierarchySelectionAnchor_ = selectionBefore;
            } else {
                hierarchySelectionAnchor_ = {};
            }
            status_ = "Could not duplicate the selected entity hierarchies.";
            return;
        }
        duplicates.push_back(duplicate);
    }
    hierarchySelection_.clear();
    hierarchySelection_.insert(duplicates.begin(), duplicates.end());
    selection_ = duplicates.front();
    hierarchySelectionAnchor_ = selection_;
    RecordImmediateEdit(duplicates.size() == 1u ? "Duplicate Entity" : "Duplicate Entities",
                        before, selectionBefore);
    status_ = duplicates.size() == 1u ? "Duplicated the selected entity hierarchy."
                                      : "Duplicated the selected entity hierarchies.";
}

void EditorScene::ReparentSelection(EntityId draggedEntity, EntityId parent) {
    if (!world_.Contains(draggedEntity) || (parent.IsValid() && !world_.Contains(parent))) {
        return;
    }
    SynchronizeHierarchySelection();
    std::vector<EntityId> roots;
    if (hierarchySelection_.contains(draggedEntity)) {
        roots = GetTopLevelSelectedEntities();
    } else {
        roots.push_back(draggedEntity);
    }
    std::erase_if(roots, [this, parent](EntityId entity) {
        const WorldEntity* current = world_.Find(entity);
        return current == nullptr || current->parent == parent;
    });
    if (roots.empty()) {
        return;
    }
    const std::unordered_set<EntityId, EntityIdHash> rootIds(roots.begin(), roots.end());
    for (EntityId ancestor = parent; ancestor.IsValid();) {
        if (rootIds.contains(ancestor)) {
            status_ = "Cannot reparent entities into their own hierarchy.";
            return;
        }
        const WorldEntity* entity = world_.Find(ancestor);
        ancestor = entity != nullptr ? entity->parent : EntityId{};
    }

    DirectX::XMMATRIX inverseParent = DirectX::XMMatrixIdentity();
    if (parent.IsValid()) {
        DirectX::XMFLOAT4X4 parentWorld{};
        if (!world_.TryGetWorldMatrix(parent, parentWorld)) {
            status_ = "Could not read the new parent world transform.";
            return;
        }
        DirectX::XMVECTOR determinant{};
        inverseParent =
            DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&parentWorld));
        const float determinantValue = DirectX::XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
            status_ = "Cannot reparent under a singular transform.";
            return;
        }
    }

    struct ReparentTransform {
        EntityId entity{};
        TransformComponent local{};
    };
    std::vector<ReparentTransform> transforms;
    transforms.reserve(roots.size());
    for (EntityId root : roots) {
        DirectX::XMFLOAT4X4 worldMatrix{};
        TransformComponent local{};
        if (!world_.TryGetWorldMatrix(root, worldMatrix) ||
            !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix) * inverseParent,
                                            local)) {
            status_ = "Could not preserve the selected entities' world transforms.";
            return;
        }
        transforms.push_back({root, local});
    }

    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    for (const ReparentTransform& transform : transforms) {
        if (!world_.SetParent(transform.entity, parent)) {
            World restored;
            if (WorldSerializer::Deserialize(before, restored, nullptr)) {
                world_ = std::move(restored);
                world_.SetPhysicsSettings(physicsSettings_);
            }
            status_ = "Cannot create a cyclic or invalid hierarchy.";
            return;
        }
        WorldEntity* reparented = world_.Find(transform.entity);
        if (reparented == nullptr) {
            World restored;
            if (WorldSerializer::Deserialize(before, restored, nullptr)) {
                world_ = std::move(restored);
                world_.SetPhysicsSettings(physicsSettings_);
            }
            status_ = "A reparented entity no longer exists.";
            return;
        }
        reparented->transform = transform.local;
    }
    selection_ = world_.Contains(draggedEntity) ? draggedEntity : roots.front();
    RecordImmediateEdit(roots.size() == 1u ? "Reparent Entity" : "Reparent Entities", before,
                        selectionBefore);
    if (roots.size() == 1u) {
        status_ = parent.IsValid() ? "Reparented the entity without moving it."
                                   : "Moved the entity to the scene root without moving it.";
    } else {
        status_ = parent.IsValid() ? "Reparented the entities without moving them."
                                   : "Moved the entities to the scene root without moving them.";
    }
}

