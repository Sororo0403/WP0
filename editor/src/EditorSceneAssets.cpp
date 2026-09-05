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

#include "internal/EditorSceneAssetUtils.h"

using namespace EditorSceneAssetUtils;

namespace {
bool AssetReferenceMatches(std::string_view reference,
                           const std::filesystem::path& relativePath, bool directory) {
    const std::optional<std::filesystem::path> referenced =
        AssetRelativeFromReference(reference);
    return referenced && AssetPathMatches(*referenced, relativePath, directory);
}

bool MaterialReferencesAsset(const MaterialOverrideComponent* material,
                             const std::filesystem::path& relativePath, bool directory) {
    if (material == nullptr) {
        return false;
    }
    const std::array<std::string_view, 4> references = {
        material->baseColorTexturePath,
        material->normalTexturePath,
        material->roughnessTexturePath,
        material->metallicTexturePath,
    };
    return std::ranges::any_of(references, [&](std::string_view reference) {
        return AssetReferenceMatches(reference, relativePath, directory);
    });
}

bool ScriptsReferenceAsset(const std::vector<BehaviorComponent>& scripts,
                           const std::filesystem::path& relativePath, bool directory) {
    return std::ranges::any_of(scripts, [&](const BehaviorComponent& script) {
        return AssetReferenceMatches(script.scriptAssetPath, relativePath, directory);
    });
}

bool EntityReferencesAsset(const WorldEntity& entity,
                           const std::filesystem::path& relativePath, bool directory) {
    const bool modelMatches =
        entity.meshRenderer && entity.meshRenderer->sourceType == MeshSourceType::Model &&
        AssetReferenceMatches(entity.meshRenderer->modelPath, relativePath, directory);
    return modelMatches ||
           MaterialReferencesAsset(entity.materialOverride ? &*entity.materialOverride : nullptr,
                                   relativePath, directory) ||
           ScriptsReferenceAsset(entity.scripts, relativePath, directory) ||
           (entity.audioSource &&
            AssetReferenceMatches(entity.audioSource->clipPath, relativePath, directory)) ||
           (entity.image &&
            AssetReferenceMatches(entity.image->texturePath, relativePath, directory)) ||
           (entity.text &&
            AssetReferenceMatches(entity.text->fontPath, relativePath, directory));
}
} // namespace

