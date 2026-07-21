#include "EditorScene.h"

#include "AssetImportPlanner.h"

#include "core/AssetManager.h"
#include "graphics/DirectXCommon.h"
#include "graphics/LightingScene.h"
#include "graphics/RenderScene.h"
#include "graphics/SrvManager.h"
#include "imgui/ImguiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

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
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;

constexpr const char* kPrimitiveNames[] = {"Box", "Sphere", "Plane", "Cylinder"};
constexpr const char* kEntityDragPayload = "EDITOR_ENTITY";
constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
constexpr size_t kMaxHistoryEntries = 128;
constexpr size_t kMaxRecentScenes = 10;

bool ContainsCaseInsensitive(std::string value, std::string query) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(query, query.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value.find(query) != std::string::npos;
}

std::string LowercaseAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
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

bool IsPathAtOrWithinRoot(const std::filesystem::path& root,
                          const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    return !error &&
           (canonicalPath == canonicalRoot || IsPathWithinRoot(canonicalRoot, canonicalPath));
}

bool IsValidAssetFilename(std::string_view filename) {
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.ends_with('.') || filename.ends_with(' ')) {
        return false;
    }
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    return std::ranges::none_of(filename, [invalidCharacters](unsigned char character) {
        return character < 32u || invalidCharacters.find(static_cast<char>(character)) !=
                                      std::string_view::npos;
    });
}

std::optional<std::filesystem::path> AssetRelativeFromReference(std::string_view reference) {
    constexpr std::string_view uriPrefix = "asset://";
    constexpr std::string_view projectPrefix = "assets/";
    if (reference.starts_with(uriPrefix)) {
        reference.remove_prefix(uriPrefix.size());
    } else if (reference.starts_with(projectPrefix)) {
        reference.remove_prefix(projectPrefix.size());
    } else {
        return std::nullopt;
    }
    const std::filesystem::path relative =
        std::filesystem::path(std::string(reference)).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || HasParentTraversal(relative)) {
        return std::nullopt;
    }
    return relative;
}

bool AssetPathMatches(const std::filesystem::path& candidate,
                      const std::filesystem::path& target, bool directory) {
    const std::string candidateText = candidate.lexically_normal().generic_string();
    const std::string targetText = target.lexically_normal().generic_string();
    return candidateText == targetText ||
           (directory && candidateText.starts_with(targetText + '/'));
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

bool ProjectScenePoint(const Camera& camera, const DirectX::XMFLOAT3& worldPosition,
                       const ImVec2& imageMin, const ImVec2& imageMax, ImVec2& screenPosition,
                       bool requireInside = true) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
        DirectX::XMVectorSet(worldPosition.x, worldPosition.y, worldPosition.z, 1.0f),
        camera.GetViewProjection());
    const float clipW = DirectX::XMVectorGetW(clip);
    if (!std::isfinite(clipW) || clipW <= 1.0e-5f) {
        return false;
    }
    const float ndcX = DirectX::XMVectorGetX(clip) / clipW;
    const float ndcY = DirectX::XMVectorGetY(clip) / clipW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || std::abs(ndcX) > 10000.0f ||
        std::abs(ndcY) > 10000.0f) {
        return false;
    }
    screenPosition = {imageMin.x + (ndcX * 0.5f + 0.5f) * width,
                      imageMin.y + (0.5f - ndcY * 0.5f) * height};
    return !requireInside ||
           (screenPosition.x >= imageMin.x && screenPosition.x <= imageMax.x &&
            screenPosition.y >= imageMin.y && screenPosition.y <= imageMax.y);
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
                         std::filesystem::path imguiSettingsPath,
                         std::function<void()> requestClose)
    : requestClose_(std::move(requestClose)), projectRoot_(std::move(projectRoot)),
      assetRoot_(std::move(assetRoot)), sceneRoot_(std::move(sceneRoot)),
      imguiSettingsPath_(std::move(imguiSettingsPath)),
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
    if (ctx.systems.imgui == nullptr ||
        !ctx.systems.imgui->ConfigureDocking(imguiSettingsPath_)) {
        status_ = "Could not configure the Editor docking layout.";
    }
    if (ctx.rendering.dxCommon == nullptr || ctx.rendering.srv == nullptr ||
        !sceneViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Scene View RenderSurface initialization failed.";
        return;
    }
    if (!gameViewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 960, 540)) {
        status_ = "Game View RenderSurface initialization failed.";
    }
    if (!assetPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 320)) {
        status_ = "Asset Preview RenderSurface initialization failed.";
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
    gameViewCamera_.Initialize(960.0f / 540.0f);
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
    assetPreviewCamera_.SetRotation({0.0f, 0.0f, 0.0f});
    assetPreviewCamera_.Initialize(1.0f);
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
    if (gameViewSurface_.IsReady() && gameViewPostProcess_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording() &&
        (requestedGameWidth_ != gameViewSurface_.GetWidth() ||
         requestedGameHeight_ != gameViewSurface_.GetHeight())) {
        const int width = (std::max)(1, requestedGameWidth_);
        const int height = (std::max)(1, requestedGameHeight_);
        if (!gameViewSurface_.Resize(width, height) ||
            !gameViewPostProcess_.Resize(width, height)) {
            status_ = "Game View resize failed.";
        }
    }
    if (!gamePostProcessInitializationAttempted_ && gameViewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr &&
        ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        gamePostProcessInitializationAttempted_ = true;
        gameViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                        gameViewSurface_.GetWidth(),
                                        gameViewSurface_.GetHeight());
        if (!gameViewPostProcess_.IsReady()) {
            status_ = "Game View PostProcess initialization failed.";
        }
    }
    if (!assetPreviewPostProcessInitializationAttempted_ && assetPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        assetPreviewPostProcessInitializationAttempted_ = true;
        assetPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                            assetPreviewSurface_.GetWidth(),
                                            assetPreviewSurface_.GetHeight());
        if (!assetPreviewPostProcess_.IsReady()) {
            status_ = "Asset Preview PostProcess initialization failed.";
        }
    }
    UpdateAssetPreview();
}

void EditorScene::SubmitLighting(LightingScene& lightingScene) {
    SceneLighting lighting{};
    const bool hasWorldLights = std::ranges::any_of(world_.Entities(), [](const WorldEntity& entity) {
        return entity.light && entity.light->enabled && entity.light->intensity > 0.0f;
    });
    if (!hasWorldLights) {
        lightingScene.SetSceneLighting(lighting);
        return;
    }

    lighting.keyLightColor = {0.0f, 0.0f, 0.0f, 0.0f};
    for (PointLight& pointLight : lighting.pointLights) {
        pointLight.positionRange.w = 0.0f;
        pointLight.colorIntensity.w = 0.0f;
    }
    lighting.spotLight.positionRange.w = 0.0f;
    lighting.spotLight.colorIntensity.w = 0.0f;
    lighting.spotLight.angleParams.w = 0.0f;

    bool directionalAssigned = false;
    size_t pointLightIndex = 0u;
    bool spotAssigned = false;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.light || !entity.light->enabled || entity.light->intensity <= 0.0f) {
            continue;
        }
        DirectX::XMFLOAT4X4 storedWorld{};
        if (!world_.TryGetWorldMatrix(entity.id, storedWorld)) {
            continue;
        }
        const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&storedWorld);
        DirectX::XMVECTOR direction = DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), world);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction)) <= 1.0e-8f) {
            continue;
        }
        direction = DirectX::XMVector3Normalize(direction);
        DirectX::XMFLOAT3 storedDirection{};
        DirectX::XMStoreFloat3(&storedDirection, direction);
        const LightComponent& component = *entity.light;
        if (component.type == LightType::Directional && !directionalAssigned) {
            lighting.keyLightDirection = storedDirection;
            lighting.keyLightColor = {component.color.x * component.intensity,
                                      component.color.y * component.intensity,
                                      component.color.z * component.intensity, 1.0f};
            directionalAssigned = true;
        } else if (component.type == LightType::Point &&
                   pointLightIndex < lighting.pointLights.size()) {
            PointLight& point = lighting.pointLights[pointLightIndex++];
            point.positionRange = {storedWorld._41, storedWorld._42, storedWorld._43,
                                   component.range};
            point.colorIntensity = {component.color.x, component.color.y, component.color.z,
                                    component.intensity};
        } else if (component.type == LightType::Spot && !spotAssigned) {
            SpotLight& spot = lighting.spotLight;
            spot.positionRange = {storedWorld._41, storedWorld._42, storedWorld._43,
                                  component.range};
            spot.direction = {storedDirection.x, storedDirection.y, storedDirection.z, 0.0f};
            spot.colorIntensity = {component.color.x, component.color.y, component.color.z,
                                   component.intensity};
            spot.angleParams = {
                std::cos(DirectX::XMConvertToRadians(component.innerAngleDegrees)),
                std::cos(DirectX::XMConvertToRadians(component.outerAngleDegrees)), 2.4f, 1.0f};
            spotAssigned = true;
        }
    }
    lightingScene.SetSceneLighting(lighting);
}

void EditorScene::Draw() {}

