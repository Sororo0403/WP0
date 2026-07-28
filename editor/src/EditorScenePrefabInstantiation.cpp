#include "EditorScene.h"

#include "internal/EditorSceneAssetUtils.h"
#include "world/WorldSerializer.h"

using namespace EditorSceneAssetUtils;

bool EditorScene::InstantiatePrefabAsset(const std::filesystem::path& path, EntityId parent,
                                         std::optional<DirectX::XMFLOAT3> position) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before instantiating a Prefab.";
        return false;
    }
    std::filesystem::path resolved;
    World prefab;
    if (!TryResolvePrefabAsset(path, resolved) || !TryLoadPrefabAsset(resolved, prefab)) {
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::vector<EntityId> roots;
    if (!TryInstantiatePrefabWorld(prefab, parent, roots)) {
        return false;
    }
    PositionInstantiatedPrefab(roots, position);
    SelectInstantiatedPrefab(roots);
    RecordImmediateEdit("Instantiate Prefab", before, selectionBefore);
    status_ = "Instantiated Prefab: " + resolved.filename().string();
    return true;
}

bool EditorScene::TryResolvePrefabAsset(const std::filesystem::path& path,
                                        std::filesystem::path& resolved) {
    const std::optional<std::filesystem::path> candidate = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!candidate || !IsPrefabAsset(*candidate) ||
        !std::filesystem::is_regular_file(*candidate, error) || error ||
        !IsPathWithinRoot(assetRoot_, *candidate)) {
        status_ = "The Prefab asset is invalid or outside the project assets directory.";
        return false;
    }
    resolved = *candidate;
    return true;
}

bool EditorScene::TryLoadPrefabAsset(const std::filesystem::path& path, World& prefab) {
    std::string error;
    if (!WorldSerializer::Load(path, prefab, &error)) {
        status_ = "Prefab load failed: " + error;
        return false;
    }
    return true;
}

bool EditorScene::TryInstantiatePrefabWorld(const World& prefab, EntityId parent,
                                            std::vector<EntityId>& roots) {
    std::string error;
    if (!world_.InstantiateEntityHierarchies(prefab, parent, roots, &error) || roots.empty()) {
        status_ = "Prefab instantiate failed: " + error;
        return false;
    }
    return true;
}

void EditorScene::PositionInstantiatedPrefab(
    const std::vector<EntityId>& roots, const std::optional<DirectX::XMFLOAT3>& position) {
    if (!position || roots.size() != 1u) {
        return;
    }
    if (WorldEntity* root = world_.Find(roots.front())) {
        root->transform.position = *position;
    }
}

void EditorScene::SelectInstantiatedPrefab(const std::vector<EntityId>& roots) {
    hierarchySelection_.clear();
    hierarchySelection_.insert(roots.begin(), roots.end());
    selection_ = roots.front();
    hierarchySelectionAnchor_ = selection_;
}