void EditorScene::DrawAssetBrowserEntry(const std::filesystem::path& relativePath,
                                        bool directory) {
    const std::filesystem::path logicalPath =
        (std::filesystem::path("assets") / relativePath).lexically_normal();
    const std::string id = logicalPath.generic_string();
    const AssetBrowserEntryKind kind = ClassifyAssetBrowserEntry(relativePath, directory);
    const std::string label = BuildAssetBrowserEntryLabel(relativePath, kind);
    ImGui::PushID(id.c_str());
    const bool selected = selectedAsset_ == relativePath;
    if (ImGui::Selectable(label.c_str(), selected,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
        selectedAsset_ = relativePath;
        ActivateAssetBrowserEntry(relativePath, logicalPath, kind);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", id.c_str());
    }
    DrawAssetBrowserEntryDragSource(id, kind);
    DrawAssetBrowserEntryContextMenu(relativePath, logicalPath, kind);
    ImGui::PopID();
}

void EditorScene::RequestAssetRename(const std::filesystem::path& relativePath,
                                     bool directory) {
    pendingAssetOperationPath_ = relativePath.lexically_normal();
    pendingAssetOperationIsDirectory_ = directory;
    assetRenameBuffer_.fill('\0');
    const std::string filename = pendingAssetOperationPath_.filename().string();
    strncpy_s(assetRenameBuffer_.data(), assetRenameBuffer_.size(), filename.c_str(), _TRUNCATE);
    showAssetRenameDialog_ = true;
    focusAssetRenameInput_ = true;
}

void EditorScene::RequestAssetDelete(const std::filesystem::path& relativePath,
                                     bool directory) {
    pendingAssetOperationPath_ = relativePath.lexically_normal();
    pendingAssetOperationIsDirectory_ = directory;
    showAssetDeleteDialog_ = true;
}

void EditorScene::RequestCreateAssetFolder() {
    assetFolderNameBuffer_.fill('\0');
    strncpy_s(assetFolderNameBuffer_.data(), assetFolderNameBuffer_.size(), "New Folder",
              _TRUNCATE);
    showCreateAssetFolderDialog_ = true;
    focusAssetFolderNameInput_ = true;
}

void EditorScene::DrawAssetOperationDialogs() {
    if (showAssetRenameDialog_) {
        ImGui::OpenPopup("Rename Asset");
        showAssetRenameDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("assets/%s", pendingAssetOperationPath_.generic_string().c_str());
        if (focusAssetRenameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusAssetRenameInput_ = false;
        }
        ImGui::SetNextItemWidth(360.0f);
        const bool submitted = ImGui::InputText(
            "##AssetName", assetRenameBuffer_.data(), assetRenameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (submitted || ImGui::Button("Rename", {100.0f, 0.0f})) {
            if (RenamePendingAsset()) {
                pendingAssetOperationPath_.clear();
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::SameLine();
            if (cancel || ImGui::Button("Cancel", {100.0f, 0.0f})) {
                pendingAssetOperationPath_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (showAssetDeleteDialog_) {
        ImGui::OpenPopup("Delete Asset");
        showAssetDeleteDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(pendingAssetOperationIsDirectory_
                                   ? "Delete this asset folder and all of its contents?"
                                   : "Delete this asset file?");
        ImGui::TextDisabled("assets/%s", pendingAssetOperationPath_.generic_string().c_str());
        const bool referenced =
            IsAssetReferenced(pendingAssetOperationPath_, pendingAssetOperationIsDirectory_);
        if (referenced) {
            ImGui::TextColored({1.0f, 0.45f, 0.3f, 1.0f},
                               "Cannot delete: the current scene references this asset.");
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Delete", {100.0f, 0.0f}) && DeletePendingAsset()) {
            pendingAssetOperationPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        if (referenced) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f}) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pendingAssetOperationPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showCreateAssetFolderDialog_) {
        ImGui::OpenPopup("Create Asset Folder");
        showCreateAssetFolderDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Create Asset Folder", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::filesystem::path parent =
            (std::filesystem::path("assets") / currentAssetDirectory_).lexically_normal();
        ImGui::TextDisabled("In %s", parent.generic_string().c_str());
        if (focusAssetFolderNameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusAssetFolderNameInput_ = false;
        }
        ImGui::SetNextItemWidth(360.0f);
        const bool submitted = ImGui::InputText(
            "##AssetFolderName", assetFolderNameBuffer_.data(),
            assetFolderNameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (submitted || ImGui::Button("Create", {100.0f, 0.0f})) {
            if (CreatePendingAssetFolder()) {
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::SameLine();
            if (cancel || ImGui::Button("Cancel", {100.0f, 0.0f})) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

bool EditorScene::RenamePendingAsset() {
    const std::filesystem::path oldRelative = pendingAssetOperationPath_.lexically_normal();
    const std::string filename(assetRenameBuffer_.data());
    if (oldRelative.empty() || oldRelative.is_absolute() || HasParentTraversal(oldRelative) ||
        !IsValidAssetFilename(filename)) {
        status_ = "Asset rename rejected an invalid name.";
        return false;
    }
    const std::filesystem::path filenamePath(filename);
    std::string newExtension = filenamePath.extension().string();
    std::string oldExtension = oldRelative.extension().string();
    std::ranges::transform(newExtension, newExtension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(oldExtension, oldExtension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (!pendingAssetOperationIsDirectory_ && newExtension != oldExtension) {
        status_ = "Asset rename cannot change a file extension.";
        return false;
    }
    const std::filesystem::path newRelative =
        (oldRelative.parent_path() / filenamePath).lexically_normal();
    if (newRelative == oldRelative) {
        status_ = "The asset already has that name.";
        return false;
    }
    const std::filesystem::path source = assetRoot_ / oldRelative;
    const std::filesystem::path destination = assetRoot_ / newRelative;
    std::error_code error;
    const bool sourceTypeMatches =
        pendingAssetOperationIsDirectory_ ? std::filesystem::is_directory(source, error)
                                          : std::filesystem::is_regular_file(source, error);
    if (error || !sourceTypeMatches || !IsPathWithinRoot(assetRoot_, source)) {
        status_ = "Asset rename failed because the source no longer exists.";
        return false;
    }
    error.clear();
    if (!IsPathAtOrWithinRoot(assetRoot_, source.parent_path()) ||
        std::filesystem::exists(destination, error) || error) {
        status_ = "Asset rename failed because the destination is invalid or already exists.";
        return false;
    }
    std::filesystem::rename(source, destination, error);
    if (error) {
        status_ = "Asset rename failed: " + error.message();
        return false;
    }
    const size_t updatedReferences =
        UpdateAssetReferences(oldRelative, newRelative, pendingAssetOperationIsDirectory_);
    selectedAsset_ = newRelative;
    loadedModels_.clear();
    animatorModels_.clear();
    RefreshAssetBrowser();
    RefreshDirty();
    status_ = "Renamed asset to assets/" + newRelative.generic_string();
    if (updatedReferences != 0u) {
        status_ += " and updated " + std::to_string(updatedReferences) + " scene reference(s).";
    }
    return true;
}

bool EditorScene::DeletePendingAsset() {
    const std::filesystem::path relative = pendingAssetOperationPath_.lexically_normal();
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        IsAssetReferenced(relative, pendingAssetOperationIsDirectory_)) {
        status_ = "Asset deletion rejected an invalid or referenced path.";
        return false;
    }
    const std::filesystem::path physical = assetRoot_ / relative;
    if (!IsPathWithinRoot(assetRoot_, physical)) {
        status_ = "Asset deletion rejected a path outside the project assets directory.";
        return false;
    }
    std::error_code error;
    const uintmax_t removed = std::filesystem::remove_all(physical, error);
    if (error || removed == 0u) {
        status_ = "Asset deletion failed" +
                  (error ? std::string(": ") + error.message() : std::string("."));
        return false;
    }
    selectedAsset_.clear();
    loadedModels_.clear();
    animatorModels_.clear();
    RefreshAssetBrowser();
    status_ = "Deleted asset: assets/" + relative.generic_string();
    return true;
}

bool EditorScene::DuplicateAsset(const std::filesystem::path& relativePath) {
    const std::filesystem::path relative = relativePath.lexically_normal();
    const std::filesystem::path source = assetRoot_ / relative;
    std::error_code error;
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        !std::filesystem::is_regular_file(source, error) || error ||
        !IsPathWithinRoot(assetRoot_, source)) {
        status_ = "Asset duplication rejected an invalid source.";
        return false;
    }
    const std::string stem = relative.stem().string();
    const std::string extension = relative.extension().string();
    std::filesystem::path duplicateRelative;
    for (size_t copyIndex = 1; copyIndex <= 100u; ++copyIndex) {
        const std::string suffix = copyIndex == 1u ? " Copy" : " Copy (" +
                                                                   std::to_string(copyIndex) + ")";
        duplicateRelative = relative.parent_path() / (stem + suffix + extension);
        if (!std::filesystem::exists(assetRoot_ / duplicateRelative, error) && !error) {
            break;
        }
        duplicateRelative.clear();
        error.clear();
    }
    if (duplicateRelative.empty()) {
        status_ = "Asset duplication could not find an available filename.";
        return false;
    }
    std::filesystem::copy_file(source, assetRoot_ / duplicateRelative,
                               std::filesystem::copy_options::none, error);
    if (error) {
        status_ = "Asset duplication failed: " + error.message();
        return false;
    }
    selectedAsset_ = duplicateRelative;
    RefreshAssetBrowser();
    status_ = "Duplicated asset: assets/" + duplicateRelative.generic_string();
    return true;
}

bool EditorScene::CreatePendingAssetFolder() {
    const std::string folderName(assetFolderNameBuffer_.data());
    if (!IsValidAssetFilename(folderName)) {
        status_ = "Asset folder creation rejected an invalid name.";
        return false;
    }
    const std::filesystem::path parent = assetRoot_ / currentAssetDirectory_;
    const std::filesystem::path destination = parent / std::filesystem::path(folderName);
    if (!IsPathAtOrWithinRoot(assetRoot_, parent)) {
        status_ = "Asset folder creation rejected a path outside the assets directory.";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error) {
        status_ = "Asset folder creation failed because that name already exists.";
        return false;
    }
    if (!std::filesystem::create_directory(destination, error) || error) {
        status_ = "Asset folder creation failed" +
                  (error ? std::string(": ") + error.message() : std::string("."));
        return false;
    }
    selectedAsset_ = (currentAssetDirectory_ / folderName).lexically_normal();
    RefreshAssetBrowser();
    status_ = "Created asset folder: assets/" + selectedAsset_.generic_string();
    return true;
}

bool EditorScene::ImportAssetFiles() {
    const std::vector<std::filesystem::path> selectedFiles = ShowImportAssetDialog();
    if (selectedFiles.empty()) {
        return false;
    }
    return ImportAssetFiles(selectedFiles);
}

bool EditorScene::ImportAssetFiles(
    const std::vector<std::filesystem::path>& selectedFiles) {
    const std::filesystem::path destinationDirectory =
        assetRoot_ / currentAssetDirectory_;
    if (!IsPathAtOrWithinRoot(assetRoot_, destinationDirectory)) {
        status_ = "Asset import rejected an invalid destination.";
        return false;
    }

    std::vector<AssetImport::File> importFiles;
    std::string importError;
    if (!AssetImport::BuildPlan(selectedFiles, importFiles, importError)) {
        status_ = "Asset import stopped: " + importError;
        return false;
    }

    size_t alreadyPresent = 0;
    std::error_code error;
    for (const AssetImport::File& file : importFiles) {
        const std::filesystem::path destination =
            destinationDirectory / file.relativeDestination;
        if (!IsPathAtOrWithinRoot(assetRoot_, destination.parent_path())) {
            status_ = "Asset import rejected a dependency destination outside assets/.";
            return false;
        }
        error.clear();
        if (!std::filesystem::exists(destination, error) && !error) {
            continue;
        }
        if (error || !std::filesystem::is_regular_file(destination, error) || error ||
            !AssetImport::HaveEqualContents(file.source, destination)) {
            status_ = "Asset import stopped because assets/" +
                      (currentAssetDirectory_ / file.relativeDestination).generic_string() +
                      " already exists with different contents.";
            return false;
        }
        ++alreadyPresent;
    }

    std::vector<std::filesystem::path> copiedFiles;
    std::vector<std::filesystem::path> createdDirectories;
    copiedFiles.reserve(importFiles.size());
    for (const AssetImport::File& file : importFiles) {
        const std::filesystem::path destination =
            destinationDirectory / file.relativeDestination;
        error.clear();
        if (std::filesystem::exists(destination, error) && !error) {
            continue;
        }
        const std::filesystem::path parent = destination.parent_path();
        std::vector<std::filesystem::path> missingDirectories;
        for (std::filesystem::path directory = parent;
             directory != destinationDirectory && !directory.empty() &&
             !std::filesystem::exists(directory, error);
             directory = directory.parent_path()) {
            if (error) {
                break;
            }
            missingDirectories.push_back(directory);
        }
        for (auto directory = missingDirectories.rbegin();
             !error && directory != missingDirectories.rend(); ++directory) {
            if (std::filesystem::create_directory(*directory, error)) {
                createdDirectories.push_back(*directory);
            }
        }
        if (error) {
            for (const std::filesystem::path& copied : copiedFiles) {
                std::error_code rollbackError;
                std::filesystem::remove(copied, rollbackError);
            }
            for (auto directory = createdDirectories.rbegin();
                 directory != createdDirectories.rend(); ++directory) {
                std::error_code rollbackError;
                std::filesystem::remove(*directory, rollbackError);
            }
            status_ = "Asset import failed and was rolled back: " + error.message();
            RefreshAssetBrowser();
            return false;
        }
        error.clear();
        std::filesystem::copy_file(file.source, destination, std::filesystem::copy_options::none,
                                   error);
        if (error) {
            for (const std::filesystem::path& copied : copiedFiles) {
                std::error_code rollbackError;
                std::filesystem::remove(copied, rollbackError);
            }
            for (auto directory = createdDirectories.rbegin();
                 directory != createdDirectories.rend(); ++directory) {
                std::error_code rollbackError;
                std::filesystem::remove(*directory, rollbackError);
            }
            status_ = "Asset import failed and was rolled back: " + error.message();
            RefreshAssetBrowser();
            return false;
        }
        copiedFiles.push_back(destination);
    }

    selectedAsset_ =
        (currentAssetDirectory_ / selectedFiles.front().filename()).lexically_normal();
    RefreshAssetBrowser();
    status_ = "Imported " + std::to_string(copiedFiles.size()) +
              " new asset file(s) into assets/" + currentAssetDirectory_.generic_string();
    if (alreadyPresent != 0u) {
        status_ += " (kept " + std::to_string(alreadyPresent) + " identical file(s)).";
    }
    return true;
}

bool EditorScene::RevealAssetInExplorer(const std::filesystem::path& relativePath) {
    const std::filesystem::path relative = relativePath.lexically_normal();
    const std::filesystem::path physical = assetRoot_ / relative;
    std::error_code error;
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        !std::filesystem::exists(physical, error) || error ||
        !IsPathWithinRoot(assetRoot_, physical)) {
        status_ = "Could not reveal an invalid or missing asset path.";
        return false;
    }
    HINSTANCE result = nullptr;
    if (std::filesystem::is_directory(physical, error) && !error) {
        result = ShellExecuteW(nullptr, L"open", physical.c_str(), nullptr, nullptr,
                               SW_SHOWNORMAL);
    } else {
        const std::wstring arguments = L"/select,\"" + physical.wstring() + L"\"";
        result = ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                               SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        status_ = "Could not open the asset location in Explorer.";
        return false;
    }
    status_ = "Opened asset location: assets/" + relative.generic_string();
    return true;
}

void EditorScene::SelectAssetReferences(const std::filesystem::path& relativePath,
                                        bool directory) {
    hierarchySelection_.clear();
    selection_ = {};
    for (const WorldEntity& entity : world_.Entities()) {
        if (!EntityReferencesAsset(entity, relativePath, directory)) {
            continue;
        }
        hierarchySelection_.insert(entity.id);
        if (!selection_.IsValid()) {
            selection_ = entity.id;
        }
    }
    hierarchySelectionAnchor_ = selection_;
    showHierarchyPanel_ = true;
    status_ = hierarchySelection_.empty()
                  ? "No scene entities reference the selected asset."
                  : "Selected " + std::to_string(hierarchySelection_.size()) +
                        " entity reference(s) to assets/" +
                        relativePath.lexically_normal().generic_string();
}

bool EditorScene::IsAssetReferenced(const std::filesystem::path& relativePath,
                                    bool directory) const {
    return CountAssetReferences(relativePath, directory) != 0u;
}

size_t EditorScene::CountAssetReferences(const std::filesystem::path& relativePath,
                                         bool directory) const {
    return static_cast<size_t>(std::ranges::count_if(
        world_.Entities(), [&](const WorldEntity& entity) {
            return EntityReferencesAsset(entity, relativePath, directory);
        }));
}

size_t EditorScene::UpdateAssetReferences(const std::filesystem::path& oldRelativePath,
                                          const std::filesystem::path& newRelativePath,
                                          bool directory) {
    size_t updated = 0;
    for (const WorldEntity& candidate : world_.Entities()) {
        WorldEntity* entity = world_.Find(candidate.id);
        if (entity == nullptr) {
            continue;
        }
        auto updateReference = [&](std::string& reference) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(reference);
            if (!referenced || !AssetPathMatches(*referenced, oldRelativePath, directory)) {
                return;
            }
            const std::filesystem::path suffix = referenced->lexically_relative(oldRelativePath);
            const std::filesystem::path replacement =
                suffix.empty() || suffix == L"." ? newRelativePath : newRelativePath / suffix;
            reference = "asset://" + replacement.lexically_normal().generic_string();
            ++updated;
        };
        if (entity->meshRenderer && entity->meshRenderer->sourceType == MeshSourceType::Model) {
            updateReference(entity->meshRenderer->modelPath);
        }
        if (entity->materialOverride) {
            updateReference(entity->materialOverride->baseColorTexturePath);
            updateReference(entity->materialOverride->normalTexturePath);
            updateReference(entity->materialOverride->roughnessTexturePath);
            updateReference(entity->materialOverride->metallicTexturePath);
        }
        if (entity->audioSource) {
            updateReference(entity->audioSource->clipPath);
        }
        if (entity->image) {
            updateReference(entity->image->texturePath);
        }
        if (entity->text) {
            updateReference(entity->text->fontPath);
        }
        for (BehaviorComponent& script : entity->scripts) {
            updateReference(script.scriptAssetPath);
        }
    }
    return updated;
}