void EditorScene::DrawPostProcessOverlay() {
    ImGuizmo::BeginFrame();
    CaptureConsoleStatus();
    HandleEditorShortcuts();
    DrawMainMenu();
    DrawDockSpace();
    DrawUnsavedChangesDialog();
    DrawEntityRenameDialog();
    DrawAssetOperationDialogs();
    DrawPanels();
    CaptureConsoleStatus();
}

bool EditorScene::OnCloseRequested() {
    if (!dirty_) {
        return true;
    }
    RequestSceneAction(PendingSceneAction::Exit);
    return false;
}

void EditorScene::OnFilesDropped(std::span<const std::filesystem::path> files, int screenX,
                                 int screenY) {
    const bool overProject = static_cast<float>(screenX) >= projectPanelMinX_ &&
                             static_cast<float>(screenX) < projectPanelMaxX_ &&
                             static_cast<float>(screenY) >= projectPanelMinY_ &&
                             static_cast<float>(screenY) < projectPanelMaxY_;
    if (!overProject) {
        status_ = "Drop model files onto the Project panel to import them.";
        return;
    }
    ImportAssetFiles(std::vector<std::filesystem::path>(files.begin(), files.end()));
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
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !world_.Empty())) {
            SelectAllHierarchyEntities();
        }
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
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Hierarchy", nullptr, &showHierarchyPanel_);
            ImGui::MenuItem("Project", nullptr, &showProjectPanel_);
            ImGui::MenuItem("Scene", nullptr, &showScenePanel_);
            ImGui::MenuItem("Game", nullptr, &showGamePanel_);
            ImGui::MenuItem("Console", nullptr, &showConsolePanel_);
            ImGui::MenuItem("Inspector", nullptr, &showInspectorPanel_);
            ImGui::Separator();
            if (ImGui::MenuItem("Show All Panels")) {
                showHierarchyPanel_ = true;
                showProjectPanel_ = true;
                showScenePanel_ = true;
                showGamePanel_ = true;
                showConsolePanel_ = true;
                showInspectorPanel_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Panel Layout")) {
            showHierarchyPanel_ = true;
            showProjectPanel_ = true;
            showScenePanel_ = true;
            showGamePanel_ = true;
            showConsolePanel_ = true;
            showInspectorPanel_ = true;
            resetDockLayoutRequested_ = true;
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

void EditorScene::DrawDockSpace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId = ImHashStr("LikeEngineEditorDockSpace");
    if (resetDockLayoutRequested_) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        resetDockLayoutRequested_ = false;
    }
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID mainDock = dockspaceId;
        ImGuiID leftDock{};
        ImGuiID rightDock{};
        ImGuiID projectDock{};
        ImGuiID consoleDock{};
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Left, 0.22f, &leftDock, &mainDock);
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Right, 0.31f, &rightDock, &mainDock);
        ImGui::DockBuilderSplitNode(leftDock, ImGuiDir_Down, 0.28f, &projectDock, &leftDock);
        ImGui::DockBuilderSplitNode(mainDock, ImGuiDir_Down, 0.28f, &consoleDock, &mainDock);
        ImGui::DockBuilderDockWindow("Hierarchy", leftDock);
        ImGui::DockBuilderDockWindow("Project", projectDock);
        ImGui::DockBuilderDockWindow("Game", mainDock);
        ImGui::DockBuilderDockWindow("Scene", mainDock);
        ImGui::DockBuilderDockWindow("Console", consoleDock);
        ImGui::DockBuilderDockWindow("Inspector", rightDock);
        ImGui::DockBuilderFinish(dockspaceId);
        status_ = "Initialized the default docking layout.";
    }
    ImGui::DockSpaceOverViewport(dockspaceId, viewport);
}

void EditorScene::DrawPanels() {
    sceneViewSurface_.ReleaseCompletedFrameResources();
    gameViewSurface_.ReleaseCompletedFrameResources();
    assetPreviewSurface_.ReleaseCompletedFrameResources();
    projectPanelMinX_ = 0.0f;
    projectPanelMinY_ = 0.0f;
    projectPanelMaxX_ = 0.0f;
    projectPanelMaxY_ = 0.0f;
    if (showHierarchyPanel_) {
        if (ImGui::Begin("Hierarchy", &showHierarchyPanel_, kPanelFlags)) {
            DrawHierarchyPanel();
        }
        ImGui::End();
    }

    if (showProjectPanel_) {
        if (ImGui::Begin("Project", &showProjectPanel_, kPanelFlags)) {
            DrawProjectPanel();
        }
        ImGui::End();
    }

    if (showScenePanel_) {
        if (ImGui::Begin("Scene", &showScenePanel_, kPanelFlags)) {
            DrawSceneGizmoToolbar();
            ImGui::Separator();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedSceneWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedSceneHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (sceneViewSurface_.IsReady() && sceneViewPostProcess_.IsReady() && ctx_ != nullptr &&
                ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr) {
                const ImVec2 expectedImageMin = ImGui::GetCursorScreenPos();
                const ImVec2 expectedImageMax = {
                    expectedImageMin.x + static_cast<float>(requestedSceneWidth_),
                    expectedImageMin.y + static_cast<float>(requestedSceneHeight_)};
                const bool expectedImageHovered =
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(expectedImageMin, expectedImageMax);
                HandleSceneCameraControls(expectedImageMin, expectedImageMax,
                                          expectedImageHovered);
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
                DrawSceneComponentGizmos(imageMin, imageMax);
                DrawSceneSelectionOutline(imageMin, imageMax);
                if (!DrawSceneTransformGizmo(imageMin, imageMax)) {
                    PickSceneEntity(imageMin, imageMax, imageHovered);
                }
                if (imageHovered) {
                    constexpr const char* cameraHint =
                        "RMB Look  |  WASD/QE Move  |  MMB Pan  |  Wheel Dolly  |  F Focus";
                    const ImVec2 hintSize = ImGui::CalcTextSize(cameraHint);
                    const ImVec2 hintMin = {imageMin.x + 8.0f,
                                            imageMax.y - hintSize.y - 12.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled({hintMin.x - 4.0f, hintMin.y - 2.0f},
                                            {hintMin.x + hintSize.x + 4.0f,
                                             hintMin.y + hintSize.y + 2.0f},
                                            IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230), cameraHint);
                }
            } else {
                ImGui::TextDisabled("Scene View RenderSurface is not ready.");
            }
        }
        ImGui::End();
    }

    if (showGamePanel_) {
        if (ImGui::Begin("Game", &showGamePanel_, kPanelFlags)) {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedGameWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedGameHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (!gameViewSurface_.IsReady() || !gameViewPostProcess_.IsReady() ||
                ctx_ == nullptr || ctx_->rendering.dxCommon == nullptr ||
                ctx_->rendering.model == nullptr) {
                ImGui::TextDisabled("Game View RenderSurface is not ready.");
            } else if (!UpdateGameViewCamera()) {
                ImGui::TextDisabled("No enabled Primary Camera in the scene.");
            } else {
                BuildRenderScene();
                sceneRenderer_.Render(renderScene_, gameViewCamera_, gameViewSurface_,
                                      {0.025f, 0.035f, 0.055f, 1.0f});
                gameViewSurface_.TransitionDepthToShaderResource();
                gameViewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
                const PostProcessOutputTarget target{
                    gameViewSurface_.GetOutputRtvHandle(),
                    static_cast<uint32_t>(gameViewSurface_.GetWidth()),
                    static_cast<uint32_t>(gameViewSurface_.GetHeight()),
                    DirectXCommon::kBackBufferFormat,
                };
                gameViewPostProcess_.DrawToTarget(gameViewSurface_.GetSceneColorGpuHandle(),
                                                  gameViewSurface_.GetDepthGpuHandle(), target);
                gameViewSurface_.EndOutputPass();
                gameViewSurface_.TransitionDepthToWrite();
                ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
                const D3D12_GPU_DESCRIPTOR_HANDLE output =
                    gameViewSurface_.GetOutputGpuHandle();
                ImGui::Image(static_cast<ImTextureID>(output.ptr),
                             ImVec2(static_cast<float>(requestedGameWidth_),
                                    static_cast<float>(requestedGameHeight_)));
            }
        }
        ImGui::End();
    }

    if (showConsolePanel_) {
        if (ImGui::Begin("Console", &showConsolePanel_, kPanelFlags)) {
            DrawConsolePanel();
        }
        ImGui::End();
    }

    if (showInspectorPanel_) {
        if (ImGui::Begin("Inspector", &showInspectorPanel_, kPanelFlags)) {
            DrawInspectorPanel();
        }
        ImGui::End();
    }
}

