#include "EditorScene.h"

#include "core/AssetManager.h"
#include "graphics/DirectXCommon.h"
#include "graphics/RenderScene.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoCollapse;

constexpr const char* kPrimitiveNames[] = {"Box", "Sphere", "Plane", "Cylinder"};
constexpr const char* kEntityDragPayload = "EDITOR_ENTITY";
constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
constexpr size_t kMaxHistoryEntries = 128;
constexpr size_t kMaxRecentScenes = 10;

bool IsModelAsset(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view extensions[] = {".fbx", ".obj", ".gltf", ".glb",
                                                ".dae", ".3ds", ".ply"};
    return std::ranges::find(extensions, extension) != std::end(extensions);
}

bool ContainsCaseInsensitive(std::string value, std::string query) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find(query) != std::string::npos;
}

bool HasParentTraversal(const std::filesystem::path& path) {
    return std::ranges::any_of(path, [](const std::filesystem::path& part) {
        return part == L"..";
    });
}

bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(canonicalPath, canonicalRoot, error);
    return !error && !relative.empty() && !relative.is_absolute() &&
           !HasParentTraversal(relative);
}

Transform DecomposeTransform(const DirectX::XMFLOAT4X4& matrix) {
    using namespace DirectX;
    XMVECTOR scale;
    XMVECTOR rotation;
    XMVECTOR translation;
    Transform result{};
    if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&matrix))) {
        XMStoreFloat3(&result.scale, scale);
        XMStoreFloat4(&result.rotation, rotation);
        XMStoreFloat3(&result.position, translation);
    }
    return result;
}

bool TryGetModelBounds(const Model& model, DirectX::XMFLOAT3& boundsMin,
                       DirectX::XMFLOAT3& boundsMax) {
    bool found = false;
    for (const ModelSubMesh& subMesh : model.subMeshes) {
        if (subMesh.vertexCount == 0u) {
            continue;
        }
        if (!found) {
            boundsMin = subMesh.sourceBoundsMin;
            boundsMax = subMesh.sourceBoundsMax;
            found = true;
            continue;
        }
        boundsMin.x = (std::min)(boundsMin.x, subMesh.sourceBoundsMin.x);
        boundsMin.y = (std::min)(boundsMin.y, subMesh.sourceBoundsMin.y);
        boundsMin.z = (std::min)(boundsMin.z, subMesh.sourceBoundsMin.z);
        boundsMax.x = (std::max)(boundsMax.x, subMesh.sourceBoundsMax.x);
        boundsMax.y = (std::max)(boundsMax.y, subMesh.sourceBoundsMax.y);
        boundsMax.z = (std::max)(boundsMax.z, subMesh.sourceBoundsMax.z);
    }
    return found;
}

bool IntersectRayBounds(DirectX::FXMVECTOR rayOrigin, DirectX::FXMVECTOR rayDirection,
                        const DirectX::XMFLOAT3& boundsMin,
                        const DirectX::XMFLOAT3& boundsMax, float& distance) {
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);

    const float extent = (std::max)({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y,
                                     boundsMax.z - boundsMin.z});
    const float padding = (std::max)(0.01f, extent * 0.005f);
    const float minimum[3] = {boundsMin.x - padding, boundsMin.y - padding,
                              boundsMin.z - padding};
    const float maximum[3] = {boundsMax.x + padding, boundsMax.y + padding,
                              boundsMax.z + padding};
    const float rayOriginValues[3] = {origin.x, origin.y, origin.z};
    const float rayDirectionValues[3] = {direction.x, direction.y, direction.z};
    float entry = 0.0f;
    float exit = (std::numeric_limits<float>::max)();
    for (size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(rayDirectionValues[axis]) < 1.0e-7f) {
            if (rayOriginValues[axis] < minimum[axis] ||
                rayOriginValues[axis] > maximum[axis]) {
                return false;
            }
            continue;
        }
        float nearDistance =
            (minimum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        float farDistance =
            (maximum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
        }
        entry = (std::max)(entry, nearDistance);
        exit = (std::min)(exit, farDistance);
        if (entry > exit) {
            return false;
        }
    }
    distance = entry;
    return exit >= 0.0f;
}
}

EditorScene::EditorScene(std::filesystem::path projectRoot, std::filesystem::path assetRoot,
                         std::filesystem::path sceneRoot,
                         std::filesystem::path startupScene,
                         std::filesystem::path recentScenesPath,
                         std::function<void()> requestClose)
    : requestClose_(std::move(requestClose)), projectRoot_(std::move(projectRoot)),
      assetRoot_(std::move(assetRoot)), sceneRoot_(std::move(sceneRoot)),
      recentScenesStore_(std::move(recentScenesPath), sceneRoot_),
      scenePath_(std::move(startupScene)) {
    recentScenePaths_ = recentScenesStore_.Load();
    std::error_code error;
    if (std::filesystem::is_regular_file(scenePath_, error) && !error) {
        if (!LoadScene(scenePath_)) {
            NewScene(false);
        }
    } else {
        NewScene(false);
    }
    ClearHistory(true);
}

void EditorScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    if (ctx.rendering.dxCommon == nullptr || ctx.rendering.srv == nullptr ||
        !sceneViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Scene View RenderSurface initialization failed.";
        return;
    }
    if (ctx.rendering.model == nullptr || ctx.rendering.meshRenderer == nullptr ||
        ctx.rendering.texture == nullptr) {
        status_ = "Scene View rendering services are unavailable.";
        return;
    }
    sceneRenderer_.Initialize(ctx.rendering.meshRenderer);
    Material material{};
    material.enableTexture = 0;
    const uint32_t whiteTexture = ctx.rendering.texture->GetWhiteTextureId();
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Box)] =
        ctx.rendering.model->CreateBoxHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Sphere)] =
        ctx.rendering.model->CreateSphereHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)] =
        ctx.rendering.model->CreatePlaneHandle(whiteTexture, material);
    primitiveModels_[static_cast<size_t>(MeshPrimitive::Cylinder)] =
        ctx.rendering.model->CreateCylinderHandle(whiteTexture, material);
    sceneViewCamera_.SetPosition({0.0f, 0.35f, -4.0f});
    sceneViewCamera_.SetRotation({0.08f, 0.0f, 0.0f});
    sceneViewCamera_.Initialize(960.0f / 540.0f);
    RefreshAssetBrowser();
    ResolveMeshResources();
}

void EditorScene::Update() {
    ResolveMeshResources();
    if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedSceneWidth_ != sceneViewSurface_.GetWidth() ||
         requestedSceneHeight_ != sceneViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedSceneWidth_);
        const int height = (std::max)(1, requestedSceneHeight_);
        if (sceneViewSurface_.Resize(width, height) &&
            sceneViewPostProcess_.Resize(width, height)) {
            sceneViewCamera_.SetAspect(static_cast<float>(width) / static_cast<float>(height));
        } else {
            status_ = "Scene View resize failed.";
        }
    }
    if (!postProcessInitializationAttempted_ && sceneViewSurface_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        postProcessInitializationAttempted_ = true;
        sceneViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                         sceneViewSurface_.GetWidth(),
                                         sceneViewSurface_.GetHeight());
        if (!sceneViewPostProcess_.IsReady()) {
            status_ = "Scene View PostProcess initialization failed.";
        }
    }
}

void EditorScene::Draw() {}

void EditorScene::DrawPostProcessOverlay() {
    HandleEditorShortcuts();
    DrawMainMenu();
    DrawUnsavedChangesDialog();
    DrawPanels();
}

bool EditorScene::OnCloseRequested() {
    if (!dirty_) {
        return true;
    }
    RequestSceneAction(PendingSceneAction::Exit);
    return false;
}

