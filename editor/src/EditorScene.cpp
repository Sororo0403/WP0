#include "EditorScene.h"

#include "core/AssetManager.h"
#include "graphics/DirectXCommon.h"
#include "graphics/RenderScene.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "ImGuizmo.h"
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
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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

bool TryDecomposeTransformComponent(const DirectX::XMMATRIX& matrix,
                                    TransformComponent& transform) {
    DirectX::XMFLOAT4X4 stored{};
    DirectX::XMStoreFloat4x4(&stored, matrix);
    float translation[3]{};
    float rotation[3]{};
    float scale[3]{};
    ImGuizmo::DecomposeMatrixToComponents(&stored._11, translation, rotation, scale);
    const bool finite = std::ranges::all_of(translation, [](float value) {
                            return std::isfinite(value);
                        }) &&
                        std::ranges::all_of(rotation, [](float value) {
                            return std::isfinite(value);
                        }) &&
                        std::ranges::all_of(scale, [](float value) {
                            return std::isfinite(value);
                        });
    if (!finite) {
        return false;
    }
    transform.position = {translation[0], translation[1], translation[2]};
    transform.rotationDegrees = {rotation[0], rotation[1], rotation[2]};
    transform.scale = {scale[0], scale[1], scale[2]};
    return true;
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

bool BuildSceneRay(const Camera& camera, const ImVec2& imageMin, const ImVec2& imageMax,
                   const ImVec2& screenPosition, DirectX::XMVECTOR& rayOrigin,
                   DirectX::XMVECTOR& rayDirection) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    using namespace DirectX;
    rayOrigin = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 0.0f, 1.0f),
        0.0f, 0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(),
        XMMatrixIdentity());
    const XMVECTOR farPoint = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 1.0f, 1.0f),
        0.0f, 0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(),
        XMMatrixIdentity());
    rayDirection = XMVector3Normalize(XMVectorSubtract(farPoint, rayOrigin));
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    XMStoreFloat3(&origin, rayOrigin);
    XMStoreFloat3(&direction, rayDirection);
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) &&
           std::isfinite(direction.z);
}

