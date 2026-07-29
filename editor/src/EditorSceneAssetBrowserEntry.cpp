#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "ScriptAsset.h"
#include "imgui.h"
#include "internal/EditorSceneAssetUtils.h"

#include <Windows.h>
#include <shellapi.h>

using namespace EditorSceneAssetUtils;

EditorScene::AssetBrowserEntryKind EditorScene::ClassifyAssetBrowserEntry(
    const std::filesystem::path& relativePath, bool directory) const {
    if (directory) {
        return AssetBrowserEntryKind::Directory;
    }
    if (IsPrefabAsset(relativePath)) {
        return AssetBrowserEntryKind::Prefab;
    }
    if (AssetImport::IsTextureFile(relativePath)) {
        return AssetBrowserEntryKind::Texture;
    }
    if (AssetImport::IsAudioFile(relativePath)) {
        return AssetBrowserEntryKind::Audio;
    }
    if (AssetImport::IsFontFile(relativePath)) {
        return AssetBrowserEntryKind::Font;
    }
    if (ScriptAssets::IsScriptFile(relativePath)) {
        return AssetBrowserEntryKind::Script;
    }
    if (ScriptAssets::IsScriptSourceFile(relativePath)) {
        return AssetBrowserEntryKind::ScriptHeader;
    }
    return AssetBrowserEntryKind::Model;
}

std::string EditorScene::BuildAssetBrowserEntryLabel(
    const std::filesystem::path& relativePath, AssetBrowserEntryKind kind) const {
    const char* prefix = "[Model] ";
    switch (kind) {
        case AssetBrowserEntryKind::Directory:
            prefix = "[Folder] ";
            break;
        case AssetBrowserEntryKind::Prefab:
            prefix = "[Prefab] ";
            break;
        case AssetBrowserEntryKind::Texture:
            prefix = "[Texture] ";
            break;
        case AssetBrowserEntryKind::Audio:
            prefix = "[Audio] ";
            break;
        case AssetBrowserEntryKind::Font:
            prefix = "[Font] ";
            break;
        case AssetBrowserEntryKind::Script:
            prefix = "[Script] ";
            break;
        case AssetBrowserEntryKind::ScriptHeader:
            prefix = "[C++ Script] ";
            break;
        case AssetBrowserEntryKind::Model:
            break;
    }
    return prefix + relativePath.filename().string();
}

bool EditorScene::OpenAssetScriptSource(const std::filesystem::path& relativePath,
                                        const std::string& logicalId) {
    const std::filesystem::path physical = assetRoot_ / relativePath;
    if (reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", physical.c_str(), nullptr,
                                                physical.parent_path().c_str(),
                                                SW_SHOWNORMAL)) > 32) {
        return true;
    }
    status_ = "Could not open Script source: " + logicalId;
    return false;
}

void EditorScene::ActivateAssetBrowserEntry(const std::filesystem::path& relativePath,
                                            const std::filesystem::path& logicalPath,
                                            AssetBrowserEntryKind kind) {
    if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        return;
    }
    if (kind == AssetBrowserEntryKind::Directory) {
        NavigateAssetBrowser(relativePath);
    } else if (kind == AssetBrowserEntryKind::Prefab) {
        InstantiatePrefabAsset(logicalPath);
    } else if (kind == AssetBrowserEntryKind::Script ||
               kind == AssetBrowserEntryKind::ScriptHeader) {
        OpenAssetScriptSource(relativePath, logicalPath.generic_string());
    }
}

void EditorScene::DrawAssetBrowserEntryDragSource(const std::string& logicalId,
                                                  AssetBrowserEntryKind kind) {
    if (kind == AssetBrowserEntryKind::Directory || kind == AssetBrowserEntryKind::ScriptHeader ||
        !ImGui::BeginDragDropSource()) {
        return;
    }
    const char* payloadType = kModelAssetDragPayload;
    switch (kind) {
        case AssetBrowserEntryKind::Prefab:
            payloadType = kPrefabAssetDragPayload;
            break;
        case AssetBrowserEntryKind::Texture:
            payloadType = kTextureAssetDragPayload;
            break;
        case AssetBrowserEntryKind::Audio:
            payloadType = kAudioAssetDragPayload;
            break;
        case AssetBrowserEntryKind::Font:
            payloadType = kFontAssetDragPayload;
            break;
        case AssetBrowserEntryKind::Script:
            payloadType = kScriptAssetDragPayload;
            break;
        case AssetBrowserEntryKind::Directory:
        case AssetBrowserEntryKind::ScriptHeader:
        case AssetBrowserEntryKind::Model:
            break;
    }
    ImGui::SetDragDropPayload(payloadType, logicalId.c_str(), logicalId.size() + 1u);
    ImGui::TextUnformatted(logicalId.c_str());
    ImGui::EndDragDropSource();
}