void EditorScene::DrawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
            RequestSceneAction(PendingSceneAction::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) {
            RequestSceneAction(PendingSceneAction::OpenScene);
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            SaveScene();
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) {
            SaveSceneAs();
        }
        if (ImGui::BeginMenu("Recent Scenes", !recentScenePaths_.empty())) {
            for (const std::filesystem::path& path : recentScenePaths_) {
                std::error_code error;
                std::filesystem::path label =
                    std::filesystem::relative(path, sceneRoot_, error);
                if (error) {
                    label = path.filename();
                }
                const std::string text = label.generic_string();
                if (ImGui::MenuItem(text.c_str())) {
                    RequestSceneAction(PendingSceneAction::OpenScene, path);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reload Scene")) {
            RequestSceneAction(PendingSceneAction::ReloadScene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            RequestSceneAction(PendingSceneAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool canUndo = !undoHistory_.empty();
        const bool canRedo = !redoHistory_.empty();
        const bool canDuplicate = world_.Contains(selection_);
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
            Undo();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
            Redo();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate)) {
            DuplicateSelection();
        }
        ImGui::EndMenu();
    }
    std::string editorLabel = "LikeEngine Editor - ";
    editorLabel += scenePath_.empty() ? "Untitled" : scenePath_.filename().string();
    if (dirty_) {
        editorLabel += " *";
    }
    ImGui::TextUnformatted(editorLabel.c_str());
    ImGui::EndMainMenuBar();
}

void EditorScene::DrawUnsavedChangesDialog() {
    if (showUnsavedChangesDialog_) {
        ImGui::OpenPopup("Unsaved Changes");
        showUnsavedChangesDialog_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("The current scene has unsaved changes.");
    ImGui::TextUnformatted("Save before continuing?");
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(100.0f, 0.0f))) {
        if (SaveScene()) {
            const PendingSceneAction action = pendingSceneAction_;
            const std::filesystem::path path = pendingScenePath_;
            pendingSceneAction_ = PendingSceneAction::None;
            pendingScenePath_.clear();
            ImGui::CloseCurrentPopup();
            ExecuteSceneAction(action, path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(100.0f, 0.0f))) {
        const PendingSceneAction action = pendingSceneAction_;
        const std::filesystem::path path = pendingScenePath_;
        pendingSceneAction_ = PendingSceneAction::None;
        pendingScenePath_.clear();
        ImGui::CloseCurrentPopup();
        ExecuteSceneAction(action, path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        pendingSceneAction_ = PendingSceneAction::None;
        pendingScenePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorScene::BeginFixedPanel(const char* name, float x, float y, float width,
                                  float height) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin(name, nullptr, kPanelFlags);
}

void EditorScene::DrawPanels() {
    sceneViewSurface_.ReleaseCompletedFrameResources();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    const float leftWidth = size.x * 0.22f;
    const float rightWidth = size.x * 0.24f;
    const float centerWidth = size.x - leftWidth - rightWidth;
    const float bottomHeight = size.y * 0.28f;
    const float upperHeight = size.y - bottomHeight;

    BeginFixedPanel("Hierarchy", origin.x, origin.y, leftWidth, upperHeight);
    DrawHierarchyPanel();
    ImGui::End();

    BeginFixedPanel("Project", origin.x, origin.y + upperHeight, leftWidth, bottomHeight);
    DrawProjectPanel();
    ImGui::End();

    BeginFixedPanel("Scene", origin.x + leftWidth, origin.y, centerWidth, upperHeight);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    requestedSceneWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
    requestedSceneHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
    if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr) {
        BuildRenderScene();
        sceneRenderer_.Render(renderScene_, sceneViewCamera_, sceneViewSurface_,
                              {0.025f, 0.035f, 0.055f, 1.0f});
        sceneViewSurface_.TransitionDepthToShaderResource();
        sceneViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
        const PostProcessOutputTarget target{
            sceneViewSurface_.GetOutputRtvHandle(),
            static_cast<uint32_t>(sceneViewSurface_.GetWidth()),
            static_cast<uint32_t>(sceneViewSurface_.GetHeight()),
            DirectXCommon::kBackBufferFormat,
        };
        sceneViewPostProcess_.DrawToTarget(sceneViewSurface_.GetSceneColorGpuHandle(),
                                           sceneViewSurface_.GetDepthGpuHandle(), target);
        sceneViewSurface_.EndOutputPass();
        sceneViewSurface_.TransitionDepthToWrite();
        ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
        const D3D12_GPU_DESCRIPTOR_HANDLE output = sceneViewSurface_.GetOutputGpuHandle();
        ImGui::Image(static_cast<ImTextureID>(output.ptr),
                     ImVec2(static_cast<float>(requestedSceneWidth_),
                            static_cast<float>(requestedSceneHeight_)));
        PickSceneEntity(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    } else {
        ImGui::TextDisabled("Scene View RenderSurface is not ready.");
    }
    ImGui::End();

    BeginFixedPanel("Console", origin.x + leftWidth, origin.y + upperHeight, centerWidth,
                    bottomHeight);
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();

    BeginFixedPanel("Inspector", origin.x + leftWidth + centerWidth, origin.y, rightWidth,
                    size.y);
    DrawInspectorPanel();
    ImGui::End();
}

void EditorScene::DrawProjectPanel() {
    if (pendingAssetDirectory_) {
        currentAssetDirectory_ = std::move(*pendingAssetDirectory_);
        pendingAssetDirectory_.reset();
        selectedAsset_.clear();
        RefreshAssetBrowser();
    }
    if (ImGui::Button("Refresh")) {
        RefreshAssetBrowser();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu model(s)", modelAssets_.size());
    ImGui::Separator();
    if (!currentAssetDirectory_.empty()) {
        if (ImGui::Button("< Back")) {
            NavigateAssetBrowser(currentAssetDirectory_.parent_path());
        }
        ImGui::SameLine();
    }
    DrawAssetBrowserBreadcrumbs();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetSearch", "Search model assets...", assetSearch_.data(),
                             assetSearch_.size());
    ImGui::Separator();

    const std::string search(assetSearch_.data());
    if (!search.empty()) {
        bool found = false;
        for (const std::filesystem::path& logicalPath : modelAssets_) {
            if (ContainsCaseInsensitive(logicalPath.generic_string(), search)) {
                DrawAssetBrowserEntry(logicalPath.lexically_relative("assets"), false);
                found = true;
            }
        }
        if (!found) {
            ImGui::TextDisabled("No matching model assets.");
        }
        return;
    }

    if (assetBrowserEntries_.empty()) {
        ImGui::TextDisabled("This folder contains no model assets or folders.");
        return;
    }
    for (const AssetBrowserEntry& entry : assetBrowserEntries_) {
        DrawAssetBrowserEntry(entry.relativePath, entry.directory);
    }
}

void EditorScene::DrawAssetBrowserBreadcrumbs() {
    if (ImGui::SmallButton("assets")) {
        NavigateAssetBrowser({});
    }
    std::filesystem::path accumulated;
    for (const std::filesystem::path& component : currentAssetDirectory_) {
        if (component == L".") {
            continue;
        }
        accumulated /= component;
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextUnformatted(">");
        ImGui::SameLine(0.0f, 3.0f);
        const std::string label = component.string();
        const std::string id = accumulated.generic_string();
        ImGui::PushID(id.c_str());
        if (ImGui::SmallButton(label.c_str())) {
            NavigateAssetBrowser(accumulated);
        }
        ImGui::PopID();
    }
}

void EditorScene::DrawAssetBrowserEntry(const std::filesystem::path& relativePath,
                                        bool directory) {
    const std::filesystem::path logicalPath =
        (std::filesystem::path("assets") / relativePath).lexically_normal();
    const std::string id = logicalPath.generic_string();
    const std::string label = std::string(directory ? "[Folder] " : "[Model] ") +
                              relativePath.filename().string();
    ImGui::PushID(id.c_str());
    const bool selected = selectedAsset_ == relativePath;
    if (ImGui::Selectable(label.c_str(), selected,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
        selectedAsset_ = relativePath;
        if (directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            NavigateAssetBrowser(relativePath);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", id.c_str());
    }
    if (!directory && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kModelAssetDragPayload, id.c_str(), id.size() + 1u);
        ImGui::TextUnformatted(id.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("AssetContext")) {
        if (directory) {
            if (ImGui::MenuItem("Open")) {
                NavigateAssetBrowser(relativePath);
            }
        } else {
            const std::string uri =
                "asset://" + relativePath.lexically_normal().generic_string();
            if (ImGui::MenuItem("Copy Asset URI")) {
                ImGui::SetClipboardText(uri.c_str());
                status_ = "Copied asset URI: " + uri;
            }
            if (ImGui::MenuItem("Copy Project Path")) {
                ImGui::SetClipboardText(id.c_str());
                status_ = "Copied project asset path: " + id;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void EditorScene::DrawHierarchyPanel() {
    if (ImGui::Button("Create Empty")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        selection_ = world_.CreateEntity();
        RecordImmediateEdit("Create Entity", before, selectionBefore);
        status_ = "Created a new entity.";
    }
    ImGui::SameLine();
    const bool canDelete = world_.Contains(selection_);
    if (!canDelete) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Duplicate")) {
        DuplicateSelection();
    }
    if (!canDelete) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!canDelete) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Delete")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        world_.DestroyEntity(selection_);
        selection_ = {};
        RecordImmediateEdit("Delete Entity", before, selectionBefore);
        status_ = "Deleted the selected entity hierarchy.";
    }
    if (!canDelete) {
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    for (EntityId id : world_.GetRootEntities()) {
        DrawEntityNode(id);
    }
    ImGui::Separator();
    ImGui::Selectable("Scene Root (drop here)", false);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize == sizeof(EntityId)) {
            EntityId child{};
            std::memcpy(&child, payload->Data, sizeof(child));
            ReparentEntity(child, {});
        }
        ImGui::EndDragDropTarget();
    }
}

void EditorScene::DrawEntityNode(EntityId id) {
    const WorldEntity* entity = world_.Find(id);
    if (entity == nullptr) {
        return;
    }
    const std::vector<EntityId> children = world_.GetChildren(id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selection_ == id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string idText = id.ToString();
    ImGui::PushID(idText.c_str());
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    if (ImGui::IsItemClicked()) {
        selection_ = id;
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityDragPayload, &id, sizeof(id));
        ImGui::TextUnformatted(entity->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize == sizeof(EntityId)) {
            EntityId child{};
            std::memcpy(&child, payload->Data, sizeof(child));
            ReparentEntity(child, id);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignModelAsset(id, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (open && !children.empty()) {
        for (EntityId child : children) {
            DrawEntityNode(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorScene::DrawInspectorPanel() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr) {
        ImGui::TextDisabled("Nothing selected.");
        return;
    }

    std::array<char, 256> nameBuffer{};
    strncpy_s(nameBuffer.data(), nameBuffer.size(), entity->name.c_str(), _TRUNCATE);
    const bool nameChanged = ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size());
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Rename Entity");
    }
    if (nameChanged) {
        entity->name = nameBuffer.data();
        if (entity->name.empty()) {
            entity->name = "Entity";
        }
        RefreshDirty();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    ImGui::TextDisabled("ID: %s", entity->id.ToString().c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Transform");
    auto drawTransform = [&](const char* label, DirectX::XMFLOAT3& value, float speed) {
        const bool changed = ImGui::DragFloat3(label, &value.x, speed);
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit(std::string("Modify Transform ") + label);
        }
        if (changed) {
            RefreshDirty();
            status_ = "Modified Transform.";
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    };
    drawTransform("Position", entity->transform.position, 0.05f);
    drawTransform("Rotation", entity->transform.rotationDegrees, 0.25f);
    drawTransform("Scale", entity->transform.scale, 0.02f);

    ImGui::Separator();
    if (!entity->meshRenderer) {
        if (ImGui::Button("Add Mesh Renderer")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->meshRenderer = MeshRendererComponent{};
            RecordImmediateEdit("Add MeshRenderer", before, selectionBefore);
            status_ = "Added MeshRenderer.";
        }
        return;
    }

    MeshRendererComponent& renderer = *entity->meshRenderer;
    ImGui::TextUnformatted("Mesh Renderer");
    if (ImGui::Button("Remove Mesh Renderer")) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->meshRenderer.reset();
        RecordImmediateEdit("Remove MeshRenderer", before, selectionBefore);
        status_ = "Removed MeshRenderer.";
        return;
    }
    std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (ImGui::Checkbox("Enabled", &renderer.enabled)) {
        RecordImmediateEdit("Toggle MeshRenderer", std::move(before), selectionBefore);
    }
    int source = static_cast<int>(renderer.sourceType);
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Combo("Source", &source, "Primitive\0Model\0")) {
        renderer.sourceType = static_cast<MeshSourceType>(source);
        RecordImmediateEdit("Change Mesh Source", std::move(before), selectionBefore);
    }
    if (renderer.sourceType == MeshSourceType::Primitive) {
        int primitive = static_cast<int>(renderer.primitive);
        before = WorldSerializer::Serialize(world_);
        if (ImGui::Combo("Primitive", &primitive, kPrimitiveNames,
                         static_cast<int>(std::size(kPrimitiveNames)))) {
            renderer.primitive = static_cast<MeshPrimitive>(primitive);
            RecordImmediateEdit("Change Primitive", std::move(before), selectionBefore);
        }
    } else {
        std::array<char, 512> pathBuffer{};
        strncpy_s(pathBuffer.data(), pathBuffer.size(), renderer.modelPath.c_str(), _TRUNCATE);
        if (ImGui::InputText("Model", pathBuffer.data(), pathBuffer.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            AssignModelAsset(selection_, pathBuffer.data());
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
                payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                AssignModelAsset(selection_, static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }
    }
}

void EditorScene::HandleEditorShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl || io.WantTextInput || pendingSceneAction_ != PendingSceneAction::None) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        RequestSceneAction(PendingSceneAction::NewScene);
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        RequestSceneAction(PendingSceneAction::OpenScene);
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (io.KeyShift) {
            SaveSceneAs();
        } else {
            SaveScene();
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        DuplicateSelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) {
            Redo();
        } else {
            Undo();
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        Redo();
    }
}

void EditorScene::DuplicateSelection() {
    if (!world_.Contains(selection_)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const EntityId duplicate = world_.DuplicateEntityHierarchy(selection_);
    if (!duplicate.IsValid()) {
        status_ = "Could not duplicate the selected entity hierarchy.";
        return;
    }
    selection_ = duplicate;
    RecordImmediateEdit("Duplicate Entity", before, selectionBefore);
    status_ = "Duplicated the selected entity hierarchy.";
}

void EditorScene::ReparentEntity(EntityId child, EntityId parent) {
    const WorldEntity* childEntity = world_.Find(child);
    if (childEntity == nullptr || childEntity->parent == parent) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!world_.SetParent(child, parent)) {
        status_ = "Cannot create a cyclic or invalid hierarchy.";
        return;
    }
    selection_ = child;
    RecordImmediateEdit("Reparent Entity", before, selectionBefore);
    status_ = parent.IsValid() ? "Reparented the entity." : "Moved the entity to the scene root.";
}

void EditorScene::AssignModelAsset(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !IsModelAsset(path)) {
        status_ = "The dropped model asset is invalid.";
        return;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped model asset no longer exists.";
        return;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    std::string assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped model asset path is too long.";
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath =
        entity->meshRenderer ? entity->meshRenderer->modelPath : std::string{};
    if (!entity->meshRenderer) {
        entity->meshRenderer = MeshRendererComponent{};
    }
    entity->meshRenderer->sourceType = MeshSourceType::Model;
    entity->meshRenderer->modelPath = assetPath;
    loadedModels_.erase(previousPath);
    loadedModels_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Model Asset", before, selectionBefore);
    status_ = "Assigned model asset: " + assetPath;
}

void EditorScene::RefreshAssetBrowser() {
    modelAssets_.clear();
    assetBrowserEntries_.clear();
    std::error_code error;
    if (!std::filesystem::is_directory(assetRoot_, error) || error) {
        return;
    }

    std::filesystem::path currentDirectory =
        (assetRoot_ / currentAssetDirectory_).lexically_normal();
    if ((!currentAssetDirectory_.empty() &&
         !IsPathWithinRoot(assetRoot_, currentDirectory)) ||
        !std::filesystem::is_directory(currentDirectory, error) || error) {
        currentAssetDirectory_.clear();
        currentDirectory = assetRoot_;
        error.clear();
    }

    std::filesystem::directory_iterator directoryIterator(
        currentDirectory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator directoryEnd;
    while (!error && directoryIterator != directoryEnd) {
        const std::filesystem::directory_entry entry = *directoryIterator;
        const bool directory = entry.is_directory(error);
        if (!error && directory && IsPathWithinRoot(assetRoot_, entry.path())) {
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), assetRoot_, error);
            if (!error) {
                assetBrowserEntries_.push_back({relative.lexically_normal(), true});
            }
        } else if (!error && entry.is_regular_file(error) && !error &&
                   IsModelAsset(entry.path())) {
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), assetRoot_, error);
            if (!error) {
                assetBrowserEntries_.push_back({relative.lexically_normal(), false});
            }
        }
        error.clear();
        directoryIterator.increment(error);
    }
    std::ranges::sort(assetBrowserEntries_, [](const AssetBrowserEntry& left,
                                               const AssetBrowserEntry& right) {
        if (left.directory != right.directory) {
            return left.directory && !right.directory;
        }
        return left.relativePath.generic_string() < right.relativePath.generic_string();
    });

    error.clear();
    std::filesystem::recursive_directory_iterator iterator(
        assetRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error && IsModelAsset(iterator->path())) {
            std::filesystem::path relative =
                std::filesystem::relative(iterator->path(), assetRoot_, error);
            if (!error) {
                modelAssets_.push_back(
                    (std::filesystem::path("assets") / relative).lexically_normal());
            }
        }
        iterator.increment(error);
    }
    std::ranges::sort(modelAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
}

void EditorScene::NavigateAssetBrowser(
    const std::filesystem::path& relativeDirectory) {
    const std::filesystem::path normalized = relativeDirectory.lexically_normal();
    if (normalized.is_absolute() || normalized.has_root_name() ||
        normalized.has_root_directory() || HasParentTraversal(normalized)) {
        status_ = "Asset Browser rejected an invalid directory.";
        return;
    }
    const std::filesystem::path physical =
        normalized == L"." ? assetRoot_ : assetRoot_ / normalized;
    std::error_code error;
    if (!std::filesystem::is_directory(physical, error) || error ||
        (normalized != L"." && !normalized.empty() &&
         !IsPathWithinRoot(assetRoot_, physical))) {
        status_ = "Asset Browser folder no longer exists.";
        return;
    }
    pendingAssetDirectory_ = normalized == L"." ? std::filesystem::path{} : normalized;
}

std::optional<std::filesystem::path>
EditorScene::ResolveProjectAssetPath(const std::filesystem::path& path) const {
    const std::filesystem::path resolved = AssetManager::ResolvePathStrict(path);
    return resolved.empty() ? std::nullopt
                            : std::optional<std::filesystem::path>(resolved);
}

EditorScene::HistoryState EditorScene::CaptureHistoryState() const {
    return {WorldSerializer::Serialize(world_), selection_};
}

bool EditorScene::RestoreHistoryState(const HistoryState& state) {
    World restored;
    std::string error;
    if (!WorldSerializer::Deserialize(state.world, restored, &error)) {
        status_ = "History restore failed: " + error;
        return false;
    }
    world_ = std::move(restored);
    selection_ = world_.Contains(state.selection) ? state.selection : EntityId{};
    RefreshDirty();
    return true;
}

void EditorScene::BeginHistoryEdit(std::string label) {
    if (!pendingHistoryEdit_) {
        pendingHistoryEdit_ = PendingHistoryEdit{std::move(label), CaptureHistoryState()};
    }
}

void EditorScene::CommitHistoryEdit() {
    if (!pendingHistoryEdit_) {
        return;
    }
    PendingHistoryEdit pending = std::move(*pendingHistoryEdit_);
    pendingHistoryEdit_.reset();
    HistoryState after = CaptureHistoryState();
    if (pending.before.world == after.world && pending.before.selection == after.selection) {
        return;
    }
    undoHistory_.push_back(
        {std::move(pending.label), std::move(pending.before), std::move(after)});
    if (undoHistory_.size() > kMaxHistoryEntries) {
        undoHistory_.erase(undoHistory_.begin());
    }
    redoHistory_.clear();
    RefreshDirty();
}

void EditorScene::RecordImmediateEdit(std::string label, std::string before,
                                      EntityId selectionBefore) {
    pendingHistoryEdit_.reset();
    HistoryState after = CaptureHistoryState();
    if (before == after.world && selectionBefore == after.selection) {
        return;
    }
    undoHistory_.push_back({std::move(label), {std::move(before), selectionBefore},
                            std::move(after)});
    if (undoHistory_.size() > kMaxHistoryEntries) {
        undoHistory_.erase(undoHistory_.begin());
    }
    redoHistory_.clear();
    RefreshDirty();
}

void EditorScene::Undo() {
    CommitHistoryEdit();
    if (undoHistory_.empty()) {
        return;
    }
    HistoryEntry entry = std::move(undoHistory_.back());
    undoHistory_.pop_back();
    if (!RestoreHistoryState(entry.before)) {
        undoHistory_.push_back(std::move(entry));
        return;
    }
    status_ = "Undo: " + entry.label;
    redoHistory_.push_back(std::move(entry));
}

void EditorScene::Redo() {
    CommitHistoryEdit();
    if (redoHistory_.empty()) {
        return;
    }
    HistoryEntry entry = std::move(redoHistory_.back());
    redoHistory_.pop_back();
    if (!RestoreHistoryState(entry.after)) {
        redoHistory_.push_back(std::move(entry));
        return;
    }
    status_ = "Redo: " + entry.label;
    undoHistory_.push_back(std::move(entry));
}

void EditorScene::ClearHistory(bool markClean) {
    undoHistory_.clear();
    redoHistory_.clear();
    pendingHistoryEdit_.reset();
    savedWorldSnapshot_ = markClean ? WorldSerializer::Serialize(world_) : std::string{};
    RefreshDirty();
}

void EditorScene::RefreshDirty() {
    dirty_ = WorldSerializer::Serialize(world_) != savedWorldSnapshot_;
}

void EditorScene::ResolveMeshResources() {
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model ||
            entity.meshRenderer->modelPath.empty() ||
            loadedModels_.contains(entity.meshRenderer->modelPath)) {
            continue;
        }
        if (!ResolveProjectAssetPath(entity.meshRenderer->modelPath)) {
            continue;
        }
        loadedModels_.emplace(entity.meshRenderer->modelPath,
                              ctx_->rendering.model->LoadHandle(
                                  std::filesystem::path(entity.meshRenderer->modelPath).wstring()));
    }
}

ModelHandle EditorScene::ResolveModel(const MeshRendererComponent& component) const {
    if (component.sourceType == MeshSourceType::Primitive) {
        const size_t index = static_cast<size_t>(component.primitive);
        return index < std::size(primitiveModels_) ? primitiveModels_[index] : ModelHandle{};
    }
    const auto found = loadedModels_.find(component.modelPath);
    return found != loadedModels_.end() ? found->second : ModelHandle{};
}

void EditorScene::BuildRenderScene() {
    renderScene_.BeginFrame();
    ModelManager* models = ctx_ ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || !entity.meshRenderer->enabled) {
            continue;
        }
        const ModelHandle handle = ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        DirectX::XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            continue;
        }
        const Transform transform = DecomposeTransform(worldMatrix);
        auto submit = [&](uint32_t meshId, uint32_t materialId, uint32_t textureId,
                          uint32_t normalTextureId) {
            if (!IsValidResourceId(meshId)) {
                return;
            }
            RenderMeshItem item{};
            item.mesh = &models->GetMesh(meshId);
            if (IsValidResourceId(materialId)) {
                item.material = models->GetMaterial(materialId);
            }
            item.transform = transform;
            item.textureId = textureId;
            item.normalTextureId = normalTextureId;
            item.objectId = static_cast<uint32_t>(EntityIdHash{}(entity.id));
            renderScene_.SubmitMesh(item);
        };
        if (!model->subMeshes.empty()) {
            for (const ModelSubMesh& subMesh : model->subMeshes) {
                submit(subMesh.meshId, subMesh.materialId, subMesh.textureId,
                       subMesh.normalTextureId);
            }
        } else {
            submit(model->meshId, model->materialId, model->textureId, kInvalidResourceId);
        }
    }
}