DirectX::XMFLOAT3 CalculateScenePlacementPosition(const Camera& camera,
                                                  const ImVec2& imageMin,
                                                  const ImVec2& imageMax,
                                                  const ImVec2& screenPosition) {
    DirectX::XMVECTOR rayOrigin{};
    DirectX::XMVECTOR rayDirection{};
    DirectX::XMFLOAT3 position{};
    if (!BuildSceneRay(camera, imageMin, imageMax, screenPosition, rayOrigin, rayDirection)) {
        return position;
    }
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);
    float distance = 5.0f;
    if (std::abs(direction.y) > 1.0e-5f) {
        const float groundDistance = -origin.y / direction.y;
        if (groundDistance >= 0.0f) {
            distance = groundDistance;
        }
    }
    DirectX::XMStoreFloat3(
        &position,
        DirectX::XMVectorAdd(rayOrigin, DirectX::XMVectorScale(rayDirection, distance)));
    position.y = 0.0f;
    return position;
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
    ImGuizmo::BeginFrame();
    HandleEditorShortcuts();
    DrawMainMenu();
    DrawUnsavedChangesDialog();
    DrawEntityRenameDialog();
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
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, canDuplicate)) {
            CopySelection();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, canDuplicate)) {
            CutSelection();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !entityClipboard_.empty())) {
            PasteEntityClipboard();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate)) {
            DuplicateSelection();
        }
        if (ImGui::MenuItem("Delete", "Delete", false, canDuplicate)) {
            DeleteSelection();
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

void EditorScene::DrawEntityRenameDialog() {
    if (showEntityRenameDialog_) {
        ImGui::OpenPopup("Rename Entity");
        showEntityRenameDialog_ = false;
        focusEntityRenameInput_ = true;
    }
    if (!ImGui::BeginPopupModal("Rename Entity", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    WorldEntity* entity = world_.Find(renameEntity_);
    if (entity == nullptr) {
        renameEntity_ = {};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("ID: %s", renameEntity_.ToString().c_str());
    if (focusEntityRenameInput_) {
        ImGui::SetKeyboardFocusHere();
        focusEntityRenameInput_ = false;
    }
    ImGui::SetNextItemWidth(320.0f);
    const bool submitted = ImGui::InputText(
        "##EntityName", renameBuffer_.data(), renameBuffer_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (submitted || ImGui::Button("Rename", ImVec2(100.0f, 0.0f))) {
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->name = renameBuffer_.data();
        if (entity->name.empty()) {
            entity->name = "Entity";
        }
        selection_ = renameEntity_;
        renameEntity_ = {};
        RecordImmediateEdit("Rename Entity", before, selectionBefore);
        status_ = "Renamed the entity.";
        ImGui::CloseCurrentPopup();
    } else {
        ImGui::SameLine();
        if (cancel || ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            renameEntity_ = {};
            ImGui::CloseCurrentPopup();
        }
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
    DrawSceneGizmoToolbar();
    ImGui::Separator();
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
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        const bool imageHovered = ImGui::IsItemHovered();
        HandleSceneAssetDrop(imageMin, imageMax);
        HandleSceneContextMenu(imageMin, imageMax, imageHovered);
        DrawSceneGrid(imageMin, imageMax);
        DrawSceneSelectionOutline(imageMin, imageMax);
        if (!DrawSceneTransformGizmo(imageMin, imageMax)) {
            PickSceneEntity(imageMin, imageMax, imageHovered);
        }
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
    SynchronizeHierarchySelection();
    if (ImGui::Button("Create")) {
        ImGui::OpenPopup("CreateEntity");
    }
    if (ImGui::BeginPopup("CreateEntity")) {
        DrawCreateEntityMenu({0.0f, 0.0f, 0.0f});
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool canDelete = !hierarchySelection_.empty();
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
        DeleteSelection();
    }
    if (!canDelete) {
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-58.0f);
    ImGui::InputTextWithHint("##HierarchySearch", "Search entities...", hierarchySearch_.data(),
                             hierarchySearch_.size());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        hierarchySearch_.fill('\0');
    }
    const std::string hierarchyQuery(hierarchySearch_.data());
    visibleHierarchyEntities_.clear();
    if (!hierarchyQuery.empty()) {
        for (const WorldEntity& entity : world_.Entities()) {
            if (!ContainsCaseInsensitive(entity.name, hierarchyQuery)) {
                continue;
            }
            EntityId current = entity.id;
            for (size_t depth = 0; current.IsValid() && depth <= world_.Entities().size();
                 ++depth) {
                if (!visibleHierarchyEntities_.insert(current).second) {
                    break;
                }
                const WorldEntity* currentEntity = world_.Find(current);
                current = currentEntity != nullptr ? currentEntity->parent : EntityId{};
            }
        }
    }
    ImGui::Separator();
    bool drewEntity = false;
    for (EntityId id : world_.GetRootEntities()) {
        if (!hierarchyQuery.empty() && !visibleHierarchyEntities_.contains(id)) {
            continue;
        }
        DrawEntityNode(id);
        drewEntity = true;
    }
    if (!hierarchyQuery.empty() && !drewEntity) {
        ImGui::TextDisabled("No matching entities.");
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
    std::vector<EntityId> children = world_.GetChildren(id);
    if (filtering) {
        std::erase_if(children, [this](EntityId child) {
            return !visibleHierarchyEntities_.contains(child);
        });
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (filtering) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (IsHierarchyEntitySelected(id)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string idText = id.ToString();
    ImGui::PushID(idText.c_str());
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    if (ImGui::IsItemClicked()) {
        const ImGuiIO& io = ImGui::GetIO();
        SelectHierarchyEntity(id, io.KeyCtrl, io.KeyShift);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        RequestEntityRename(id);
    }
    bool hierarchyChanged = false;
    bool deleteRequested = false;
    if (ImGui::BeginPopupContextItem("EntityContext")) {
        if (!IsHierarchyEntitySelected(id)) {
            SelectHierarchyEntity(id, false, false);
        }
        if (ImGui::BeginMenu("Create Child")) {
            hierarchyChanged = DrawCreateEntityMenu({0.0f, 0.0f, 0.0f}, id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2")) {
            RequestEntityRename(id);
        }
        const WorldEntity* contextEntity = world_.Find(id);
        const std::vector<EntityId> siblings =
            contextEntity != nullptr && contextEntity->parent.IsValid()
                ? world_.GetChildren(contextEntity->parent)
                : world_.GetRootEntities();
        const auto siblingPosition = std::ranges::find(siblings, id);
        const bool canMoveUp = siblingPosition != siblings.end() &&
                               siblingPosition != siblings.begin();
        const bool canMoveDown = siblingPosition != siblings.end() &&
                                 std::next(siblingPosition) != siblings.end();
        if (ImGui::MenuItem("Move Up", "Alt+Up", false, canMoveUp)) {
            hierarchyChanged = MoveEntityInHierarchy(id, -1);
        }
        if (ImGui::MenuItem("Move Down", "Alt+Down", false, canMoveDown)) {
            hierarchyChanged = MoveEntityInHierarchy(id, 1);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            DuplicateSelection();
            hierarchyChanged = true;
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            CopySelection();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X")) {
            CutSelection();
            hierarchyChanged = true;
        }
        if (ImGui::MenuItem("Paste as Child", nullptr, false, !entityClipboard_.empty())) {
            hierarchyChanged = PasteEntityClipboard(id);
        }
        if (ImGui::MenuItem("Delete", "Delete")) {
            deleteRequested = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Entity ID")) {
            ImGui::SetClipboardText(idText.c_str());
            status_ = "Copied entity ID: " + idText;
        }
        ImGui::EndPopup();
    }
    if (deleteRequested) {
        DeleteSelection();
        hierarchyChanged = true;
    }
    if (hierarchyChanged) {
        if (open && !children.empty()) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
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
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##Transform")) {
        CommitHistoryEdit();
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->transform = TransformComponent{};
        RecordImmediateEdit("Reset Transform", before, selectionBefore);
        status_ = "Reset Transform.";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##Transform")) {
        CommitHistoryEdit();
        transformClipboard_ = entity->transform;
        status_ = "Copied Transform.";
    }
    ImGui::SameLine();
    if (!transformClipboard_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("Paste##Transform")) {
        CommitHistoryEdit();
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        entity->transform = *transformClipboard_;
        RecordImmediateEdit("Paste Transform", before, selectionBefore);
        status_ = "Pasted Transform.";
    }
    if (!transformClipboard_) {
        ImGui::EndDisabled();
    }
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
    if (io.WantTextInput || pendingSceneAction_ != PendingSceneAction::None) {
        return;
    }
    if (!io.KeyCtrl) {
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            MoveEntityInHierarchy(selection_, -1);
        } else if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
            MoveEntityInHierarchy(selection_, 1);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            RequestEntityRename(selection_);
        } else if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            gizmoOperation_ = GizmoOperation::Translate;
        } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            gizmoOperation_ = GizmoOperation::Rotate;
        } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            gizmoOperation_ = GizmoOperation::Scale;
        } else if (!gizmoWasUsing_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            DeleteSelection();
        }
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
    } else if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        CopySelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        CutSelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        PasteEntityClipboard();
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
    const size_t rootCount = static_cast<size_t>(std::ranges::count_if(
        clipboardWorld.Entities(), [](const WorldEntity& entity) {
            return !entity.parent.IsValid();
        }));
    if (rootCount == 0u) {
        status_ = "Paste failed: clipboard has no entity hierarchy roots.";
        return false;
    }

    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::unordered_map<EntityId, EntityId, EntityIdHash> pastedIds;
    pastedIds.reserve(clipboardWorld.Entities().size());
    std::vector<EntityId> pastedRoots;
    pastedRoots.reserve(rootCount);
    for (const WorldEntity& source : clipboardWorld.Entities()) {
        const EntityId pasted = world_.CreateEntity(source.name);
        pastedIds.emplace(source.id, pasted);
        WorldEntity* destination = world_.Find(pasted);
        if (destination == nullptr) {
            continue;
        }
        destination->transform = source.transform;
        destination->meshRenderer = source.meshRenderer;
        if (!source.parent.IsValid()) {
            pastedRoots.push_back(pasted);
            destination->name += " Copy";
        }
    }
    bool valid = pastedRoots.size() == rootCount;
    for (const WorldEntity& source : clipboardWorld.Entities()) {
        const EntityId pasted = pastedIds.at(source.id);
        const EntityId pastedParent = source.parent.IsValid() ? pastedIds.at(source.parent) : parent;
        if (pastedParent.IsValid() && !world_.SetParent(pasted, pastedParent)) {
            valid = false;
            break;
        }
    }
    if (!valid) {
        World restored;
        if (WorldSerializer::Deserialize(before, restored, nullptr)) {
            world_ = std::move(restored);
        }
        selection_ = selectionBefore;
        status_ = "Paste failed while rebuilding the entity hierarchy.";
        return false;
    }
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

void EditorScene::ReparentEntity(EntityId child, EntityId parent) {
    const WorldEntity* childEntity = world_.Find(child);
    if (childEntity == nullptr || childEntity->parent == parent) {
        return;
    }
    DirectX::XMFLOAT4X4 childWorld{};
    if (!world_.TryGetWorldMatrix(child, childWorld)) {
        status_ = "Could not read the entity world transform.";
        return;
    }
    DirectX::XMMATRIX localMatrix = DirectX::XMLoadFloat4x4(&childWorld);
    if (parent.IsValid()) {
        DirectX::XMFLOAT4X4 parentWorld{};
        if (!world_.TryGetWorldMatrix(parent, parentWorld)) {
            status_ = "Could not read the new parent world transform.";
            return;
        }
        DirectX::XMVECTOR determinant{};
        const DirectX::XMMATRIX inverseParent =
            DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&parentWorld));
        const float determinantValue = DirectX::XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
            status_ = "Cannot reparent under a singular transform.";
            return;
        }
        localMatrix *= inverseParent;
    }
    TransformComponent localTransform{};
    if (!TryDecomposeTransformComponent(localMatrix, localTransform)) {
        status_ = "Could not preserve the entity world transform.";
        return;
    }
    CommitHistoryEdit();
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!world_.SetParent(child, parent)) {
        status_ = "Cannot create a cyclic or invalid hierarchy.";
        return;
    }
    WorldEntity* reparented = world_.Find(child);
    if (reparented == nullptr) {
        status_ = "The reparented entity no longer exists.";
        return;
    }
    reparented->transform = localTransform;
    selection_ = child;
    RecordImmediateEdit("Reparent Entity", before, selectionBefore);
    status_ = parent.IsValid() ? "Reparented the entity without moving it."
                               : "Moved the entity to the scene root without moving it.";
}

void EditorScene::AssignModelAsset(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeModelAssetReference(path, assetPath)) {
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

bool EditorScene::TryNormalizeModelAssetReference(const std::filesystem::path& path,
                                                  std::string& assetPath) {
    if (!IsModelAsset(path)) {
        status_ = "The dropped model asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped model asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped model asset path is too long.";
        return false;
    }
    return true;
}

void EditorScene::HandleSceneAssetDrop(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        CreateModelEntityFromAsset(static_cast<const char*>(payload->Data), position);
    }
    ImGui::EndDragDropTarget();
}

void EditorScene::HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                         bool imageHovered) {
    if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        sceneContextCreatePosition_ = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        ImGui::OpenPopup("SceneContext");
    }
    if (!ImGui::BeginPopup("SceneContext")) {
        return;
    }
    ImGui::TextDisabled("Create at %.2f, %.2f, %.2f", sceneContextCreatePosition_.x,
                        sceneContextCreatePosition_.y, sceneContextCreatePosition_.z);
    ImGui::Separator();
    DrawCreateEntityMenu(sceneContextCreatePosition_);
    ImGui::EndPopup();
}

