#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "ScriptAsset.h"
#include "internal/EditorSceneAssetUtils.h"

#include <algorithm>
#include <ranges>

using namespace EditorSceneAssetUtils;

namespace {

void SortAssetPaths(std::vector<std::filesystem::path>& paths) {
    std::ranges::sort(paths, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
}

}  // namespace

void EditorScene::RefreshAssetBrowser() {
    ResetAssetBrowserCache();
    RefreshSceneAssetList();
    std::error_code error;
    if (!std::filesystem::is_directory(assetRoot_, error) || error) {
        return;
    }
    const std::filesystem::path currentDirectory = ResolveAssetBrowserDirectory();
    RefreshAssetBrowserEntries(currentDirectory);
    RefreshProjectAssetLists();
}

void EditorScene::ResetAssetBrowserCache() {
    assetPreviewAsset_.clear();
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    modelAssets_.clear();
    textureAssets_.clear();
    audioAssets_.clear();
    fontAssets_.clear();
    scriptAssets_.clear();
    prefabAssets_.clear();
    sceneAssets_.clear();
    assetBrowserEntries_.clear();
}

void EditorScene::RefreshSceneAssetList() {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        sceneRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error &&
            LowercaseAscii(iterator->path().extension().string()) == ".likescene") {
            std::filesystem::path relative =
                std::filesystem::relative(iterator->path(), sceneRoot_, error);
            if (!error) {
                sceneAssets_.push_back(relative.lexically_normal());
            }
        }
        iterator.increment(error);
    }
    SortAssetPaths(sceneAssets_);
}

std::filesystem::path EditorScene::ResolveAssetBrowserDirectory() {
    std::filesystem::path currentDirectory =
        (assetRoot_ / currentAssetDirectory_).lexically_normal();
    std::error_code error;
    if ((!currentAssetDirectory_.empty() && !IsPathWithinRoot(assetRoot_, currentDirectory)) ||
        !std::filesystem::is_directory(currentDirectory, error) || error) {
        currentAssetDirectory_.clear();
        currentDirectory = assetRoot_;
    }
    return currentDirectory;
}

void EditorScene::RefreshAssetBrowserEntries(const std::filesystem::path& currentDirectory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        currentDirectory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        const bool directory = entry.is_directory(error);
        const bool supportedFile =
            !error && entry.is_regular_file(error) && !error &&
            (AssetImport::IsModelFile(entry.path()) || AssetImport::IsTextureFile(entry.path()) ||
             AssetImport::IsAudioFile(entry.path()) || AssetImport::IsFontFile(entry.path()) ||
             IsPrefabAsset(entry.path()) || ScriptAssets::IsScriptFile(entry.path()) ||
             ScriptAssets::IsScriptSourceFile(entry.path()));
        if (!error &&
            ((directory && IsPathWithinRoot(assetRoot_, entry.path())) || supportedFile)) {
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), assetRoot_, error);
            if (!error) {
                assetBrowserEntries_.push_back({relative.lexically_normal(), directory});
            }
        }
        error.clear();
        iterator.increment(error);
    }
    std::ranges::sort(
        assetBrowserEntries_, [](const AssetBrowserEntry& left, const AssetBrowserEntry& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            return left.relativePath.generic_string() < right.relativePath.generic_string();
        });
}

void EditorScene::RefreshProjectAssetLists() {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        assetRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error) {
            const std::filesystem::path& physicalPath = iterator->path();
            const bool supported =
                AssetImport::IsModelFile(physicalPath) ||
                AssetImport::IsTextureFile(physicalPath) ||
                AssetImport::IsAudioFile(physicalPath) || AssetImport::IsFontFile(physicalPath) ||
                IsPrefabAsset(physicalPath) || ScriptAssets::IsScriptFile(physicalPath);
            if (supported) {
                std::filesystem::path relative =
                    std::filesystem::relative(physicalPath, assetRoot_, error);
                if (!error) {
                    AddProjectAsset(physicalPath, relative);
                }
            }
        }
        iterator.increment(error);
    }
    SortAssetPaths(modelAssets_);
    SortAssetPaths(textureAssets_);
    SortAssetPaths(audioAssets_);
    SortAssetPaths(fontAssets_);
    SortAssetPaths(scriptAssets_);
    SortAssetPaths(prefabAssets_);
}

void EditorScene::AddProjectAsset(const std::filesystem::path& physicalPath,
                                  const std::filesystem::path& relativePath) {
    auto& assets = IsPrefabAsset(physicalPath)                ? prefabAssets_
                   : AssetImport::IsTextureFile(physicalPath) ? textureAssets_
                   : AssetImport::IsAudioFile(physicalPath)   ? audioAssets_
                   : AssetImport::IsFontFile(physicalPath)    ? fontAssets_
                   : ScriptAssets::IsScriptFile(physicalPath) ? scriptAssets_
                                                              : modelAssets_;
    assets.push_back((std::filesystem::path("assets") / relativePath).lexically_normal());
}

void EditorScene::NavigateAssetBrowser(const std::filesystem::path& relativeDirectory) {
    const std::filesystem::path normalized = relativeDirectory.lexically_normal();
    if (normalized.is_absolute() || normalized.has_root_name() || normalized.has_root_directory() ||
        HasParentTraversal(normalized)) {
        status_ = "Asset Browser rejected an invalid directory.";
        return;
    }
    const std::filesystem::path physical =
        normalized == L"." ? assetRoot_ : assetRoot_ / normalized;
    std::error_code error;
    if (!std::filesystem::is_directory(physical, error) || error ||
        (normalized != L"." && !normalized.empty() && !IsPathWithinRoot(assetRoot_, physical))) {
        status_ = "Asset Browser folder no longer exists.";
        return;
    }
    pendingAssetDirectory_ = normalized == L"." ? std::filesystem::path{} : normalized;
}