void EditorScene::PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (!ImGui::IsItemHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    using namespace DirectX;
    const XMVECTOR nearPoint = XMVector3Unproject(
        XMVectorSet(mouse.x - imageMin.x, mouse.y - imageMin.y, 0.0f, 1.0f), 0.0f, 0.0f,
        width, height, 0.0f, 1.0f, sceneViewCamera_.GetProj(), sceneViewCamera_.GetView(),
        XMMatrixIdentity());
    const XMVECTOR farPoint = XMVector3Unproject(
        XMVectorSet(mouse.x - imageMin.x, mouse.y - imageMin.y, 1.0f, 1.0f), 0.0f, 0.0f,
        width, height, 0.0f, 1.0f, sceneViewCamera_.GetProj(), sceneViewCamera_.GetView(),
        XMMatrixIdentity());
    const XMVECTOR rayDirection = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

    EntityId closest{};
    float closestDistance = (std::numeric_limits<float>::max)();
    ModelManager* models = ctx_->rendering.model;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || !entity.meshRenderer->enabled) {
            continue;
        }
        const ModelHandle handle = ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        XMFLOAT3 boundsMin{};
        XMFLOAT3 boundsMax{};
        XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !TryGetModelBounds(*model, boundsMin, boundsMax) ||
            !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            continue;
        }
        XMVECTOR determinant{};
        const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, XMLoadFloat4x4(&worldMatrix));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) < 1.0e-8f) {
            continue;
        }
        const XMVECTOR localOrigin = XMVector3TransformCoord(nearPoint, inverseWorld);
        const XMVECTOR localDirection = XMVector3TransformNormal(rayDirection, inverseWorld);
        float hitDistance = 0.0f;
        if (IntersectRayBounds(localOrigin, localDirection, boundsMin, boundsMax, hitDistance) &&
            hitDistance < closestDistance) {
            closest = entity.id;
            closestDistance = hitDistance;
        }
    }
    selection_ = closest;
    status_ = closest.IsValid() ? "Selected entity from Scene View."
                                : "Scene View selection cleared.";
}

