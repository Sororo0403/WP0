#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "ScriptAsset.h"
#include "imgui.h"
#include "internal/EditorSceneAssetUtils.h"

#include <system_error>

using namespace EditorSceneAssetUtils;

void EditorScene::DrawSelectedAssetDetails() {
    ImGui::SeparatorText("Selected Asset");
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    const std::filesystem::path physical = assetRoot_ / relative;
    const std::string logicalPath =
        (std::filesystem::path("assets") / relative).lexically_normal().generic_string();
    ImGui::TextWrapped("%s", logicalPath.c_str());

    bool directory = false;
    bool regularFile = false;
    if (!InspectSelectedAssetPath(physical, directory, regularFile)) {
        return;
    }
    DrawSelectedAssetMetadata(physical, directory, regularFile);
    const size_t references = CountAssetReferences(relative, directory);
    ImGui::TextDisabled("Scene references: %zu", references);
    DrawSelectedAssetActions(relative, directory, references);
    DrawSelectedAssetPreview(relative, physical, logicalPath, regularFile);
}

bool EditorScene::InspectSelectedAssetPath(const std::filesystem::path& physical,
                                           bool& directory, bool& regularFile) const {
    std::error_code error;
    directory = std::filesystem::is_directory(physical, error) && !error;
    error.clear();
    regularFile = std::filesystem::is_regular_file(physical, error) && !error;
    if (directory || regularFile) {
        return true;
    }
    ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Asset no longer exists.");
    return false;
}

std::string EditorScene::BuildSelectedAssetTypeLabel(
    const std::filesystem::path& physical, const bool directory,
    const bool regularFile) {
    std::string label = SelectedAssetKindLabel(physical, directory);
    const std::string extension = physical.extension().string();
    if (regularFile && !extension.empty()) {
        label += " (" + extension + ")";
    }
    return label;
}

const char* EditorScene::SelectedAssetKindLabel(const std::filesystem::path& physical,
                                                const bool directory) {
    return directory                            ? "Folder"
           : IsPrefabAsset(physical)            ? "Prefab"
           : AssetImport::IsTextureFile(physical) ? "Texture"
           : AssetImport::IsAudioFile(physical) ? "Audio"
           : AssetImport::IsFontFile(physical)  ? "Font"
           : ScriptAssets::IsScriptFile(physical) ? "Script"
           : ScriptAssets::IsScriptSourceFile(physical) ? "C++ Script Source"
                                                        : "Model";
}

void EditorScene::DrawSelectedAssetMetadata(const std::filesystem::path& physical,
                                            const bool directory,
                                            const bool regularFile) const {
    const std::string typeLabel =
        BuildSelectedAssetTypeLabel(physical, directory, regularFile);
    ImGui::TextDisabled("Type: %s", typeLabel.c_str());
    if (regularFile) {
        DrawSelectedAssetFileSize(physical);
    }
}

void EditorScene::DrawSelectedAssetFileSize(const std::filesystem::path& physical) {
    std::error_code error;
    const uintmax_t bytes = std::filesystem::file_size(physical, error);
    if (error) {
        return;
    }
    constexpr double kilobyte = 1024.0;
    constexpr double megabyte = kilobyte * 1024.0;
    ImGui::SameLine();
    if (bytes >= static_cast<uintmax_t>(megabyte)) {
        ImGui::TextDisabled("Size: %.2f MB", static_cast<double>(bytes) / megabyte);
    } else {
        ImGui::TextDisabled("Size: %.1f KB", static_cast<double>(bytes) / kilobyte);
    }
}

void EditorScene::DrawSelectedAssetActions(const std::filesystem::path& relative,
                                           const bool directory,
                                           const size_t references) {
    if (ImGui::SmallButton("Show in Explorer")) {
        RevealAssetInExplorer(relative);
    }
    ImGui::SameLine();
    if (references == 0u) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("Select References")) {
        SelectAssetReferences(relative, directory);
    }
    if (references == 0u) {
        ImGui::EndDisabled();
    }
}

void EditorScene::DrawSelectedAssetPreview(const std::filesystem::path& relative,
                                           const std::filesystem::path& physical,
                                           const std::string& logicalPath,
                                           const bool regularFile) {
    if (regularFile && AssetImport::IsAudioFile(physical)) {
        DrawAudioAssetPreview(physical);
        return;
    }
    if (regularFile && AssetImport::IsModelFile(physical)) {
        DrawSelectedModelDetails(relative, logicalPath);
    }
}

void EditorScene::DrawSelectedModelDetails(const std::filesystem::path& relative,
                                           const std::string& logicalPath) {
    if (!DrawSelectedModelDependencyStatus(relative)) {
        return;
    }
    const size_t dependencyCount =
        assetPreviewPlan_.empty() ? 0u : assetPreviewPlan_.size() - 1u;
    ImGui::TextDisabled("Dependencies: %zu", dependencyCount);
    DrawSelectedModelActions(logicalPath, dependencyCount);
    DrawAssetDependenciesPopup(relative);
    DrawAssetPreviewPopup();
}

bool EditorScene::DrawSelectedModelDependencyStatus(
    const std::filesystem::path& relative) {
    if (assetPreviewAsset_ != relative) {
        ImGui::TextDisabled("Dependencies: Analyzing...");
        return false;
    }
    if (assetPreviewError_.empty()) {
        return true;
    }
    ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Dependencies: Invalid");
    ImGui::SameLine();
    if (ImGui::SmallButton("Details##AssetDependencyError")) {
        status_ = assetPreviewError_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", assetPreviewError_.c_str());
    }
    return false;
}

void EditorScene::DrawSelectedModelActions(const std::string& logicalPath,
                                           const size_t dependencyCount) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Create Entity##SelectedAsset")) {
        CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a model entity at the scene origin");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Preview##SelectedAsset")) {
        ImGui::OpenPopup("Model Preview");
    }
    if (dependencyCount != 0u) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show##AssetDependencies")) {
            ImGui::OpenPopup("AssetDependencies");
        }
    }
}

void EditorScene::DrawAssetDependenciesPopup(const std::filesystem::path& relative) {
    if (!ImGui::BeginPopup("AssetDependencies")) {
        return;
    }
    for (size_t index = 1; index < assetPreviewPlan_.size(); ++index) {
        const std::string dependency =
            (std::filesystem::path("assets") / relative.parent_path() /
             assetPreviewPlan_[index].relativeDestination)
                .lexically_normal()
                .generic_string();
        ImGui::BulletText("%s", dependency.c_str());
    }
    ImGui::EndPopup();
}