void EditorScene::CreateModelEntityFromAsset(const std::filesystem::path& path,
                                             const DirectX::XMFLOAT3& position) {
    std::string assetPath;
    if (!TryNormalizeModelAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::string entityName = path.stem().string();
    if (entityName.empty()) {
        entityName = "Model";
    }
    const EntityId entityId = world_.CreateEntity(std::move(entityName));
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "Could not create an entity for the model asset.";
        return;
    }
    entity->transform.position = position;
    entity->meshRenderer = MeshRendererComponent{};
    entity->meshRenderer->sourceType = MeshSourceType::Model;
    entity->meshRenderer->modelPath = assetPath;
    loadedModels_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Create Model Entity", before, selectionBefore);
    status_ = "Created model entity: " + assetPath;
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
    hierarchySelection_.clear();
    if (selection_.IsValid()) {
        hierarchySelection_.insert(selection_);
    }
    hierarchySelectionAnchor_ = selection_;
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

void EditorScene::PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax,
                                  bool imageHovered) {
    if (!imageHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    using namespace DirectX;
    XMVECTOR nearPoint{};
    XMVECTOR rayDirection{};
    if (!BuildSceneRay(sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos(), nearPoint,
                       rayDirection)) {
        return;
    }

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

void EditorScene::DrawSceneSelectionOutline(const ImVec2& imageMin,
                                            const ImVec2& imageMax) const {
    if (!selection_.IsValid() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    const WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->meshRenderer || !entity->meshRenderer->enabled) {
        return;
    }
    const ModelHandle handle = ResolveModel(*entity->meshRenderer);
    const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (model == nullptr || !TryGetModelBounds(*model, boundsMin, boundsMax) ||
        !world_.TryGetWorldMatrix(entity->id, worldMatrix)) {
        return;
    }

    using namespace DirectX;
    const XMMATRIX worldViewProjection =
        XMLoadFloat4x4(&worldMatrix) * sceneViewCamera_.GetViewProjection();
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    const XMFLOAT3 corners[8] = {
        {boundsMin.x, boundsMin.y, boundsMin.z}, {boundsMax.x, boundsMin.y, boundsMin.z},
        {boundsMax.x, boundsMax.y, boundsMin.z}, {boundsMin.x, boundsMax.y, boundsMin.z},
        {boundsMin.x, boundsMin.y, boundsMax.z}, {boundsMax.x, boundsMin.y, boundsMax.z},
        {boundsMax.x, boundsMax.y, boundsMax.z}, {boundsMin.x, boundsMax.y, boundsMax.z},
    };
    ImVec2 projected[8]{};
    for (size_t index = 0; index < std::size(corners); ++index) {
        const XMVECTOR clip = XMVector4Transform(
            XMVectorSet(corners[index].x, corners[index].y, corners[index].z, 1.0f),
            worldViewProjection);
        const float clipW = XMVectorGetW(clip);
        if (!std::isfinite(clipW) || clipW <= 1.0e-5f) {
            return;
        }
        const float ndcX = XMVectorGetX(clip) / clipW;
        const float ndcY = XMVectorGetY(clip) / clipW;
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
            return;
        }
        projected[index] = {imageMin.x + (ndcX * 0.5f + 0.5f) * width,
                            imageMin.y + (0.5f - ndcY * 0.5f) * height};
    }

    constexpr size_t edges[][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
        {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    constexpr ImU32 outlineColor = IM_COL32(255, 184, 56, 255);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    for (const auto& edge : edges) {
        drawList->AddLine(projected[edge[0]], projected[edge[1]], outlineColor, 2.0f);
    }

    ImVec2 labelPosition = projected[0];
    for (const ImVec2& point : projected) {
        labelPosition.x = (std::min)(labelPosition.x, point.x);
        labelPosition.y = (std::min)(labelPosition.y, point.y);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(entity->name.c_str());
    const float labelMinX = imageMin.x + 3.0f;
    const float labelMinY = imageMin.y + 3.0f;
    const float labelMaxX = (std::max)(labelMinX, imageMax.x - textSize.x - 9.0f);
    const float labelMaxY = (std::max)(labelMinY, imageMax.y - textSize.y - 7.0f);
    labelPosition.x = std::clamp(labelPosition.x, labelMinX, labelMaxX);
    labelPosition.y =
        std::clamp(labelPosition.y - textSize.y - 8.0f, labelMinY, labelMaxY);
    drawList->AddRectFilled(labelPosition,
                            {labelPosition.x + textSize.x + 6.0f,
                             labelPosition.y + textSize.y + 4.0f},
                            IM_COL32(20, 20, 24, 210), 3.0f);
    drawList->AddText({labelPosition.x + 3.0f, labelPosition.y + 2.0f}, outlineColor,
                      entity->name.c_str());
    drawList->PopClipRect();
}

void EditorScene::DrawSceneGizmoToolbar() {
    auto operationButton = [&](const char* label, GizmoOperation operation) {
        const bool selected = gizmoOperation_ == operation;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label)) {
            gizmoOperation_ = operation;
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    };
    operationButton("Move (W)", GizmoOperation::Translate);
    ImGui::SameLine();
    operationButton("Rotate (E)", GizmoOperation::Rotate);
    ImGui::SameLine();
    operationButton("Scale (R)", GizmoOperation::Scale);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", gizmoSpace_ == GizmoSpace::Local)) {
        gizmoSpace_ = GizmoSpace::Local;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("World", gizmoSpace_ == GizmoSpace::World)) {
        gizmoSpace_ = GizmoSpace::World;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &showSceneGrid_);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &gizmoSnapEnabled_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    if (gizmoOperation_ == GizmoOperation::Translate) {
        ImGui::DragFloat("##GizmoSnap", &translationSnap_, 0.05f, 0.001f, 1000.0f, "%.3f m",
                         ImGuiSliderFlags_AlwaysClamp);
    } else if (gizmoOperation_ == GizmoOperation::Rotate) {
        ImGui::DragFloat("##GizmoSnap", &rotationSnapDegrees_, 0.5f, 0.1f, 180.0f, "%.1f deg",
                         ImGuiSliderFlags_AlwaysClamp);
    } else {
        ImGui::DragFloat("##GizmoSnap", &scaleSnap_, 0.01f, 0.001f, 10.0f, "%.3f",
                         ImGuiSliderFlags_AlwaysClamp);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable Snap or hold Ctrl while manipulating.");
    }
}

void EditorScene::DrawSceneGrid(const ImVec2& imageMin, const ImVec2& imageMax) const {
    if (!showSceneGrid_) {
        return;
    }
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMFLOAT4X4 identity{};
    DirectX::XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    DirectX::XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::DrawGridCustomColor(
        &view._11, &projection._11, &identity._11, 50.0f, 5.0f, 5u,
        IM_COL32(125, 135, 150, 90), IM_COL32(95, 105, 120, 45),
        IM_COL32(230, 165, 70, 150));
}

bool EditorScene::DrawSceneTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (entity == nullptr || !world_.TryGetWorldMatrix(selection_, worldMatrix)) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
            activeGizmoEntity_ = {};
        }
        return false;
    }

    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    DirectX::XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (gizmoOperation_ == GizmoOperation::Rotate) {
        operation = ImGuizmo::ROTATE;
    } else if (gizmoOperation_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
    }
    const ImGuizmo::MODE mode =
        gizmoSpace_ == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    float snapValues[3]{};
    if (gizmoOperation_ == GizmoOperation::Translate) {
        std::ranges::fill(snapValues, translationSnap_);
    } else if (gizmoOperation_ == GizmoOperation::Rotate) {
        std::ranges::fill(snapValues, rotationSnapDegrees_);
    } else {
        std::ranges::fill(snapValues, scaleSnap_);
    }
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, mode, &worldMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        BeginHistoryEdit("Transform Entity");
        activeGizmoEntity_ = selection_;
    }

    if (manipulated && activeGizmoEntity_ == selection_) {
        using namespace DirectX;
        XMMATRIX localMatrix = XMLoadFloat4x4(&worldMatrix);
        bool canApply = true;
        if (entity->parent.IsValid()) {
            XMFLOAT4X4 parentWorld{};
            if (!world_.TryGetWorldMatrix(entity->parent, parentWorld)) {
                canApply = false;
            } else {
                XMVECTOR determinant{};
                const XMMATRIX inverseParent =
                    XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
                const float determinantValue = XMVectorGetX(determinant);
                if (std::isfinite(determinantValue) && std::abs(determinantValue) > 1.0e-8f) {
                    localMatrix *= inverseParent;
                } else {
                    canApply = false;
                }
            }
        }
        TransformComponent localTransform{};
        if (canApply && TryDecomposeTransformComponent(localMatrix, localTransform)) {
            entity->transform = localTransform;
            RefreshDirty();
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        activeGizmoEntity_ = {};
        status_ = "Transformed entity from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
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