void EditorScene::CaptureConsoleStatus() {
    if (status_.empty() || status_ == lastCapturedStatus_) {
        return;
    }
    lastCapturedStatus_ = status_;
    std::string normalized = status_;
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    ConsoleSeverity severity = ConsoleSeverity::Info;
    if (normalized.find("failed") != std::string::npos ||
        normalized.find("failure") != std::string::npos ||
        normalized.find("error") != std::string::npos) {
        severity = ConsoleSeverity::Error;
    } else if (normalized.find("could not") != std::string::npos ||
               normalized.find("cannot") != std::string::npos ||
               normalized.find("invalid") != std::string::npos ||
               normalized.find("rejected") != std::string::npos ||
               normalized.find("unavailable") != std::string::npos) {
        severity = ConsoleSeverity::Warning;
    }
    consoleEntries_.push_back({status_, ImGui::GetTime(), severity});
    constexpr size_t kMaxConsoleEntries = 512u;
    if (consoleEntries_.size() > kMaxConsoleEntries) {
        consoleEntries_.erase(consoleEntries_.begin(),
                              consoleEntries_.begin() +
                                  static_cast<ptrdiff_t>(consoleEntries_.size() -
                                                         kMaxConsoleEntries));
    }
    consoleScrollToBottom_ = true;
}