void EditorScene::RequestSceneAction(PendingSceneAction action,
                                     std::filesystem::path path) {
    if (action == PendingSceneAction::None) {
        return;
    }
    if (!dirty_) {
        ExecuteSceneAction(action, path);
        return;
    }
    pendingSceneAction_ = action;
    pendingScenePath_ = std::move(path);
    showUnsavedChangesDialog_ = true;
}

void EditorScene::ExecuteSceneAction(PendingSceneAction action,
                                     const std::filesystem::path& path) {
    switch (action) {
    case PendingSceneAction::NewScene:
        NewScene(true);
        break;
    case PendingSceneAction::OpenScene:
        if (!path.empty()) {
            LoadScene(path);
        } else if (const std::optional<std::filesystem::path> selected = ShowOpenSceneDialog()) {
            LoadScene(*selected);
        }
        break;
    case PendingSceneAction::ReloadScene:
        if (!scenePath_.empty()) {
            LoadScene(scenePath_);
        }
        break;
    case PendingSceneAction::Exit:
        if (requestClose_) {
            requestClose_();
        }
        break;
    case PendingSceneAction::None:
        break;
    }
}

void EditorScene::NewScene(bool clearPath) {
    world_.Clear();
    const EntityId camera = world_.CreateEntity("Main Camera");
    if (WorldEntity* cameraEntity = world_.Find(camera)) {
        cameraEntity->transform.position = {0.0f, 2.0f, -5.0f};
    }
    selection_ = world_.CreateEntity("Cube");
    if (WorldEntity* cube = world_.Find(selection_)) {
        cube->meshRenderer = MeshRendererComponent{};
    }
    if (clearPath) {
        scenePath_.clear();
    }
    ClearHistory(false);
    status_ = "Created a new scene.";
}

