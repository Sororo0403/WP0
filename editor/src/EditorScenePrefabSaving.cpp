#include "EditorScene.h"

#include "world/WorldSerializer.h"

bool EditorScene::SaveSelectionAsPrefab() {
    EntityId root{};
    std::filesystem::path destination;
    if (!TryPreparePrefabSave(root, destination)) {
        return false;
    }
    const auto includedIds = CollectPrefabEntityIds(root);
    if (!SavePrefabEntities(BuildPrefabEntities(root, includedIds), destination)) {
        return false;
    }
    RefreshAssetBrowser();
    SelectSavedPrefabAsset(destination);
    status_ = "Saved Prefab: " + destination.string();
    return true;
}

bool EditorScene::TryPreparePrefabSave(EntityId& root, std::filesystem::path& destination) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before creating a Prefab.";
        return false;
    }
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    if (roots.size() != 1u) {
        status_ = "Select exactly one entity hierarchy to create a Prefab.";
        return false;
    }
    const WorldEntity* rootEntity = world_.Find(roots.front());
    if (rootEntity == nullptr) {
        status_ = "The selected entity no longer exists.";
        return false;
    }
    const std::optional<std::filesystem::path> selectedDestination =
        ShowSavePrefabDialog(rootEntity->name);
    if (!selectedDestination) {
        status_ = "Prefab save cancelled.";
        return false;
    }
    root = roots.front();
    destination = *selectedDestination;
    return true;
}

std::unordered_set<EntityId, EntityIdHash> EditorScene::CollectPrefabEntityIds(
    EntityId root) const {
    std::unordered_set<EntityId, EntityIdHash> includedIds;
    includedIds.insert(root);
    for (const WorldEntity& candidate : world_.Entities()) {
        EntityId current = candidate.parent;
        for (size_t depth = 0u; current.IsValid() && depth <= world_.Entities().size(); ++depth) {
            if (current == root) {
                includedIds.insert(candidate.id);
                break;
            }
            const WorldEntity* parent = world_.Find(current);
            current = parent != nullptr ? parent->parent : EntityId{};
        }
    }
    return includedIds;
}

std::vector<WorldEntity> EditorScene::BuildPrefabEntities(
    EntityId root, const std::unordered_set<EntityId, EntityIdHash>& includedIds) const {
    std::vector<WorldEntity> entities;
    entities.reserve(includedIds.size());
    for (const WorldEntity& source : world_.Entities()) {
        if (!includedIds.contains(source.id)) {
            continue;
        }
        WorldEntity prefabEntity = source;
        if (prefabEntity.id == root) {
            prefabEntity.parent = {};
        }
        ClearExternalPrefabEntityReferences(prefabEntity, includedIds);
        entities.push_back(std::move(prefabEntity));
    }
    return entities;
}

void EditorScene::ClearExternalPrefabEntityReferences(
    WorldEntity& entity,
    const std::unordered_set<EntityId, EntityIdHash>& includedIds) const {
    for (BehaviorComponent& script : entity.scripts) {
        for (ScriptPropertyValue& property : script.properties) {
            if (property.type == ScriptPropertyType::Entity && property.entityValue.IsValid() &&
                !includedIds.contains(property.entityValue)) {
                property.entityValue = {};
            }
        }
    }
}

bool EditorScene::SavePrefabEntities(std::vector<WorldEntity> entities,
                                     const std::filesystem::path& destination) {
    World prefab;
    std::string error;
    if (!prefab.ReplaceEntities(std::move(entities), &error) ||
        !WorldSerializer::Save(prefab, destination, &error)) {
        status_ = "Prefab save failed: " + error;
        return false;
    }
    return true;
}

void EditorScene::SelectSavedPrefabAsset(const std::filesystem::path& destination) {
    std::error_code error;
    selectedAsset_ = std::filesystem::relative(destination, assetRoot_, error);
    if (error) {
        selectedAsset_.clear();
    }
}