void EditorScene::DrawConsolePanel() {
    size_t infoCount = 0;
    size_t warningCount = 0;
    size_t errorCount = 0;
    for (const ConsoleEntry& entry : consoleEntries_) {
        switch (entry.severity) {
        case ConsoleSeverity::Info:
            ++infoCount;
            break;
        case ConsoleSeverity::Warning:
            ++warningCount;
            break;
        case ConsoleSeverity::Error:
            ++errorCount;
            break;
        }
    }
    if (ImGui::Button("Clear")) {
        consoleEntries_.clear();
        lastCapturedStatus_ = status_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string text;
        for (const ConsoleEntry& entry : consoleEntries_) {
            const char* severity = entry.severity == ConsoleSeverity::Error
                                       ? "Error"
                                       : entry.severity == ConsoleSeverity::Warning ? "Warning"
                                                                                    : "Info";
            text += '[';
            text += severity;
            text += "] ";
            text += entry.message;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    std::string infoLabel = "Info (" + std::to_string(infoCount) + ")";
    std::string warningLabel = "Warnings (" + std::to_string(warningCount) + ")";
    std::string errorLabel = "Errors (" + std::to_string(errorCount) + ")";
    ImGui::Checkbox(infoLabel.c_str(), &showConsoleInfo_);
    ImGui::SameLine();
    ImGui::Checkbox(warningLabel.c_str(), &showConsoleWarnings_);
    ImGui::SameLine();
    ImGui::Checkbox(errorLabel.c_str(), &showConsoleErrors_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ConsoleSearch", "Search messages...", consoleSearch_.data(),
                             consoleSearch_.size());
    ImGui::Separator();

    if (ImGui::BeginChild("ConsoleMessages", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const std::string query(consoleSearch_.data());
        for (size_t index = 0; index < consoleEntries_.size(); ++index) {
            const ConsoleEntry& entry = consoleEntries_[index];
            const bool severityVisible =
                (entry.severity == ConsoleSeverity::Info && showConsoleInfo_) ||
                (entry.severity == ConsoleSeverity::Warning && showConsoleWarnings_) ||
                (entry.severity == ConsoleSeverity::Error && showConsoleErrors_);
            if (!severityVisible || (!query.empty() &&
                                     !ContainsCaseInsensitive(entry.message, query))) {
                continue;
            }
            const char* label = entry.severity == ConsoleSeverity::Error
                                    ? "Error"
                                    : entry.severity == ConsoleSeverity::Warning ? "Warning"
                                                                                 : "Info";
            const ImVec4 color = entry.severity == ConsoleSeverity::Error
                                     ? ImVec4{1.0f, 0.35f, 0.35f, 1.0f}
                                     : entry.severity == ConsoleSeverity::Warning
                                           ? ImVec4{1.0f, 0.75f, 0.25f, 1.0f}
                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextDisabled("[%7.2f]", entry.timestampSeconds);
            ImGui::SameLine();
            ImGui::TextColored(color, "[%s]", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.message.c_str());
            if (ImGui::BeginPopupContextItem("MessageContext")) {
                if (ImGui::MenuItem("Copy Message")) {
                    ImGui::SetClipboardText(entry.message.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (consoleScrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            consoleScrollToBottom_ = false;
        }
    }
    ImGui::EndChild();
}

void EditorScene::DrawProjectPanel() {
    const ImVec2 panelPosition = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetWindowSize();
    projectPanelMinX_ = panelPosition.x;
    projectPanelMinY_ = panelPosition.y;
    projectPanelMaxX_ = panelPosition.x + panelSize.x;
    projectPanelMaxY_ = panelPosition.y + panelSize.y;
    if (pendingAssetDirectory_) {
        currentAssetDirectory_ = std::move(*pendingAssetDirectory_);
        pendingAssetDirectory_.reset();
        selectedAsset_.clear();
        RefreshAssetBrowser();
    }
    if (ImGui::Button("New")) {
        ImGui::OpenPopup("AssetCreateMenu");
    }
    if (ImGui::BeginPopup("AssetCreateMenu")) {
        if (ImGui::MenuItem("Folder")) {
            RequestCreateAssetFolder();
        }
        if (ImGui::MenuItem("Import Model Files...")) {
            ImportAssetFiles();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
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

    constexpr const char* formatLabels[] = {"All formats", "glTF", "GLB", "OBJ",
                                             "FBX",         "DAE",  "3DS", "PLY"};
    constexpr const char* formatExtensions[] = {"",     ".gltf", ".glb", ".obj",
                                                 ".fbx", ".dae",  ".3ds", ".ply"};
    constexpr const char* sortLabels[] = {"Name", "Type", "Size"};
    ImGui::SetNextItemWidth(105.0f);
    ImGui::Combo("##AssetFormat", &assetFormatFilter_, formatLabels,
                 static_cast<int>(std::size(formatLabels)));
    ImGui::SameLine();
    int sortMode = static_cast<int>(assetSortMode_);
    ImGui::SetNextItemWidth(75.0f);
    if (ImGui::Combo("##AssetSort", &sortMode, sortLabels,
                     static_cast<int>(std::size(sortLabels)))) {
        assetSortMode_ = static_cast<AssetSortMode>(sortMode);
    }
    ImGui::SameLine();
    if (ImGui::Button(assetSortAscending_ ? "Asc" : "Desc")) {
        assetSortAscending_ = !assetSortAscending_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle sort direction");
    }
    ImGui::Separator();

    const auto matchesFormat = [&](const std::filesystem::path& relativePath) {
        if (assetFormatFilter_ <= 0 ||
            assetFormatFilter_ >= static_cast<int>(std::size(formatExtensions))) {
            return true;
        }
        return LowercaseAscii(relativePath.extension().string()) ==
               formatExtensions[assetFormatFilter_];
    };
    const auto fileSize = [&](const std::filesystem::path& relativePath) {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(assetRoot_ / relativePath, error);
        return error ? uintmax_t{0} : size;
    };
    const auto comparePaths = [&](const std::filesystem::path& left,
                                  const std::filesystem::path& right) {
        int result = 0;
        if (assetSortMode_ == AssetSortMode::Type) {
            result = LowercaseAscii(left.extension().string()).compare(
                LowercaseAscii(right.extension().string()));
        } else if (assetSortMode_ == AssetSortMode::Size) {
            const uintmax_t leftSize = fileSize(left);
            const uintmax_t rightSize = fileSize(right);
            result = leftSize < rightSize ? -1 : (leftSize > rightSize ? 1 : 0);
        }
        if (result == 0) {
            result = LowercaseAscii(left.filename().string())
                         .compare(LowercaseAscii(right.filename().string()));
        }
        return assetSortAscending_ ? result < 0 : result > 0;
    };

    const float detailsHeight = selectedAsset_.empty() ? 0.0f : 190.0f;
    if (ImGui::BeginChild("AssetBrowserEntries", {0.0f, -detailsHeight},
                          ImGuiChildFlags_None)) {
        const std::string search(assetSearch_.data());
        if (!search.empty()) {
            std::vector<std::filesystem::path> matches;
            for (const std::filesystem::path& logicalPath : modelAssets_) {
                const std::filesystem::path relativePath =
                    logicalPath.lexically_relative("assets");
                if (ContainsCaseInsensitive(logicalPath.generic_string(), search) &&
                    matchesFormat(relativePath)) {
                    matches.push_back(relativePath);
                }
            }
            std::ranges::sort(matches, comparePaths);
            for (const std::filesystem::path& relativePath : matches) {
                DrawAssetBrowserEntry(relativePath, false);
            }
            if (matches.empty()) {
                ImGui::TextDisabled("No matching model assets.");
            }
        } else {
            std::vector<AssetBrowserEntry> visibleEntries;
            std::ranges::copy_if(assetBrowserEntries_, std::back_inserter(visibleEntries),
                                 [&](const AssetBrowserEntry& entry) {
                                     return entry.directory || matchesFormat(entry.relativePath);
                                 });
            std::ranges::sort(visibleEntries, [&](const AssetBrowserEntry& left,
                                                  const AssetBrowserEntry& right) {
                if (left.directory != right.directory) {
                    return left.directory;
                }
                return comparePaths(left.relativePath, right.relativePath);
            });
            for (const AssetBrowserEntry& entry : visibleEntries) {
                DrawAssetBrowserEntry(entry.relativePath, entry.directory);
            }
            if (visibleEntries.empty()) {
                ImGui::TextDisabled("This folder contains no matching model assets or folders.");
            }
        }
    }
    ImGui::EndChild();
    if (!selectedAsset_.empty()) {
        DrawSelectedAssetDetails();
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
        selectedAsset_ = relativePath;
        if (directory) {
            if (ImGui::MenuItem("Open")) {
                NavigateAssetBrowser(relativePath);
            }
        } else if (ImGui::MenuItem("Create Entity")) {
            CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
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
        if (ImGui::MenuItem("Select Referencing Entities", nullptr, false,
                            references != 0u)) {
            SelectAssetReferences(relativePath, directory);
        }
        if (!directory) {
            ImGui::Separator();
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

void EditorScene::DrawSelectedAssetDetails() {
    ImGui::SeparatorText("Selected Asset");
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    const std::filesystem::path physical = assetRoot_ / relative;
    const std::string logicalPath =
        (std::filesystem::path("assets") / relative).lexically_normal().generic_string();
    ImGui::TextWrapped("%s", logicalPath.c_str());

    std::error_code error;
    const bool directory = std::filesystem::is_directory(physical, error) && !error;
    error.clear();
    const bool regularFile = std::filesystem::is_regular_file(physical, error) && !error;
    if (!directory && !regularFile) {
        ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Asset no longer exists.");
        return;
    }
    const std::string extension = physical.extension().string();
    std::string typeLabel = directory ? "Folder" : "Model";
    if (regularFile && !extension.empty()) {
        typeLabel += " (" + extension + ")";
    }
    ImGui::TextDisabled("Type: %s", typeLabel.c_str());
    if (regularFile) {
        const uintmax_t bytes = std::filesystem::file_size(physical, error);
        if (!error) {
            constexpr double kilobyte = 1024.0;
            constexpr double megabyte = kilobyte * 1024.0;
            if (bytes >= static_cast<uintmax_t>(megabyte)) {
                ImGui::SameLine();
                ImGui::TextDisabled("Size: %.2f MB", static_cast<double>(bytes) / megabyte);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("Size: %.1f KB", static_cast<double>(bytes) / kilobyte);
            }
        }
    }
    const size_t references = CountAssetReferences(relative, directory);
    ImGui::TextDisabled("Scene references: %zu", references);
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
    if (!regularFile || !AssetImport::IsModelFile(physical)) {
        return;
    }

    if (assetPreviewAsset_ != relative) {
        ImGui::TextDisabled("Dependencies: Analyzing...");
        return;
    }
    if (!assetPreviewError_.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Dependencies: Invalid");
        ImGui::SameLine();
        if (ImGui::SmallButton("Details##AssetDependencyError")) {
            status_ = assetPreviewError_;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", assetPreviewError_.c_str());
        }
        return;
    }
    const size_t dependencyCount =
        assetPreviewPlan_.empty() ? 0u : assetPreviewPlan_.size() - 1u;
    ImGui::TextDisabled("Dependencies: %zu", dependencyCount);
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
    if (ImGui::BeginPopup("AssetDependencies")) {
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
    DrawAssetPreviewPopup();
}

void EditorScene::DrawAssetPreviewPopup() {
    ImGui::SetNextWindowSize({360.0f, 460.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopup("Model Preview")) {
        return;
    }
    const std::string filename = assetPreviewAsset_.filename().string();
    ImGui::TextUnformatted(filename.c_str());
    ImGui::Separator();
    if (!assetPreviewModel_.IsValid() || !assetPreviewSurface_.IsReady() ||
        !assetPreviewPostProcess_.IsReady() || ctx_ == nullptr ||
        ctx_->rendering.dxCommon == nullptr || ctx_->rendering.model == nullptr) {
        ImGui::TextDisabled("Model preview is not ready.");
        ImGui::EndPopup();
        return;
    }

    const Model* model = ctx_->rendering.model->GetModel(assetPreviewModel_);
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    std::unordered_set<uint32_t> materials;
    if (model != nullptr) {
        for (const ModelSubMesh& subMesh : model->subMeshes) {
            vertexCount += subMesh.vertexCount;
            if (IsValidResourceId(subMesh.meshId)) {
                triangleCount += ctx_->rendering.model->GetMesh(subMesh.meshId).indexCount / 3u;
            }
            if (IsValidResourceId(subMesh.materialId)) {
                materials.insert(subMesh.materialId);
            }
        }
        if (model->subMeshes.empty() && IsValidResourceId(model->meshId)) {
            const Mesh& mesh = ctx_->rendering.model->GetMesh(model->meshId);
            vertexCount = mesh.vertexStride == 0u ? 0u : mesh.vertexBytes / mesh.vertexStride;
            triangleCount = mesh.indexCount / 3u;
            if (IsValidResourceId(model->materialId)) {
                materials.insert(model->materialId);
            }
        }
        ImGui::TextDisabled("Meshes: %zu   Vertices: %llu   Triangles: %llu",
                            model->subMeshes.empty() ? size_t{1} : model->subMeshes.size(),
                            static_cast<unsigned long long>(vertexCount),
                            static_cast<unsigned long long>(triangleCount));
        ImGui::TextDisabled("Materials: %zu   Animations: %zu   Bones: %zu", materials.size(),
                            model->animations.size(), model->bones.size());
    }

    BuildAssetPreviewScene();
    sceneRenderer_.Render(assetPreviewScene_, assetPreviewCamera_, assetPreviewSurface_,
                          {0.035f, 0.045f, 0.065f, 1.0f});
    assetPreviewSurface_.TransitionDepthToShaderResource();
    assetPreviewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
    const PostProcessOutputTarget target{
        assetPreviewSurface_.GetOutputRtvHandle(),
        static_cast<uint32_t>(assetPreviewSurface_.GetWidth()),
        static_cast<uint32_t>(assetPreviewSurface_.GetHeight()),
        DirectXCommon::kBackBufferFormat,
    };
    assetPreviewPostProcess_.DrawToTarget(assetPreviewSurface_.GetSceneColorGpuHandle(),
                                          assetPreviewSurface_.GetDepthGpuHandle(), target);
    assetPreviewSurface_.EndOutputPass();
    assetPreviewSurface_.TransitionDepthToWrite();
    ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
    const D3D12_GPU_DESCRIPTOR_HANDLE output = assetPreviewSurface_.GetOutputGpuHandle();
    ImGui::Image(static_cast<ImTextureID>(output.ptr), {320.0f, 320.0f});
    ImGui::EndPopup();
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
        status_ = "Asset rename cannot change a model file extension.";
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
    const bool containsModel =
        std::ranges::any_of(selectedFiles, [](const std::filesystem::path& path) {
            return AssetImport::IsModelFile(path);
        });
    if (!containsModel) {
        status_ = "Asset import requires at least one supported model file.";
        return false;
    }
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

    const auto firstModel =
        std::ranges::find_if(selectedFiles, [](const std::filesystem::path& path) {
            return AssetImport::IsModelFile(path);
        });
    selectedAsset_ =
        (currentAssetDirectory_ / firstModel->filename()).lexically_normal();
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
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model) {
            continue;
        }
        const std::optional<std::filesystem::path> referenced =
            AssetRelativeFromReference(entity.meshRenderer->modelPath);
        if (!referenced || !AssetPathMatches(*referenced, relativePath, directory)) {
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
    size_t references = 0;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model) {
            continue;
        }
        const std::optional<std::filesystem::path> referenced =
            AssetRelativeFromReference(entity.meshRenderer->modelPath);
        if (referenced && AssetPathMatches(*referenced, relativePath, directory)) {
            ++references;
        }
    }
    return references;
}

size_t EditorScene::UpdateAssetReferences(const std::filesystem::path& oldRelativePath,
                                          const std::filesystem::path& newRelativePath,
                                          bool directory) {
    size_t updated = 0;
    for (const WorldEntity& candidate : world_.Entities()) {
        WorldEntity* entity = world_.Find(candidate.id);
        if (entity == nullptr || !entity->meshRenderer ||
            entity->meshRenderer->sourceType != MeshSourceType::Model) {
            continue;
        }
        const std::optional<std::filesystem::path> referenced =
            AssetRelativeFromReference(entity->meshRenderer->modelPath);
        if (!referenced || !AssetPathMatches(*referenced, oldRelativePath, directory)) {
            continue;
        }
        const std::filesystem::path suffix = referenced->lexically_relative(oldRelativePath);
        const std::filesystem::path replacement =
            suffix.empty() || suffix == L"." ? newRelativePath : newRelativePath / suffix;
        entity->meshRenderer->modelPath =
            "asset://" + replacement.lexically_normal().generic_string();
        ++updated;
    }
    return updated;
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
    if (ImGui::Selectable("Scene Root (drop here)", false)) {
        ClearHierarchySelection();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize == sizeof(EntityId)) {
            EntityId child{};
            std::memcpy(&child, payload->Data, sizeof(child));
            ReparentSelection(child, {});
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered()) {
        ClearHierarchySelection();
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
    const bool hasMeshRenderer = entity->meshRenderer.has_value();
    bool rendererEnabled = hasMeshRenderer && entity->meshRenderer->enabled;
    if (hasMeshRenderer) {
        if (ImGui::Checkbox("##RendererVisible", &rendererEnabled)) {
            SetSelectedMeshRenderersEnabled(id, rendererEnabled);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(rendererEnabled ? "Hide MeshRenderer" : "Show MeshRenderer");
        }
    } else {
        ImGui::Dummy({ImGui::GetFrameHeight(), ImGui::GetFrameHeight()});
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (hasMeshRenderer && !rendererEnabled) {
        const ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              {textColor.x, textColor.y, textColor.z, textColor.w * 0.45f});
    }
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    const ImVec2 nodeMin = ImGui::GetItemRectMin();
    const ImVec2 nodeMax = ImGui::GetItemRectMax();
    if (hasMeshRenderer && !rendererEnabled) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemClicked()) {
        const ImGuiIO& io = ImGui::GetIO();
        SelectHierarchyEntity(id, io.KeyCtrl, io.KeyShift);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        SelectHierarchyEntity(id, false, false);
        FocusSceneCameraOnSelection();
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
        if (ImGui::MenuItem("Focus in Scene", "F")) {
            SelectHierarchyEntity(id, false, false);
            FocusSceneCameraOnSelection();
        }
        if (entity->meshRenderer &&
            ImGui::MenuItem("Renderer Enabled", nullptr, entity->meshRenderer->enabled)) {
            SetSelectedMeshRenderersEnabled(id, !entity->meshRenderer->enabled);
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
        if (IsHierarchyEntitySelected(id) && hierarchySelection_.size() > 1u) {
            ImGui::Text("Move %zu selected entities", hierarchySelection_.size());
        } else {
            ImGui::TextUnformatted(entity->name.c_str());
        }
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
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
    if (!entity->meshRenderer || !entity->camera || !entity->light) {
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("AddComponentMenu");
        }
        if (ImGui::BeginPopup("AddComponentMenu")) {
            if (!entity->meshRenderer && ImGui::MenuItem("Mesh Renderer")) {
                const std::string before = WorldSerializer::Serialize(world_);
                const EntityId selectionBefore = selection_;
                entity->meshRenderer = MeshRendererComponent{};
                RecordImmediateEdit("Add MeshRenderer", before, selectionBefore);
                status_ = "Added MeshRenderer.";
            }
            if (!entity->camera && ImGui::MenuItem("Camera")) {
                const std::string before = WorldSerializer::Serialize(world_);
                const EntityId selectionBefore = selection_;
                entity->camera = CameraComponent{};
                RecordImmediateEdit("Add Camera", before, selectionBefore);
                status_ = "Added Camera.";
            }
            if (!entity->light && ImGui::MenuItem("Light")) {
                const std::string before = WorldSerializer::Serialize(world_);
                const EntityId selectionBefore = selection_;
                entity->light = LightComponent{};
                RecordImmediateEdit("Add Light", before, selectionBefore);
                status_ = "Added Light.";
            }
            ImGui::EndPopup();
        }
    }

    if (entity->camera) {
        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Remove Camera")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->camera.reset();
            RecordImmediateEdit("Remove Camera", before, selectionBefore);
            status_ = "Removed Camera.";
        } else {
            CameraComponent& camera = *entity->camera;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Camera", &camera.enabled)) {
                RecordImmediateEdit("Toggle Camera", std::move(before), selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Primary##Camera", &camera.primary)) {
                if (camera.primary) {
                    world_.SetPrimaryCamera(entity->id);
                }
                RecordImmediateEdit("Change Primary Camera", std::move(before), selectionBefore);
            }
            int projection = static_cast<int>(camera.projection);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Projection##Camera", &projection,
                             "Perspective\0Orthographic\0")) {
                camera.projection = static_cast<CameraProjection>(projection);
                RecordImmediateEdit("Change Camera Projection", std::move(before),
                                    selectionBefore);
            }
            auto drawCameraFloat = [&](const char* label, float& value, float speed,
                                       float minimum, float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Camera.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Camera");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            if (camera.projection == CameraProjection::Perspective) {
                drawCameraFloat("Field of View", camera.fieldOfViewDegrees, 0.25f, 1.0f,
                                179.0f, "%.1f deg");
            } else {
                drawCameraFloat("Orthographic Height", camera.orthographicHeight, 0.05f,
                                0.001f, 1000000.0f, "%.3f");
            }
            drawCameraFloat("Near Clip", camera.nearClip, 0.005f, 0.001f,
                            (std::max)(0.001f, camera.farClip - 0.001f), "%.3f");
            drawCameraFloat("Far Clip", camera.farClip, 0.5f,
                            camera.nearClip + 0.001f, 1000000000.0f, "%.1f");
        }
    }

    if (entity->light) {
        ImGui::SeparatorText("Light");
        if (ImGui::Button("Remove Light")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->light.reset();
            RecordImmediateEdit("Remove Light", before, selectionBefore);
            status_ = "Removed Light.";
        } else {
            LightComponent& light = *entity->light;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Light", &light.enabled)) {
                RecordImmediateEdit("Toggle Light", std::move(before), selectionBefore);
            }
            int type = static_cast<int>(light.type);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Type##Light", &type, "Directional\0Point\0Spot\0")) {
                light.type = static_cast<LightType>(type);
                RecordImmediateEdit("Change Light Type", std::move(before), selectionBefore);
            }
            if (ImGui::ColorEdit3("Color##Light", &light.color.x,
                                  ImGuiColorEditFlags_Float)) {
                RefreshDirty();
                status_ = "Modified Light.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Light Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawLightFloat = [&](const char* label, float& value, float speed,
                                      float minimum, float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Light.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Light");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawLightFloat("Intensity##Light", light.intensity, 0.02f, 0.0f, 1000000.0f,
                           "%.2f");
            if (light.type != LightType::Directional) {
                drawLightFloat("Range##Light", light.range, 0.05f, 0.001f, 1000000.0f,
                               "%.2f");
            }
            if (light.type == LightType::Spot) {
                drawLightFloat("Inner Angle##Light", light.innerAngleDegrees, 0.25f, 0.0f,
                               (std::max)(0.0f, light.outerAngleDegrees - 0.1f), "%.1f deg");
                drawLightFloat("Outer Angle##Light", light.outerAngleDegrees, 0.25f,
                               light.innerAngleDegrees + 0.1f, 179.0f, "%.1f deg");
            }
        }
    }

    if (!entity->meshRenderer) {
        return;
    }
    ImGui::SeparatorText("Mesh Renderer");
    MeshRendererComponent& renderer = *entity->meshRenderer;
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
    if (io.WantTextInput || sceneCameraNavigating_ || sceneCameraPanning_ ||
        pendingSceneAction_ != PendingSceneAction::None) {
        return;
    }
    if (!io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ClearHierarchySelection();
        } else if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
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
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        SelectAllHierarchyEntities();
    } else if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
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

void EditorScene::SetSelectedMeshRenderersEnabled(EntityId source, bool enabled) {
    if (!world_.Contains(source)) {
        return;
    }
    SynchronizeHierarchySelection();
    std::vector<EntityId> targets;
    if (hierarchySelection_.contains(source)) {
        targets.reserve(hierarchySelection_.size());
        for (const WorldEntity& entity : world_.Entities()) {
            if (hierarchySelection_.contains(entity.id) && entity.meshRenderer &&
                entity.meshRenderer->enabled != enabled) {
                targets.push_back(entity.id);
            }
        }
    } else {
        const WorldEntity* entity = world_.Find(source);
        if (entity != nullptr && entity->meshRenderer &&
            entity->meshRenderer->enabled != enabled) {
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
        if (entity != nullptr && entity->meshRenderer) {
            entity->meshRenderer->enabled = enabled;
        }
    }
    RecordImmediateEdit(enabled ? "Show Mesh Renderers" : "Hide Mesh Renderers", before,
                        selectionBefore);
    if (targets.size() == 1u) {
        status_ = enabled ? "Enabled the MeshRenderer." : "Disabled the MeshRenderer.";
    } else {
        status_ = enabled ? "Enabled the selected MeshRenderers."
                          : "Disabled the selected MeshRenderers.";
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
            }
            status_ = "Cannot create a cyclic or invalid hierarchy.";
            return;
        }
        WorldEntity* reparented = world_.Find(transform.entity);
        if (reparented == nullptr) {
            World restored;
            if (WorldSerializer::Deserialize(before, restored, nullptr)) {
                world_ = std::move(restored);
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
    if (!AssetImport::IsModelFile(path)) {
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

void EditorScene::HandleSceneCameraControls(const ImVec2& imageMin,
                                            const ImVec2& imageMax,
                                            bool imageHovered) {
    ImGuiIO& io = ImGui::GetIO();
    if (imageHovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FocusSceneCameraOnSelection();
    }
    const bool beginLook =
        imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool beginPan =
        imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if (beginLook) {
        sceneCameraNavigating_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        sceneCameraNavigating_ = false;
    }
    if (beginPan) {
        sceneCameraPanning_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        sceneCameraPanning_ = false;
    }

    const int cursorCenterX =
        static_cast<int>(std::lround((imageMin.x + imageMax.x) * 0.5f));
    const int cursorCenterY =
        static_cast<int>(std::lround((imageMin.y + imageMax.y) * 0.5f));
    const bool beginCapture = (beginLook || beginPan) && !sceneCameraCursorCaptured_;
    if (beginCapture) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            sceneCameraCursorRestoreX_ = cursor.x;
            sceneCameraCursorRestoreY_ = cursor.y;
        }
        sceneCameraPointerTravel_ = 0.0f;
        sceneCameraCursorCaptured_ = true;
        SetCursorPos(cursorCenterX, cursorCenterY);
    }

    float pointerDeltaX = 0.0f;
    float pointerDeltaY = 0.0f;
    if (sceneCameraCursorCaptured_ && !beginCapture &&
        (sceneCameraNavigating_ || sceneCameraPanning_)) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            pointerDeltaX = static_cast<float>(cursor.x - cursorCenterX);
            pointerDeltaY = static_cast<float>(cursor.y - cursorCenterY);
            sceneCameraPointerTravel_ +=
                std::sqrt(pointerDeltaX * pointerDeltaX + pointerDeltaY * pointerDeltaY);
        }
        SetCursorPos(cursorCenterX, cursorCenterY);
    }
    if (sceneCameraCursorCaptured_ && !sceneCameraNavigating_ && !sceneCameraPanning_) {
        SetCursorPos(sceneCameraCursorRestoreX_, sceneCameraCursorRestoreY_);
        sceneCameraCursorCaptured_ = false;
    }

    DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    bool rotationChanged = false;
    if (sceneCameraNavigating_) {
        constexpr float mouseSensitivity = 0.004f;
        rotation.x = std::clamp(rotation.x + pointerDeltaY * mouseSensitivity,
                                -DirectX::XM_PIDIV2 + 0.01f,
                                DirectX::XM_PIDIV2 - 0.01f);
        rotation.y += pointerDeltaX * mouseSensitivity;
        rotationChanged = pointerDeltaX != 0.0f || pointerDeltaY != 0.0f;
        if (rotationChanged) {
            sceneViewCamera_.SetRotation(rotation);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    const DirectX::XMVECTOR right = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation);
    const DirectX::XMVECTOR up = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), orientation);
    const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation);
    DirectX::XMVECTOR movement = DirectX::XMVectorZero();
    if (sceneCameraNavigating_) {
        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            movement = DirectX::XMVectorAdd(movement, forward);
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            movement = DirectX::XMVectorSubtract(movement, forward);
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            movement = DirectX::XMVectorAdd(movement, right);
        }
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            movement = DirectX::XMVectorSubtract(movement, right);
        }
        if (ImGui::IsKeyDown(ImGuiKey_E)) {
            movement = DirectX::XMVectorAdd(movement, up);
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) {
            movement = DirectX::XMVectorSubtract(movement, up);
        }
    }

    const float movementLengthSquared =
        DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(movement));
    DirectX::XMVECTOR position =
        DirectX::XMLoadFloat3(&sceneViewCamera_.GetPosition());
    bool positionChanged = false;
    if (movementLengthSquared > 0.0f) {
        const float deltaTime = std::clamp(io.DeltaTime, 0.0f, 0.1f);
        const float speed = io.KeyShift ? 12.0f : 4.0f;
        movement = DirectX::XMVectorScale(DirectX::XMVector3Normalize(movement),
                                          speed * deltaTime);
        position = DirectX::XMVectorAdd(position, movement);
        positionChanged = true;
    }
    if (sceneCameraPanning_ &&
        (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f)) {
        constexpr float panSensitivity = 0.01f;
        position = DirectX::XMVectorAdd(
            position,
            DirectX::XMVectorScale(right, -pointerDeltaX * panSensitivity));
        position = DirectX::XMVectorAdd(
            position,
            DirectX::XMVectorScale(up, pointerDeltaY * panSensitivity));
        positionChanged = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (imageHovered && io.MouseWheel != 0.0f) {
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(forward, io.MouseWheel * 0.75f));
        positionChanged = true;
    }
    if (positionChanged) {
        DirectX::XMFLOAT3 updatedPosition{};
        DirectX::XMStoreFloat3(&updatedPosition, position);
        sceneViewCamera_.SetPosition(updatedPosition);
    }
}

bool EditorScene::FocusSceneCameraOnSelection() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (entity == nullptr || !world_.TryGetWorldMatrix(selection_, worldMatrix)) {
        status_ = "Select an entity before focusing the Scene camera.";
        return false;
    }

    DirectX::XMFLOAT3 localCenter{};
    float radius = 1.0f;
    if (entity->meshRenderer && ctx_ != nullptr && ctx_->rendering.model != nullptr) {
        const ModelHandle handle = ResolveModel(*entity->meshRenderer);
        const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
        DirectX::XMFLOAT3 boundsMin{};
        DirectX::XMFLOAT3 boundsMax{};
        if (model != nullptr && TryGetModelBounds(*model, boundsMin, boundsMax)) {
            localCenter = {(boundsMin.x + boundsMax.x) * 0.5f,
                           (boundsMin.y + boundsMax.y) * 0.5f,
                           (boundsMin.z + boundsMax.z) * 0.5f};
            const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
            const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
            const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
            radius = (std::max)(0.1f, std::sqrt(extentX * extentX + extentY * extentY +
                                               extentZ * extentZ));
        }
    }

    const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&worldMatrix);
    const DirectX::XMVECTOR center = DirectX::XMVector3TransformCoord(
        DirectX::XMLoadFloat3(&localCenter), world);
    const float scaleX = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[0]));
    const float scaleY = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[1]));
    const float scaleZ = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[2]));
    radius *= (std::max)({scaleX, scaleY, scaleZ, 0.001f});

    const DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation);
    const float distance =
        (std::max)(1.0f, radius / std::tan(sceneViewCamera_.GetFovY() * 0.5f) * 1.25f);
    const DirectX::XMVECTOR position =
        DirectX::XMVectorSubtract(center, DirectX::XMVectorScale(forward, distance));
    DirectX::XMFLOAT3 focusedPosition{};
    DirectX::XMStoreFloat3(&focusedPosition, position);
    sceneViewCamera_.SetPosition(focusedPosition);
    sceneViewCamera_.SetClipRange(0.01f,
                                  (std::max)(1000.0f, distance + radius * 4.0f));
    status_ = "Focused the Scene camera on " + entity->name + ".";
    return true;
}