bool EditorScene::SaveScene() {
    if (scenePath_.empty()) {
        return SaveSceneAs();
    }
    std::string error;
    if (!WorldSerializer::Save(world_, scenePath_, &error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    dirty_ = false;
    savedWorldSnapshot_ = WorldSerializer::Serialize(world_);
    AddRecentScene(scenePath_);
    status_ = "Saved scene: " + scenePath_.string();
    return true;
}

bool EditorScene::SaveSceneAs() {
    const std::optional<std::filesystem::path> selected = ShowSaveSceneDialog();
    if (!selected) {
        status_ = "Save cancelled.";
        return false;
    }
    const std::filesystem::path previousPath = scenePath_;
    scenePath_ = *selected;
    if (SaveScene()) {
        return true;
    }
    scenePath_ = previousPath;
    return false;
}

bool EditorScene::LoadScene(const std::filesystem::path& path) {
    if (path.extension() != L".likescene" || !IsPathWithinRoot(sceneRoot_, path)) {
        status_ = "Load failed: scene must be inside the project scenes directory.";
        return false;
    }
    World loaded;
    std::string error;
    if (!WorldSerializer::Load(path, loaded, &error)) {
        status_ = "Load failed: " + error;
        return false;
    }
    world_ = std::move(loaded);
    scenePath_ = path;
    selection_ = world_.Empty() ? EntityId{} : world_.Entities().front().id;
    dirty_ = false;
    ClearHistory(true);
    AddRecentScene(scenePath_);
    status_ = "Loaded scene: " + scenePath_.string();
    return true;
}

void EditorScene::AddRecentScene(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return;
    }
    std::erase_if(recentScenePaths_, [&normalized](const std::filesystem::path& item) {
        return _wcsicmp(item.c_str(), normalized.c_str()) == 0;
    });
    recentScenePaths_.insert(recentScenePaths_.begin(), normalized);
    if (recentScenePaths_.size() > kMaxRecentScenes) {
        recentScenePaths_.resize(kMaxRecentScenes);
    }
    recentScenesStore_.Save(recentScenePaths_);
}

std::optional<std::filesystem::path> EditorScene::ShowOpenSceneDialog() const {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Scene (*.likescene)\0*.likescene\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::wstring initialDirectory = sceneRoot_.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likescene";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return selected.extension() == L".likescene" && IsPathWithinRoot(sceneRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}

std::optional<std::filesystem::path> EditorScene::ShowSaveSceneDialog() const {
    std::array<wchar_t, 32768> buffer{};
    if (!scenePath_.empty()) {
        const std::wstring filename = scenePath_.filename().wstring();
        wcsncpy_s(buffer.data(), buffer.size(), filename.c_str(), _TRUNCATE);
    } else {
        wcsncpy_s(buffer.data(), buffer.size(), L"untitled.likescene", _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Scene (*.likescene)\0*.likescene\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::wstring initialDirectory = sceneRoot_.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likescene";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetSaveFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return selected.extension() == L".likescene" && IsPathWithinRoot(sceneRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}