void EditorScene::DrawAssetTextureAssignmentMenu(
    const std::filesystem::path& logicalPath) {
    if (!ImGui::BeginMenu("Assign to Selected Material", selection_.IsValid())) {
        return;
    }
    if (ImGui::MenuItem("Base Color")) {
        AssignBaseColorTexture(selection_, logicalPath);
    }
    if (ImGui::MenuItem("Normal Map")) {
        AssignNormalTexture(selection_, logicalPath);
    }
    if (ImGui::MenuItem("Roughness")) {
        AssignRoughnessTexture(selection_, logicalPath);
    }
    if (ImGui::MenuItem("Metallic")) {
        AssignMetallicTexture(selection_, logicalPath);
    }
    ImGui::EndMenu();
}

void EditorScene::DrawAssetBrowserEntryContextMenu(
    const std::filesystem::path& relativePath, const std::filesystem::path& logicalPath,
    AssetBrowserEntryKind kind) {
    if (!ImGui::BeginPopupContextItem("AssetContext")) {
        return;
    }
    selectedAsset_ = relativePath;
    const bool directory = kind == AssetBrowserEntryKind::Directory;
    if (directory) {
        if (ImGui::MenuItem("Open")) {
            NavigateAssetBrowser(relativePath);
        }
    } else if (kind == AssetBrowserEntryKind::ScriptHeader) {
        if (ImGui::MenuItem("Open")) {
            OpenAssetScriptSource(relativePath, logicalPath.generic_string());
        }
    } else if (kind == AssetBrowserEntryKind::Prefab && ImGui::MenuItem("Instantiate")) {
        InstantiatePrefabAsset(logicalPath);
    } else if (kind == AssetBrowserEntryKind::Script &&
               ImGui::MenuItem("Attach to Selected Entity", nullptr, false,
                               selection_.IsValid())) {
        AssignScriptAsset(selection_, logicalPath);
    } else if (kind == AssetBrowserEntryKind::Font) {
        if (ImGui::MenuItem("Assign to Selected Text", nullptr, false, selection_.IsValid())) {
            AssignTextFont(selection_, logicalPath);
        }
    } else if (kind != AssetBrowserEntryKind::Texture && ImGui::MenuItem("Create Entity")) {
        CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
    } else if (kind == AssetBrowserEntryKind::Texture) {
        DrawAssetTextureAssignmentMenu(logicalPath);
    }
    if (ImGui::MenuItem("Rename")) {
        RequestAssetRename(relativePath, directory);
    }
    if (!directory && ImGui::MenuItem("Duplicate")) {
        DuplicateAsset(relativePath);
    }
    if (ImGui::MenuItem("Delete")) {
        RequestAssetDelete(relativePath, directory);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer")) {
        RevealAssetInExplorer(relativePath);
    }
    const size_t references = CountAssetReferences(relativePath, directory);
    if (ImGui::MenuItem("Select Referencing Entities", nullptr, false, references != 0u)) {
        SelectAssetReferences(relativePath, directory);
    }
    if (!directory) {
        ImGui::Separator();
        const std::string uri = "asset://" + relativePath.lexically_normal().generic_string();
        const std::string logicalId = logicalPath.generic_string();
        if (ImGui::MenuItem("Copy Asset URI")) {
            ImGui::SetClipboardText(uri.c_str());
            status_ = "Copied asset URI: " + uri;
        }
        if (ImGui::MenuItem("Copy Project Path")) {
            ImGui::SetClipboardText(logicalId.c_str());
            status_ = "Copied project asset path: " + logicalId;
        }
    }
    ImGui::EndPopup();
}