void EditorScene::HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                         bool imageHovered) {
    const bool rightClick = sceneCameraPointerTravel_ <= 3.0f;
    if (imageHovered && rightClick && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        sceneContextCreatePosition_ = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax,
            {static_cast<float>(sceneCameraCursorRestoreX_),
             static_cast<float>(sceneCameraCursorRestoreY_)});
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
    const std::optional<std::filesystem::path> physicalPath = ResolveProjectAssetPath(path);
    std::vector<AssetImport::File> importPlan;
    std::string importError;
    if (!physicalPath ||
        !AssetImport::BuildPlan({*physicalPath}, importPlan, importError)) {
        status_ = "Could not create model entity: " + importError;
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
    assetPreviewAsset_.clear();
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
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
                   AssetImport::IsModelFile(entry.path())) {
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
        if (iterator->is_regular_file(error) && !error &&
            AssetImport::IsModelFile(iterator->path())) {
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

bool EditorScene::UpdateGameViewCamera() {
    const WorldEntity* primaryCamera = nullptr;
    for (const WorldEntity& entity : world_.Entities()) {
        if (entity.camera && entity.camera->enabled && entity.camera->primary) {
            primaryCamera = &entity;
            break;
        }
    }
    if (primaryCamera == nullptr) {
        return false;
    }

    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (!world_.TryGetWorldMatrix(primaryCamera->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix),
                                        worldTransform)) {
        return false;
    }
    gameViewCamera_.SetPosition(worldTransform.position);
    gameViewCamera_.SetRotation(
        {DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    const CameraComponent& camera = *primaryCamera->camera;
    gameViewCamera_.SetAspect(static_cast<float>(gameViewSurface_.GetWidth()) /
                              static_cast<float>((std::max)(1, gameViewSurface_.GetHeight())));
    if (camera.projection == CameraProjection::Perspective) {
        gameViewCamera_.SetPerspectiveFovDeg(camera.fieldOfViewDegrees);
    } else {
        gameViewCamera_.SetOrthographicHeight(camera.orthographicHeight);
    }
    gameViewCamera_.SetClipRange(camera.nearClip, camera.farClip);
    return true;
}

void EditorScene::UpdateAssetPreview() {
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    if (relative == assetPreviewAsset_) {
        return;
    }
    assetPreviewAsset_ = relative;
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    assetPreviewTransform_ = {};
    if (relative.empty() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }

    const std::filesystem::path physical = assetRoot_ / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(physical, error) || error ||
        !AssetImport::IsModelFile(physical)) {
        return;
    }
    if (!AssetImport::BuildPlan({physical}, assetPreviewPlan_, assetPreviewError_)) {
        status_ = "Asset preview dependency validation failed: " + assetPreviewError_;
        return;
    }
    assetPreviewModel_ = ctx_->rendering.model->LoadHandle(physical.wstring());
    const Model* model = assetPreviewModel_.IsValid()
                             ? ctx_->rendering.model->GetModel(assetPreviewModel_)
                             : nullptr;
    if (model == nullptr) {
        assetPreviewError_ = "The selected model could not be loaded for preview.";
        status_ = "Asset preview failed for assets/" + relative.generic_string() +
                  ": model loading failed.";
        return;
    }

    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    if (!TryGetModelBounds(*model, boundsMin, boundsMax)) {
        assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
        assetPreviewCamera_.SetClipRange(0.01f, 1000.0f);
        return;
    }
    const DirectX::XMFLOAT3 center{
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f,
    };
    const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
    const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
    const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
    const float radius = (std::max)(0.05f, std::sqrt(extentX * extentX + extentY * extentY +
                                                     extentZ * extentZ));
    const float distance =
        (std::max)(0.25f, radius / std::tan(assetPreviewCamera_.GetFovY() * 0.5f) * 1.25f);
    assetPreviewTransform_.position = {-center.x, -center.y, -center.z};
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -distance});
    assetPreviewCamera_.SetClipRange((std::max)(0.01f, distance - radius * 2.0f),
                                     distance + radius * 4.0f);
}

void EditorScene::BuildAssetPreviewScene() {
    assetPreviewScene_.BeginFrame();
    ModelManager* models = ctx_ ? ctx_->rendering.model : nullptr;
    const Model* model = models != nullptr && assetPreviewModel_.IsValid()
                             ? models->GetModel(assetPreviewModel_)
                             : nullptr;
    if (model == nullptr || models == nullptr) {
        return;
    }
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
        item.transform = assetPreviewTransform_;
        item.textureId = textureId;
        item.normalTextureId = normalTextureId;
        assetPreviewScene_.SubmitMesh(item);
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
    if (!imageHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    EntityId closestComponent{};
    float closestComponentDistanceSquared = 14.0f * 14.0f;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.camera && !entity.light) {
            continue;
        }
        DirectX::XMFLOAT4X4 worldMatrix{};
        ImVec2 screenPosition{};
        if (!world_.TryGetWorldMatrix(entity.id, worldMatrix) ||
            !ProjectScenePoint(sceneViewCamera_,
                               {worldMatrix._41, worldMatrix._42, worldMatrix._43}, imageMin,
                               imageMax, screenPosition)) {
            continue;
        }
        const float deltaX = mouse.x - screenPosition.x;
        const float deltaY = mouse.y - screenPosition.y;
        const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
        if (distanceSquared <= closestComponentDistanceSquared) {
            closestComponent = entity.id;
            closestComponentDistanceSquared = distanceSquared;
        }
    }
    if (closestComponent.IsValid()) {
        const ImGuiIO& io = ImGui::GetIO();
        SelectHierarchyEntity(closestComponent, io.KeyCtrl, false);
        if (selection_ == closestComponent &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            FocusSceneCameraOnSelection();
        }
        return;
    }
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        if (!ImGui::GetIO().KeyCtrl) {
            ClearHierarchySelection();
        }
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
    const ImGuiIO& io = ImGui::GetIO();
    if (closest.IsValid()) {
        SelectHierarchyEntity(closest, io.KeyCtrl, false);
    } else if (!io.KeyCtrl) {
        ClearHierarchySelection();
    }
    if (closest.IsValid() && selection_ == closest &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        FocusSceneCameraOnSelection();
    }
}

void EditorScene::DrawSceneComponentGizmos(const ImVec2& imageMin,
                                           const ImVec2& imageMax) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    auto drawWorldLine = [&](const DirectX::XMFLOAT3& from, const DirectX::XMFLOAT3& to,
                             ImU32 color, float thickness = 1.25f) {
        ImVec2 screenFrom{};
        ImVec2 screenTo{};
        if (ProjectScenePoint(sceneViewCamera_, from, imageMin, imageMax, screenFrom, false) &&
            ProjectScenePoint(sceneViewCamera_, to, imageMin, imageMax, screenTo, false)) {
            drawList->AddLine(screenFrom, screenTo, color, thickness);
        }
    };
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.camera && !entity.light) {
            continue;
        }
        DirectX::XMFLOAT4X4 worldMatrix{};
        ImVec2 center{};
        if (!world_.TryGetWorldMatrix(entity.id, worldMatrix) ||
            !ProjectScenePoint(sceneViewCamera_,
                               {worldMatrix._41, worldMatrix._42, worldMatrix._43}, imageMin,
                               imageMax, center)) {
            continue;
        }
        const bool active = entity.id == selection_;
        const bool selected = hierarchySelection_.contains(entity.id);
        ImU32 color = entity.camera ? IM_COL32(90, 185, 255, 230)
                                    : IM_COL32(255, 215, 80, 230);
        if (active) {
            color = IM_COL32(255, 184, 56, 255);
        } else if (selected) {
            color = IM_COL32(90, 190, 255, 255);
        }
        const bool enabled = (entity.camera && entity.camera->enabled) ||
                             (entity.light && entity.light->enabled);
        if (!enabled) {
            color = (color & 0x00FFFFFFu) | (100u << 24u);
        }

        if (entity.camera) {
            drawList->AddRect({center.x - 8.0f, center.y - 6.0f},
                              {center.x + 5.0f, center.y + 6.0f}, color, 2.0f, 0, 1.8f);
            drawList->AddTriangle({center.x + 5.0f, center.y - 5.0f},
                                  {center.x + 12.0f, center.y - 9.0f},
                                  {center.x + 12.0f, center.y + 1.0f}, color, 1.8f);
        } else {
            drawList->AddCircle(center, 5.0f, color, 16, 1.8f);
            for (int index = 0; index < 8; ++index) {
                const float angle = DirectX::XM_2PI * static_cast<float>(index) / 8.0f;
                const ImVec2 direction{std::cos(angle), std::sin(angle)};
                drawList->AddLine({center.x + direction.x * 7.0f,
                                   center.y + direction.y * 7.0f},
                                  {center.x + direction.x * 11.0f,
                                   center.y + direction.y * 11.0f},
                                  color, 1.5f);
            }
        }
        if (active) {
            using namespace DirectX;
            const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
            const XMVECTOR origin = XMVectorSet(worldMatrix._41, worldMatrix._42,
                                                worldMatrix._43, 1.0f);
            auto normalizedAxis = [&](float x, float y, float z) {
                XMVECTOR axis = XMVector3TransformNormal(XMVectorSet(x, y, z, 0.0f), world);
                return XMVectorGetX(XMVector3LengthSq(axis)) > 1.0e-8f
                           ? XMVector3Normalize(axis)
                           : XMVectorSet(x, y, z, 0.0f);
            };
            const XMVECTOR right = normalizedAxis(1.0f, 0.0f, 0.0f);
            const XMVECTOR up = normalizedAxis(0.0f, 1.0f, 0.0f);
            const XMVECTOR forward = normalizedAxis(0.0f, 0.0f, 1.0f);
            auto worldPoint = [&](float x, float y, float z) {
                XMFLOAT3 result{};
                XMStoreFloat3(&result, origin + right * x + up * y + forward * z);
                return result;
            };

            if (entity.camera) {
                const CameraComponent& camera = *entity.camera;
                const float aspect = static_cast<float>((std::max)(1, gameViewSurface_.GetWidth())) /
                                     static_cast<float>((std::max)(1, gameViewSurface_.GetHeight()));
                const float nearDepth = camera.nearClip;
                const float farDepth =
                    (std::min)(camera.farClip, (std::max)(20.0f, nearDepth + 0.001f));
                float nearHalfHeight = camera.orthographicHeight * 0.5f;
                float farHalfHeight = nearHalfHeight;
                if (camera.projection == CameraProjection::Perspective) {
                    const float tangent =
                        std::tan(XMConvertToRadians(camera.fieldOfViewDegrees) * 0.5f);
                    nearHalfHeight = tangent * nearDepth;
                    farHalfHeight = tangent * farDepth;
                }
                const float nearHalfWidth = nearHalfHeight * aspect;
                const float farHalfWidth = farHalfHeight * aspect;
                const std::array<XMFLOAT3, 8> corners = {
                    worldPoint(-nearHalfWidth, -nearHalfHeight, nearDepth),
                    worldPoint(nearHalfWidth, -nearHalfHeight, nearDepth),
                    worldPoint(nearHalfWidth, nearHalfHeight, nearDepth),
                    worldPoint(-nearHalfWidth, nearHalfHeight, nearDepth),
                    worldPoint(-farHalfWidth, -farHalfHeight, farDepth),
                    worldPoint(farHalfWidth, -farHalfHeight, farDepth),
                    worldPoint(farHalfWidth, farHalfHeight, farDepth),
                    worldPoint(-farHalfWidth, farHalfHeight, farDepth),
                };
                constexpr size_t edges[][2] = {
                    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
                };
                const ImU32 guideColor = camera.enabled ? IM_COL32(90, 185, 255, 190)
                                                        : IM_COL32(90, 185, 255, 80);
                for (const auto& edge : edges) {
                    drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor);
                }
            }

            if (entity.light) {
                const LightComponent& light = *entity.light;
                const ImU32 guideColor = light.enabled ? IM_COL32(255, 215, 80, 190)
                                                       : IM_COL32(255, 215, 80, 80);
                const XMFLOAT3 worldOrigin = worldPoint(0.0f, 0.0f, 0.0f);
                auto drawCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                      XMVECTOR circleCenter = DirectX::g_XMZero) {
                    constexpr int segments = 32;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle = XM_2PI * static_cast<float>(index) /
                                            static_cast<float>(segments);
                        XMFLOAT3 point{};
                        XMStoreFloat3(&point, origin + circleCenter +
                                                 axisA * (std::cos(angle) * radius) +
                                                 axisB * (std::sin(angle) * radius));
                        if (index > 0) {
                            drawWorldLine(previous, point, guideColor);
                        }
                        previous = point;
                    }
                };
                if (light.type == LightType::Directional) {
                    const XMFLOAT3 tip = worldPoint(0.0f, 0.0f, 3.0f);
                    drawWorldLine(worldOrigin, tip, guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(-0.3f, 0.0f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.3f, 0.0f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.0f, -0.3f, 2.5f), guideColor, 1.75f);
                    drawWorldLine(tip, worldPoint(0.0f, 0.3f, 2.5f), guideColor, 1.75f);
                } else if (light.type == LightType::Point) {
                    drawCircle(light.range, right, up);
                    drawCircle(light.range, right, forward);
                    drawCircle(light.range, up, forward);
                } else {
                    const float guideAngle = (std::min)(light.outerAngleDegrees, 89.0f);
                    const float coneRadius =
                        light.range * std::tan(XMConvertToRadians(guideAngle));
                    const XMVECTOR coneCenter = forward * light.range;
                    drawCircle(coneRadius, right, up, coneCenter);
                    for (int index = 0; index < 4; ++index) {
                        const float angle = XM_PIDIV2 * static_cast<float>(index);
                        XMFLOAT3 rim{};
                        XMStoreFloat3(&rim, origin + coneCenter +
                                              right * (std::cos(angle) * coneRadius) +
                                              up * (std::sin(angle) * coneRadius));
                        drawWorldLine(worldOrigin, rim, guideColor);
                    }
                }
            }
        }
        if (active && (!entity.meshRenderer || !entity.meshRenderer->enabled)) {
            drawList->AddText({center.x + 14.0f, center.y - ImGui::GetTextLineHeight() * 0.5f},
                              color, entity.name.c_str());
        }
    }
    drawList->PopClipRect();
}

void EditorScene::DrawSceneSelectionOutline(const ImVec2& imageMin,
                                            const ImVec2& imageMax) const {
    if (!selection_.IsValid() || ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    using namespace DirectX;
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    constexpr size_t edges[][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
        {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    auto drawEntityOutline = [&](const WorldEntity& entity, bool active) {
        if (!entity.meshRenderer || !entity.meshRenderer->enabled) {
            return;
        }
        const ModelHandle handle = ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
        XMFLOAT3 boundsMin{};
        XMFLOAT3 boundsMax{};
        XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !TryGetModelBounds(*model, boundsMin, boundsMax) ||
            !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            return;
        }
        const XMMATRIX worldViewProjection =
            XMLoadFloat4x4(&worldMatrix) * sceneViewCamera_.GetViewProjection();
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
        const ImU32 outlineColor = active ? IM_COL32(255, 184, 56, 255)
                                          : IM_COL32(90, 190, 255, 220);
        for (const auto& edge : edges) {
            drawList->AddLine(projected[edge[0]], projected[edge[1]], outlineColor,
                              active ? 2.0f : 1.5f);
        }
        if (!active) {
            return;
        }
        ImVec2 labelPosition = projected[0];
        for (const ImVec2& point : projected) {
            labelPosition.x = (std::min)(labelPosition.x, point.x);
            labelPosition.y = (std::min)(labelPosition.y, point.y);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
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
                          entity.name.c_str());
    };
    for (const WorldEntity& entity : world_.Entities()) {
        const bool selected = hierarchySelection_.contains(entity.id) || entity.id == selection_;
        if (selected && entity.id != selection_) {
            drawEntityOutline(entity, false);
        }
    }
    if (const WorldEntity* active = world_.Find(selection_)) {
        drawEntityOutline(*active, true);
    }
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
            activeGizmoWorldTransforms_.clear();
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
    const DirectX::XMFLOAT4X4 worldBeforeManipulation = worldMatrix;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, mode, &worldMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        SynchronizeHierarchySelection();
        const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
        BeginHistoryEdit(roots.size() > 1u ? "Transform Entities" : "Transform Entity");
        activeGizmoEntity_ = selection_;
        activeGizmoStartWorld_ = worldBeforeManipulation;
        activeGizmoWorldTransforms_.clear();
        activeGizmoWorldTransforms_.reserve(roots.size());
        for (EntityId root : roots) {
            DirectX::XMFLOAT4X4 initialWorld{};
            if (world_.TryGetWorldMatrix(root, initialWorld)) {
                activeGizmoWorldTransforms_.emplace_back(root, initialWorld);
            }
        }
    }

    if (manipulated && activeGizmoEntity_ == selection_) {
        using namespace DirectX;
        XMVECTOR pivotDeterminant{};
        const XMMATRIX inverseStartPivot =
            XMMatrixInverse(&pivotDeterminant, XMLoadFloat4x4(&activeGizmoStartWorld_));
        const float pivotDeterminantValue = XMVectorGetX(pivotDeterminant);
        if (std::isfinite(pivotDeterminantValue) &&
            std::abs(pivotDeterminantValue) > 1.0e-8f) {
            const XMMATRIX groupDelta = inverseStartPivot * XMLoadFloat4x4(&worldMatrix);
            bool changed = false;
            for (const auto& [entityId, initialStoredWorld] : activeGizmoWorldTransforms_) {
                WorldEntity* transformed = world_.Find(entityId);
                if (transformed == nullptr) {
                    continue;
                }
                XMMATRIX localMatrix = XMLoadFloat4x4(&initialStoredWorld) * groupDelta;
                bool canApply = true;
                if (transformed->parent.IsValid()) {
                    XMFLOAT4X4 parentWorld{};
                    if (!world_.TryGetWorldMatrix(transformed->parent, parentWorld)) {
                        canApply = false;
                    } else {
                        XMVECTOR parentDeterminant{};
                        const XMMATRIX inverseParent =
                            XMMatrixInverse(&parentDeterminant, XMLoadFloat4x4(&parentWorld));
                        const float parentDeterminantValue = XMVectorGetX(parentDeterminant);
                        if (std::isfinite(parentDeterminantValue) &&
                            std::abs(parentDeterminantValue) > 1.0e-8f) {
                            localMatrix *= inverseParent;
                        } else {
                            canApply = false;
                        }
                    }
                }
                TransformComponent localTransform{};
                if (canApply && TryDecomposeTransformComponent(localMatrix, localTransform)) {
                    transformed->transform = localTransform;
                    changed = true;
                }
            }
            if (changed) {
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        activeGizmoEntity_ = {};
        const size_t transformedCount = activeGizmoWorldTransforms_.size();
        activeGizmoWorldTransforms_.clear();
        status_ = transformedCount > 1u
                      ? "Transformed " + std::to_string(transformedCount) +
                            " entities from Scene View."
                      : "Transformed entity from Scene View.";
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
        cameraEntity->camera = CameraComponent{};
        cameraEntity->camera->primary = true;
    }
    const EntityId light = world_.CreateEntity("Directional Light");
    if (WorldEntity* lightEntity = world_.Find(light)) {
        lightEntity->transform.rotationDegrees = {50.0f, -30.0f, 0.0f};
        lightEntity->light = LightComponent{};
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

std::vector<std::filesystem::path> EditorScene::ShowImportAssetDialog() const {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Model and Dependency Files\0"
        L"*.fbx;*.obj;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.bin;*.mtl;*.png;*.jpg;*.jpeg;"
        L"*.tga;*.bmp;*.dds;*.hdr;*.exr\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }

    const std::filesystem::path first(buffer.data());
    const wchar_t* next = buffer.data() + first.native().size() + 1u;
    if (*next == L'\0') {
        return AssetImport::IsSelectableFile(first)
                   ? std::vector<std::filesystem::path>{first}
                   : std::vector<std::filesystem::path>{};
    }

    std::vector<std::filesystem::path> selectedFiles;
    while (*next != L'\0') {
        const std::filesystem::path filename(next);
        const std::filesystem::path selected = first / filename;
        if (!AssetImport::IsSelectableFile(selected)) {
            return {};
        }
        selectedFiles.push_back(selected);
        next += filename.native().size() + 1u;
    }
    return selectedFiles;
}
