#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "ScriptAsset.h"
#include "ScriptBuildService.h"

#include "core/AssetManager.h"
#include "core/MathUtils.h"
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
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"
#include "world/WorldCollision.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
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
constexpr const char* kTextureAssetDragPayload = "EDITOR_TEXTURE_ASSET";
constexpr const char* kAudioAssetDragPayload = "EDITOR_AUDIO_ASSET";
constexpr const char* kScriptAssetDragPayload = "EDITOR_SCRIPT_ASSET";
constexpr const char* kPrefabAssetDragPayload = "EDITOR_PREFAB_ASSET";
constexpr size_t kMaxHistoryEntries = 128;
constexpr size_t kMaxRecentScenes = 10;
constexpr float kRuntimeStepDeltaTime = 1.0f / 60.0f;

ImU32 PhysicsDebugLayerColor(uint8_t layer, bool enabled = true) {
    constexpr std::array<std::array<uint8_t, 3>, 8> colors = {{
        {80, 230, 130},
        {80, 170, 255},
        {255, 105, 105},
        {245, 205, 75},
        {185, 120, 255},
        {65, 220, 215},
        {255, 150, 75},
        {245, 110, 190},
    }};
    const auto& color = colors[layer % colors.size()];
    return IM_COL32(color[0], color[1], color[2], enabled ? 220 : 80);
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
    using namespace DirectX;
    XMVECTOR scale{};
    XMVECTOR rotation{};
    XMVECTOR translation{};
    if (!XMMatrixDecompose(&scale, &rotation, &translation, matrix)) {
        return false;
    }
    XMFLOAT3 decomposedScale{};
    XMFLOAT3 decomposedTranslation{};
    XMStoreFloat3(&decomposedScale, scale);
    XMStoreFloat3(&decomposedTranslation, translation);
    const XMFLOAT3 decomposedRotation =
        MathUtils::RotationDegreesFromQuaternion(rotation, transform.rotationDegrees);
    const float values[] = {
        decomposedTranslation.x, decomposedTranslation.y, decomposedTranslation.z,
        decomposedRotation.x, decomposedRotation.y, decomposedRotation.z,
        decomposedScale.x, decomposedScale.y, decomposedScale.z,
    };
    const bool finite = std::ranges::all_of(values, [](float value) {
        return std::isfinite(value);
    });
    if (!finite) {
        return false;
    }
    transform.position = decomposedTranslation;
    transform.rotationDegrees = decomposedRotation;
    transform.scale = decomposedScale;
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

bool TryGetCameraPreviewRect(const ImVec2& imageMin, const ImVec2& imageMax,
                             ImVec2& previewMin, ImVec2& previewMax) {
    const float availableWidth = imageMax.x - imageMin.x;
    const float availableHeight = imageMax.y - imageMin.y;
    if (availableWidth < 360.0f || availableHeight < 240.0f) {
        return false;
    }
    constexpr float margin = 12.0f;
    const float width = (std::min)(240.0f, availableWidth * 0.36f);
    const float height = width * 9.0f / 16.0f;
    previewMax = {imageMax.x - margin, imageMin.y + margin + height};
    previewMin = {previewMax.x - width, imageMin.y + margin};
    return true;
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

bool IsPrefabAsset(const std::filesystem::path& path) {
    return LowercaseAscii(path.extension().string()) == ".likeprefab";
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
      physicsSettingsStore_(projectRoot_ / L"settings" / L"physics.json"),
      recentScenesStore_(std::move(recentScenesPath), sceneRoot_),
      scenePath_(std::move(startupScene)) {
    std::string physicsSettingsError;
    const bool physicsSettingsLoaded =
        physicsSettingsStore_.Load(physicsSettings_, physicsSettingsError);
    world_.SetPhysicsSettings(physicsSettings_);
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
    if (!physicsSettingsLoaded) {
        status_ = "Warning: Could not load Physics Settings: " + physicsSettingsError;
    }
}

void EditorScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    std::string behaviorRequirementError;
    std::string scriptModuleError;
    if (!ScriptBuildService::BuildIfNeeded(projectRoot_, scriptModuleError) ||
        !projectScripts_.Load(projectRoot_, ctx.systems.input, behaviorRegistry_,
                              scriptModuleError)) {
        status_ = "Error: " + scriptModuleError;
    } else if (!ValidateWorldBehaviorRequirements(&behaviorRequirementError)) {
        status_ = "Error: Scene contains an invalid Behavior: " +
                  behaviorRequirementError;
    }
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
    if (!cameraPreviewSurface_.Initialize(ctx.rendering.dxCommon, ctx.rendering.srv, 320, 180)) {
        status_ = "Camera Preview RenderSurface initialization failed.";
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
    sceneGridPipelineId_ = ctx.rendering.meshRenderer->CreatePipeline(
        ShaderPaths::MeshVS, ShaderPaths::EditorGridPS);
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
    cameraPreviewCamera_.Initialize(16.0f / 9.0f);
    assetPreviewCamera_.SetPosition({0.0f, 0.0f, -4.0f});
    assetPreviewCamera_.SetRotation({0.0f, 0.0f, 0.0f});
    assetPreviewCamera_.Initialize(1.0f);
    RefreshAssetBrowser();
    ResolveMeshResources();
    InitializeScriptMonitoring();
}

void EditorScene::Update() {
    UpdateScriptCompilation();
    if (playModeState_ == PlayModeState::Playing && ctx_ != nullptr) {
        UpdateRuntimeWorld(ctx_->frame.deltaTime);
    }
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
    if (!cameraPreviewPostProcessInitializationAttempted_ && cameraPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr &&
        ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        cameraPreviewPostProcessInitializationAttempted_ = true;
        cameraPreviewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                             cameraPreviewSurface_.GetWidth(),
                                             cameraPreviewSurface_.GetHeight());
        if (!cameraPreviewPostProcess_.IsReady()) {
            status_ = "Camera Preview PostProcess initialization failed.";
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
    bool directionalAssigned = false;
    size_t pointLightIndex = 0u;
    bool spotAssigned = false;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.light ||
            !entity.light->enabled || entity.light->intensity <= 0.0f) {
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
    DrawProjectSettingsWindow();
    CaptureConsoleStatus();
}

bool EditorScene::OnCloseRequested() {
    if (IsInPlayMode()) {
        StopPlayMode();
    }
    if (physicsSettingsDirty_ && !SavePhysicsSettings()) {
        return false;
    }
    if (!dirty_) {
        return true;
    }
    RequestSceneAction(PendingSceneAction::Exit);
    return false;
}

void EditorScene::OnFilesDropped(std::span<const std::filesystem::path> files, int screenX,
                                 int screenY) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before importing assets.";
        return;
    }
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
        const bool editing = !IsInPlayMode();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, editing)) {
            RequestSceneAction(PendingSceneAction::NewScene);
        }
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, editing)) {
            RequestSceneAction(PendingSceneAction::OpenScene);
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editing)) {
            SaveScene();
        }
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, editing)) {
            SaveSceneAs();
        }
        if (ImGui::BeginMenu("Recent Scenes", editing && !recentScenePaths_.empty())) {
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
        if (ImGui::MenuItem("Reload Scene", nullptr, false, editing)) {
            RequestSceneAction(PendingSceneAction::ReloadScene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            RequestSceneAction(PendingSceneAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = !IsInPlayMode();
        const bool canUndo = editing && !undoHistory_.empty();
        const bool canRedo = editing && !redoHistory_.empty();
        const bool canDuplicate = editing && world_.Contains(selection_);
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
        if (ImGui::MenuItem("Paste", "Ctrl+V", false,
                            editing && !entityClipboard_.empty())) {
            PasteEntityClipboard();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canDuplicate)) {
            DuplicateSelection();
        }
        if (ImGui::MenuItem("Delete", "Delete", false, canDuplicate)) {
            DeleteSelection();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Project Settings...")) {
            showProjectSettings_ = true;
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
    ImGui::Separator();
    const bool playing = playModeState_ == PlayModeState::Playing;
    const bool paused = playModeState_ == PlayModeState::Paused;
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.18f, 0.48f, 0.24f, 1.0f});
    }
    ImGui::BeginDisabled(playing);
    if (ImGui::Button(paused ? "Resume" : "Play")) {
        if (playModeState_ == PlayModeState::Edit) {
            EnterPlayMode();
        } else if (paused) {
            TogglePlayPause();
        }
    }
    ImGui::EndDisabled();
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.58f, 0.40f, 0.12f, 1.0f});
    }
    if (ImGui::Button("Pause")) {
        TogglePlayPause();
    }
    if (paused) {
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step")) {
        StepRuntimeWorld();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Advance the paused Runtime World by one 1/60-second update (F7).");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!IsInPlayMode());
    if (ImGui::Button("Stop")) {
        StopPlayMode();
    }
    ImGui::EndDisabled();
    std::string editorLabel = "LikeEngine Editor - ";
    editorLabel += scenePath_.empty() ? "Untitled" : scenePath_.filename().string();
    if (dirty_) {
        editorLabel += " *";
    }
    const bool titlePlaying = playModeState_ == PlayModeState::Playing;
    const bool titlePaused = playModeState_ == PlayModeState::Paused;
    if (titlePlaying) {
        editorLabel += "  [PLAYING]";
    } else if (titlePaused) {
        editorLabel += "  [PAUSED]";
    }
    if (IsInPlayMode()) {
        char runtimeStatus[64]{};
        sprintf_s(runtimeStatus, "  Frame %llu | %.2fs",
                  static_cast<unsigned long long>(runtimeFrameCount_),
                  runtimeElapsedSeconds_);
        editorLabel += runtimeStatus;
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
    if (IsInPlayMode()) {
        renameEntity_ = {};
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
    cameraPreviewSurface_.ReleaseCompletedFrameResources();
    assetPreviewSurface_.ReleaseCompletedFrameResources();
    projectPanelMinX_ = 0.0f;
    projectPanelMinY_ = 0.0f;
    projectPanelMaxX_ = 0.0f;
    projectPanelMaxY_ = 0.0f;
    Input* input = ctx_ != nullptr ? ctx_->systems.input : nullptr;
    if (input != nullptr) {
        input->SetQueryEnabled(false, false, false);
    }
    if (gameInputCaptured_ &&
        (playModeState_ != PlayModeState::Playing || !showGamePanel_)) {
        ReleaseGameInputCapture();
    }
    if (showHierarchyPanel_) {
        if (ImGui::Begin("Hierarchy", &showHierarchyPanel_, kPanelFlags)) {
            DrawHierarchyPanel();
        }
        ImGui::End();
    }

    if (showProjectPanel_) {
        if (ImGui::Begin("Project", &showProjectPanel_, kPanelFlags)) {
            ImGui::BeginDisabled(IsInPlayMode());
            DrawProjectPanel();
            ImGui::EndDisabled();
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
                ImVec2 expectedPreviewMin{};
                ImVec2 expectedPreviewMax{};
                const WorldEntity* previewEntity = world_.Find(selection_);
                const bool cameraPreviewHovered =
                    previewEntity != nullptr && previewEntity->camera &&
                    cameraPreviewSurface_.IsReady() && cameraPreviewPostProcess_.IsReady() &&
                    TryGetCameraPreviewRect(expectedImageMin, expectedImageMax,
                                            expectedPreviewMin, expectedPreviewMax) &&
                    ImGui::IsMouseHoveringRect(expectedPreviewMin, expectedPreviewMax);
                const bool expectedImageHovered =
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(expectedImageMin, expectedImageMax) &&
                    !cameraPreviewHovered;
                HandleSceneCameraControls(expectedImageMin, expectedImageMax,
                                          expectedImageHovered);
                BuildRenderScene();
                BuildEditorOverlayScene();
                sceneRenderer_.Render(renderScene_, sceneViewCamera_, sceneViewSurface_,
                                      {0.025f, 0.035f, 0.055f, 1.0f},
                                      &editorOverlayScene_);
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
                const bool imageHovered = ImGui::IsItemHovered() && !cameraPreviewHovered;
                if (!IsInPlayMode()) {
                    HandleSceneAssetDrop(imageMin, imageMax);
                    HandleSceneContextMenu(imageMin, imageMax, imageHovered);
                }
                DrawSceneComponentGizmos(imageMin, imageMax);
                DrawSceneSelectionOutline(imageMin, imageMax);
                bool sceneGizmoHovered = false;
                if (!IsInPlayMode()) {
                    if (boxColliderGizmoMode_ != BoxColliderGizmoMode::None) {
                        sceneGizmoHovered = DrawBoxColliderGizmo(imageMin, imageMax);
                    } else if (characterControllerGizmoMode_ !=
                               CharacterControllerGizmoMode::None) {
                        sceneGizmoHovered =
                            DrawCharacterControllerGizmo(imageMin, imageMax);
                    } else {
                        sceneGizmoHovered = DrawSceneTransformGizmo(imageMin, imageMax);
                    }
                }
                if (IsInPlayMode() || !sceneGizmoHovered) {
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
                DrawSelectedCameraPreview(imageMin, imageMax);
            } else {
                ImGui::TextDisabled("Scene View RenderSurface is not ready.");
            }
        }
        ImGui::End();
    }

    if (showGamePanel_) {
        if (focusGamePanelRequested_) {
            ImGui::SetNextWindowFocus();
            focusGamePanelRequested_ = false;
        }
        if (ImGui::Begin("Game", &showGamePanel_, kPanelFlags)) {
            const bool gameViewFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (gameInputCaptured_ && !gameViewFocused) {
                ReleaseGameInputCapture();
                status_ = "Released Game input because the Game View lost focus.";
            }
            const ImVec2 available = ImGui::GetContentRegionAvail();
            requestedGameWidth_ = (std::max)(1, static_cast<int>(std::lround(available.x)));
            requestedGameHeight_ = (std::max)(1, static_cast<int>(std::lround(available.y)));
            if (!gameViewSurface_.IsReady() || !gameViewPostProcess_.IsReady() ||
                ctx_ == nullptr || ctx_->rendering.dxCommon == nullptr ||
                ctx_->rendering.model == nullptr) {
                ReleaseGameInputCapture();
                ImGui::TextDisabled("Game View RenderSurface is not ready.");
            } else if (!UpdateGameViewCamera()) {
                ReleaseGameInputCapture();
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
                const ImVec2 gameImageMin = ImGui::GetItemRectMin();
                const ImVec2 gameImageMax = ImGui::GetItemRectMax();
                const bool gameImageHovered = ImGui::IsItemHovered();
                if (playModeState_ == PlayModeState::Playing && gameImageHovered &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !gameInputCaptured_) {
                    POINT cursor{};
                    if (GetCursorPos(&cursor)) {
                        gameInputCursorRestoreX_ = cursor.x;
                        gameInputCursorRestoreY_ = cursor.y;
                    }
                    gameInputCaptured_ = true;
                    status_ = "Game input captured. Press Escape to release.";
                }
                if (gameInputCaptured_) {
                    const int cursorCenterX = static_cast<int>(
                        std::lround((gameImageMin.x + gameImageMax.x) * 0.5f));
                    const int cursorCenterY = static_cast<int>(
                        std::lround((gameImageMin.y + gameImageMax.y) * 0.5f));
                    SetCursorPos(cursorCenterX, cursorCenterY);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                }
                if (input != nullptr && playModeState_ == PlayModeState::Playing) {
                    input->SetQueryEnabled(gameViewFocused,
                                           gameViewFocused &&
                                               (gameImageHovered || gameInputCaptured_),
                                           gameViewFocused);
                }
                if (playModeState_ == PlayModeState::Playing) {
                    const char* captureHint = gameInputCaptured_
                                                  ? "Input captured - Esc to release"
                                                  : "Click Game View to capture input";
                    const ImVec2 hintSize = ImGui::CalcTextSize(captureHint);
                    const ImVec2 hintMin{gameImageMin.x + 10.0f, gameImageMin.y + 10.0f};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled({hintMin.x - 4.0f, hintMin.y - 2.0f},
                                            {hintMin.x + hintSize.x + 4.0f,
                                             hintMin.y + hintSize.y + 2.0f},
                                            IM_COL32(20, 24, 32, 190), 3.0f);
                    drawList->AddText(hintMin, IM_COL32(220, 225, 235, 230),
                                      captureHint);
                }
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
            ImGui::BeginDisabled(IsInPlayMode());
            DrawInspectorPanel();
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
}

bool EditorScene::SavePhysicsSettings() {
    std::string error;
    if (!physicsSettingsStore_.Save(physicsSettings_, error)) {
        status_ = "Error: Could not save Physics Settings: " + error;
        return false;
    }
    world_.SetPhysicsSettings(physicsSettings_);
    physicsSettingsDirty_ = false;
    status_ = "Saved Physics Settings.";
    return true;
}

void EditorScene::DrawProjectSettingsWindow() {
    if (!showProjectSettings_) {
        return;
    }
    ImGui::SetNextWindowSize({760.0f, 620.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &showProjectSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Physics");
    ImGui::TextDisabled("Project file: %s",
                        physicsSettingsStore_.Path().generic_string().c_str());
    ImGui::TextWrapped("Define project Layers and which Layer pairs are allowed to collide. "
                       "The matrix filters Character Controller blocking and Trigger events.");
    ImGui::BeginDisabled(IsInPlayMode());
    ImGui::BeginDisabled(!physicsSettingsDirty_);
    if (ImGui::Button("Save")) {
        SavePhysicsSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        PhysicsSettings restored{};
        std::string error;
        if (physicsSettingsStore_.Load(restored, error)) {
            physicsSettings_ = std::move(restored);
            world_.SetPhysicsSettings(physicsSettings_);
            physicsSettingsDirty_ = false;
            status_ = "Reverted Physics Settings.";
        } else {
            status_ = "Error: Could not reload Physics Settings: " + error;
        }
    }
    ImGui::EndDisabled();
    if (physicsSettingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.25f, 1.0f}, "Unsaved changes");
    }

    ImGui::SeparatorText("Layers");
    ImGui::TextDisabled("Layer 0 is reserved as Default. Empty Layer names are unused.");
    if (ImGui::BeginChild("PhysicsLayers", {0.0f, 220.0f}, ImGuiChildFlags_Borders)) {
        for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
            ImGui::PushID(static_cast<int>(index));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%2zu", index);
            ImGui::SameLine();
            std::array<char, 65> buffer{};
            strncpy_s(buffer.data(), buffer.size(),
                      physicsSettings_.layerNames[index].c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::BeginDisabled(index == 0u);
            if (ImGui::InputText("##LayerName", buffer.data(), buffer.size())) {
                physicsSettings_.layerNames[index] = buffer.data();
                physicsSettingsDirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    std::vector<size_t> definedLayers;
    for (size_t index = 0u; index < PhysicsSettings::kLayerCount; ++index) {
        if (!physicsSettings_.layerNames[index].empty()) {
            definedLayers.push_back(index);
        }
    }
    ImGui::SeparatorText("Layer Collision Matrix");
    ImGui::TextDisabled("A checked pair will be allowed to collide.");
    std::vector<std::string> columnLabels;
    columnLabels.reserve(definedLayers.size());
    for (size_t layer : definedLayers) {
        columnLabels.push_back(std::to_string(layer));
    }
    constexpr ImGuiTableFlags matrixFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("PhysicsCollisionMatrix",
                          static_cast<int>(definedLayers.size() + 1u), matrixFlags,
                          {0.0f, 250.0f})) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        for (const std::string& label : columnLabels) {
            ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthFixed, 30.0f);
        }
        ImGui::TableHeadersRow();
        for (size_t row = 0u; row < definedLayers.size(); ++row) {
            const size_t first = definedLayers[row];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu: %s", first, physicsSettings_.layerNames[first].c_str());
            for (size_t column = 0u; column < definedLayers.size(); ++column) {
                ImGui::TableSetColumnIndex(static_cast<int>(column + 1u));
                if (column > row) {
                    continue;
                }
                const size_t second = definedLayers[column];
                bool collide = physicsSettings_.LayersCollide(first, second);
                ImGui::PushID(static_cast<int>(first));
                ImGui::PushID(static_cast<int>(second));
                if (ImGui::Checkbox("##Collide", &collide)) {
                    physicsSettings_.SetLayersCollide(first, second, collide);
                    world_.SetPhysicsSettings(physicsSettings_);
                    physicsSettingsDirty_ = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::End();
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
               normalized.find("warning") != std::string::npos ||
               normalized.find("unavailable") != std::string::npos) {
        severity = ConsoleSeverity::Warning;
    }
    AddConsoleEntry(status_, severity);
}

void EditorScene::AddConsoleEntry(std::string message, ConsoleSeverity severity,
                                  std::filesystem::path sourcePath,
                                  uint32_t sourceLine, uint32_t sourceColumn) {
    if (message.empty()) {
        return;
    }
    consoleEntries_.push_back({std::move(message), ImGui::GetTime(), severity,
                               std::move(sourcePath), sourceLine, sourceColumn});
    constexpr size_t kMaxConsoleEntries = 512u;
    if (consoleEntries_.size() > kMaxConsoleEntries) {
        consoleEntries_.erase(consoleEntries_.begin(),
                              consoleEntries_.begin() +
                                  static_cast<ptrdiff_t>(consoleEntries_.size() -
                                                         kMaxConsoleEntries));
    }
    consoleScrollToBottom_ = true;
}

bool EditorScene::OpenConsoleSource(const std::filesystem::path& sourcePath,
                                    uint32_t sourceLine) {
    std::filesystem::path physical = sourcePath;
    if (physical.is_relative()) {
        physical = projectRoot_ / physical;
    }
    std::error_code error;
    physical = std::filesystem::weakly_canonical(physical, error);
    if (error || !std::filesystem::is_regular_file(physical, error) || error) {
        status_ = "Could not open compiler source because the file no longer exists.";
        return false;
    }
    if (reinterpret_cast<intptr_t>(ShellExecuteW(
            nullptr, L"open", physical.c_str(), nullptr,
            physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
        status_ = "Could not open compiler source: " + physical.generic_string();
        return false;
    }
    status_ = "Opened compiler source: " + physical.filename().string();
    if (sourceLine > 0u) {
        status_ += " (line " + std::to_string(sourceLine) + ").";
    }
    return true;
}

void EditorScene::InitializeScriptMonitoring() {
    lastScriptScanTime_ = std::chrono::steady_clock::now();
    lastScriptChangeTime_ = lastScriptScanTime_;
    std::string error;
    if (ScriptBuildService::GetSourceFingerprint(projectRoot_, scriptSourceFingerprint_,
                                                 error)) {
        scriptFingerprintInitialized_ = true;
    } else {
        status_ = "Warning: Project Script monitoring could not start: " + error;
    }
}

void EditorScene::UpdateScriptCompilation() {
    constexpr auto scanInterval = std::chrono::milliseconds(500);
    constexpr auto buildDebounce = std::chrono::milliseconds(750);
    const auto now = std::chrono::steady_clock::now();

    if (scriptBuildInProgress_ &&
        scriptBuildFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        FinishScriptCompilation();
    }

    if (now - lastScriptScanTime_ >= scanInterval) {
        lastScriptScanTime_ = now;
        uint64_t fingerprint = 0u;
        std::string error;
        if (ScriptBuildService::GetSourceFingerprint(projectRoot_, fingerprint, error)) {
            if (!scriptFingerprintInitialized_) {
                scriptSourceFingerprint_ = fingerprint;
                scriptFingerprintInitialized_ = true;
            } else if (fingerprint != scriptSourceFingerprint_) {
                scriptSourceFingerprint_ = fingerprint;
                scriptBuildPending_ = true;
                lastScriptChangeTime_ = now;
                if (IsInPlayMode()) {
                    if (!scriptChangesDeferredMessageShown_) {
                        status_ = "Project Script changes detected. Compilation is deferred "
                                  "until Play Mode stops.";
                        scriptChangesDeferredMessageShown_ = true;
                    }
                } else {
                    status_ = "Project Script changes detected.";
                }
            }
        } else {
            status_ = "Warning: Project Script change detection failed: " + error;
        }
    }

    if (!scriptBuildInProgress_ && scriptBuildPending_ && !IsInPlayMode() &&
        now - lastScriptChangeTime_ >= buildDebounce) {
        StartScriptCompilation();
    }
}

void EditorScene::StartScriptCompilation() {
    if (scriptBuildInProgress_ || IsInPlayMode()) {
        return;
    }
    scriptBuildPending_ = false;
    scriptBuildInProgress_ = true;
    scriptChangesDeferredMessageShown_ = false;
    status_ = "Compiling Project Scripts...";
    const std::filesystem::path projectRoot = projectRoot_;
    try {
        scriptBuildFuture_ = std::async(std::launch::async, [projectRoot] {
            ScriptBuildCompletion completion{};
            completion.succeeded = ScriptBuildService::Build(
                projectRoot, completion.error, &completion.output);
            return completion;
        });
    } catch (const std::exception& exception) {
        scriptBuildInProgress_ = false;
        status_ = "Error: Could not start Project Script compilation: " +
                  std::string(exception.what());
    }
}

void EditorScene::FinishScriptCompilation() {
    ScriptBuildCompletion completion = scriptBuildFuture_.get();
    scriptBuildInProgress_ = false;
    if (!completion.output.empty()) {
        constexpr size_t maximumOutputLength = 48u * 1024u;
        if (completion.output.size() > maximumOutputLength) {
            completion.output.erase(0u,
                                    completion.output.size() - maximumOutputLength);
            completion.output.insert(0u, "... compiler output truncated ...\n");
        }
        AddConsoleEntry("Project Script compiler output:", ConsoleSeverity::Info);
        std::istringstream stream(completion.output);
        std::string outputLine;
        while (std::getline(stream, outputLine)) {
            if (!outputLine.empty() && outputLine.back() == '\r') {
                outputLine.pop_back();
            }
            if (outputLine.empty()) {
                continue;
            }
            std::string normalized = LowercaseAscii(outputLine);
            ConsoleSeverity severity = ConsoleSeverity::Info;
            if (normalized.find("error") != std::string::npos ||
                normalized.find("failed") != std::string::npos) {
                severity = ConsoleSeverity::Error;
            } else if (normalized.find("warning") != std::string::npos) {
                severity = ConsoleSeverity::Warning;
            }
            std::filesystem::path sourcePath;
            uint32_t sourceLine = 0u;
            uint32_t sourceColumn = 0u;
            ScriptBuildService::ParseDiagnosticLocation(
                outputLine, sourcePath, sourceLine, sourceColumn);
            AddConsoleEntry(std::move(outputLine), severity, std::move(sourcePath),
                            sourceLine, sourceColumn);
        }
    }
    if (!completion.succeeded) {
        status_ = "Error: " +
                  (completion.error.empty() ?
                       std::string("Project Script compilation failed.") :
                       completion.error);
        return;
    }
    if (IsInPlayMode()) {
        scriptBuildPending_ = true;
        lastScriptChangeTime_ = std::chrono::steady_clock::now();
        scriptChangesDeferredMessageShown_ = true;
        status_ = "Project Script compilation finished during Play Mode. Reload is "
                  "deferred until Play Mode stops.";
        return;
    }
    if (scriptBuildPending_) {
        status_ = "Project Scripts changed again during compilation. Rebuilding...";
        return;
    }

    std::string reloadError;
    if (!ReloadProjectScripts(reloadError)) {
        status_ = "Error: Project Script reload failed: " + reloadError;
        return;
    }
    std::string requirementError;
    if (!ValidateWorldBehaviorRequirements(&requirementError)) {
        status_ = "Warning: Project Scripts reloaded, but the scene contains an invalid "
                  "Behavior: " + requirementError;
    } else {
        status_ = "Project Scripts compiled and reloaded successfully (" +
                  std::to_string(behaviorRegistry_.Types().size()) + " type(s)).";
    }
}

bool EditorScene::ReloadProjectScripts(std::string& error) {
    if (IsInPlayMode() || ctx_ == nullptr) {
        error = "Project Scripts can only be reloaded in Edit Mode.";
        return false;
    }
    ProjectScriptLibrary newLibrary;
    BehaviorRegistry newRegistry;
    if (!newLibrary.Load(projectRoot_, ctx_->systems.input, newRegistry, error)) {
        return false;
    }

    // Destroy factories that point into the old DLL before unloading that DLL.
    behaviorRegistry_ = std::move(newRegistry);
    projectScripts_ = std::move(newLibrary);
    RefreshAssetBrowser();
    error.clear();
    return true;
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
            ImGui::BeginGroup();
            ImGui::TextDisabled("[%7.2f]", entry.timestampSeconds);
            ImGui::SameLine();
            ImGui::TextColored(color, "[%s]", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::EndGroup();
            const bool hasSource = !entry.sourcePath.empty() && entry.sourceLine > 0u;
            if (hasSource && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (entry.sourceColumn > 0u) {
                    ImGui::SetTooltip("Double-click to open %s:%u:%u",
                                      entry.sourcePath.filename().string().c_str(),
                                      entry.sourceLine, entry.sourceColumn);
                } else {
                    ImGui::SetTooltip("Double-click to open %s:%u",
                                      entry.sourcePath.filename().string().c_str(),
                                      entry.sourceLine);
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    OpenConsoleSource(entry.sourcePath, entry.sourceLine);
                }
            }
            if (ImGui::BeginPopupContextItem("MessageContext")) {
                if (hasSource && ImGui::MenuItem("Open Source")) {
                    OpenConsoleSource(entry.sourcePath, entry.sourceLine);
                }
                if (hasSource && ImGui::MenuItem("Copy Source Location")) {
                    std::string location = entry.sourcePath.generic_string() + ":" +
                                           std::to_string(entry.sourceLine);
                    if (entry.sourceColumn > 0u) {
                        location += ":" + std::to_string(entry.sourceColumn);
                    }
                    ImGui::SetClipboardText(location.c_str());
                }
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
        if (ImGui::MenuItem("Prefab from Selection...", nullptr, false,
                            selection_.IsValid() && !IsInPlayMode())) {
            SaveSelectionAsPrefab();
        }
        if (ImGui::MenuItem("Import Assets...")) {
            ImportAssetFiles();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RefreshAssetBrowser();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "%zu model(s), %zu texture(s), %zu audio(s), %zu script(s), %zu prefab(s)",
        modelAssets_.size(), textureAssets_.size(), audioAssets_.size(),
        scriptAssets_.size(), prefabAssets_.size());
    ImGui::Separator();
    if (!currentAssetDirectory_.empty()) {
        if (ImGui::Button("< Back")) {
            NavigateAssetBrowser(currentAssetDirectory_.parent_path());
        }
        ImGui::SameLine();
    }
    DrawAssetBrowserBreadcrumbs();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetSearch", "Search assets...", assetSearch_.data(),
                             assetSearch_.size());

    constexpr const char* formatLabels[] = {
        "All formats", "Prefab", "C++ Script", "glTF", "GLB", "OBJ", "FBX", "DAE",
        "3DS", "PLY", "PNG", "JPG", "JPEG", "TGA", "BMP", "DDS", "HDR", "EXR",
        "WAV", "MP3", "AAC", "M4A", "WMA"};
    constexpr const char* formatExtensions[] = {
        "", ".likeprefab", ".cpp", ".gltf", ".glb", ".obj", ".fbx", ".dae", ".3ds",
        ".ply", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr",
        ".wav", ".mp3", ".aac", ".m4a", ".wma"};
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
            auto appendMatches = [&](const auto& assets) {
                for (const std::filesystem::path& logicalPath : assets) {
                    const std::filesystem::path relativePath =
                        logicalPath.lexically_relative("assets");
                    if (ContainsCaseInsensitive(logicalPath.generic_string(), search) &&
                        matchesFormat(relativePath)) {
                        matches.push_back(relativePath);
                    }
                }
            };
            appendMatches(modelAssets_);
            appendMatches(textureAssets_);
            appendMatches(audioAssets_);
            appendMatches(scriptAssets_);
            appendMatches(prefabAssets_);
            std::ranges::sort(matches, comparePaths);
            for (const std::filesystem::path& relativePath : matches) {
                DrawAssetBrowserEntry(relativePath, false);
            }
            if (matches.empty()) {
                ImGui::TextDisabled("No matching assets.");
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
                ImGui::TextDisabled("This folder contains no matching assets or folders.");
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
    const bool texture = !directory && AssetImport::IsTextureFile(relativePath);
    const bool audio = !directory && AssetImport::IsAudioFile(relativePath);
    const bool script = !directory && ScriptAssets::IsScriptFile(relativePath);
    const bool scriptSource =
        !directory && ScriptAssets::IsScriptSourceFile(relativePath);
    const bool scriptHeader = scriptSource && !script;
    const bool prefab = !directory && IsPrefabAsset(relativePath);
    const std::string label = std::string(directory ? "[Folder] "
                                                     : prefab ? "[Prefab] "
                                                     : texture ? "[Texture] "
                                                     : audio ? "[Audio] "
                                                     : script ? "[Script] "
                                                     : scriptSource ? "[C++ Script] "
                                                                    : "[Model] ") +
                              relativePath.filename().string();
    ImGui::PushID(id.c_str());
    const bool selected = selectedAsset_ == relativePath;
    if (ImGui::Selectable(label.c_str(), selected,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
        selectedAsset_ = relativePath;
        if (directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            NavigateAssetBrowser(relativePath);
        } else if (prefab && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            InstantiatePrefabAsset(logicalPath);
        } else if (scriptSource &&
                   ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const std::filesystem::path physical = assetRoot_ / relativePath;
            if (reinterpret_cast<intptr_t>(ShellExecuteW(
                    nullptr, L"open", physical.c_str(), nullptr,
                    physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
                status_ = "Could not open Script source: " + id;
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", id.c_str());
    }
    if (!directory && !scriptHeader && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(prefab ? kPrefabAssetDragPayload
                                          : texture ? kTextureAssetDragPayload
                                          : audio ? kAudioAssetDragPayload
                                          : script ? kScriptAssetDragPayload
                                                   : kModelAssetDragPayload,
                                  id.c_str(), id.size() + 1u);
        ImGui::TextUnformatted(id.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("AssetContext")) {
        selectedAsset_ = relativePath;
        if (directory) {
            if (ImGui::MenuItem("Open")) {
                NavigateAssetBrowser(relativePath);
            }
        } else if (scriptHeader) {
            if (ImGui::MenuItem("Open")) {
                const std::filesystem::path physical = assetRoot_ / relativePath;
                if (reinterpret_cast<intptr_t>(ShellExecuteW(
                        nullptr, L"open", physical.c_str(), nullptr,
                        physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
                    status_ = "Could not open Script source: " + id;
                }
            }
        } else if (prefab && ImGui::MenuItem("Instantiate")) {
            InstantiatePrefabAsset(logicalPath);
        } else if (script && ImGui::MenuItem("Attach to Selected Entity", nullptr, false,
                                             selection_.IsValid())) {
            AssignScriptAsset(selection_, logicalPath);
        } else if (!texture && ImGui::MenuItem("Create Entity")) {
            CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
        } else if (texture && ImGui::BeginMenu("Assign to Selected Material",
                                               selection_.IsValid())) {
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
    std::string typeLabel = directory
                                ? "Folder"
                                : IsPrefabAsset(physical)
                                      ? "Prefab"
                                : AssetImport::IsTextureFile(physical)
                                      ? "Texture"
                                : AssetImport::IsAudioFile(physical)
                                      ? "Audio"
                                      : ScriptAssets::IsScriptFile(physical)
                                            ? "Script"
                                            : ScriptAssets::IsScriptSourceFile(physical)
                                                  ? "C++ Script Source"
                                                  : "Model";
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
    if (regularFile && AssetImport::IsAudioFile(physical)) {
        DrawAudioAssetPreview(physical);
        return;
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

    ModelManager* modelManager = ctx_->rendering.model;
    Model* model = modelManager->GetModel(assetPreviewModel_);
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
        if (!model->animations.empty()) {
            std::vector<std::string> animationNames;
            animationNames.reserve(model->animations.size());
            for (const auto& [name, clip] : model->animations) {
                (void)clip;
                animationNames.push_back(name);
            }
            std::ranges::sort(animationNames);
            if (assetPreviewAnimation_.empty() ||
                !model->animations.contains(assetPreviewAnimation_)) {
                assetPreviewAnimation_ = model->animations.contains(model->currentAnimation)
                                             ? model->currentAnimation
                                             : animationNames.front();
                modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                            assetPreviewAnimationLoop_);
            }
            if (ImGui::BeginCombo("Animation##ModelPreview",
                                  assetPreviewAnimation_.c_str())) {
                for (const std::string& name : animationNames) {
                    const bool selected = name == assetPreviewAnimation_;
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        assetPreviewAnimation_ = name;
                        modelManager->PlayAnimation(assetPreviewModel_, name,
                                                    assetPreviewAnimationLoop_);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (model->isPlaying) {
                if (ImGui::Button("Pause##ModelPreviewAnimation")) {
                    model->isPlaying = false;
                }
            } else if (ImGui::Button("Play##ModelPreviewAnimation")) {
                if (model->animationFinished) {
                    modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                                assetPreviewAnimationLoop_);
                } else {
                    model->isPlaying = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart##ModelPreviewAnimation")) {
                modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                            assetPreviewAnimationLoop_);
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Loop##ModelPreviewAnimation", &assetPreviewAnimationLoop_)) {
                model->isLoop = assetPreviewAnimationLoop_;
            }
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Speed##ModelPreviewAnimation", &assetPreviewAnimationSpeed_, 0.01f,
                             0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            const AnimationClip& clip = model->animations.at(assetPreviewAnimation_);
            const float animationDuration = (std::max)(clip.duration, 0.0f);
            const auto seekAnimation = [&](const float time) {
                model->animationTime = std::clamp(time, 0.0f, animationDuration);
                model->isPlaying = false;
                model->animationFinished =
                    animationDuration > 0.0f && model->animationTime >= animationDuration;
                modelManager->UpdateAnimation(assetPreviewModel_, 0.0f);
            };
            constexpr float kAnimationPreviewStepSeconds = 1.0f / 30.0f;
            if (ImGui::SmallButton("|<##ModelPreviewAnimation")) {
                seekAnimation(0.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("<##ModelPreviewAnimation")) {
                seekAnimation(model->animationTime - kAnimationPreviewStepSeconds);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Step backward 1/30 second");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">##ModelPreviewAnimation")) {
                seekAnimation(model->animationTime + kAnimationPreviewStepSeconds);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Step forward 1/30 second");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">|##ModelPreviewAnimation")) {
                seekAnimation(animationDuration);
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            float animationTime = model->animationTime;
            if (ImGui::SliderFloat("##ModelPreviewAnimationTimeline", &animationTime, 0.0f,
                                   animationDuration, "%.2f s",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                seekAnimation(animationTime);
            }
            ImGui::TextDisabled("%.2f / %.2f s", model->animationTime, animationDuration);
            if (model->isPlaying) {
                const float deltaTime =
                    std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f) *
                    assetPreviewAnimationSpeed_;
                modelManager->UpdateAnimation(assetPreviewModel_, deltaTime);
            }
        }
    }

    if (ImGui::SmallButton("Reset View##ModelPreview")) {
        assetPreviewRotationDegrees_ = {0.0f, 180.0f};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drag the preview to rotate");
    DirectX::XMStoreFloat4(
        &assetPreviewTransform_.rotation,
        DirectX::XMQuaternionRotationRollPitchYaw(
            DirectX::XMConvertToRadians(assetPreviewRotationDegrees_.x),
            DirectX::XMConvertToRadians(assetPreviewRotationDegrees_.y), 0.0f));
    assetPreviewSurface_.BeginScenePass({0.035f, 0.045f, 0.065f, 1.0f});
    ModelRenderer* previewRenderer = modelManager->GetRenderer();
    previewRenderer->PreDraw();
    modelManager->Draw(assetPreviewModel_, assetPreviewTransform_, assetPreviewCamera_);
    ModelRenderer::PostDraw();
    assetPreviewSurface_.EndScenePass();
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
    if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        assetPreviewRotationDegrees_.x =
            std::clamp(assetPreviewRotationDegrees_.x + delta.y * 0.4f, -89.0f, 89.0f);
        assetPreviewRotationDegrees_.y += delta.x * 0.4f;
    }
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
        const std::optional<std::filesystem::path> modelReference =
            entity.meshRenderer && entity.meshRenderer->sourceType == MeshSourceType::Model
                ? AssetRelativeFromReference(entity.meshRenderer->modelPath)
                : std::nullopt;
        const std::optional<std::filesystem::path> textureReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->baseColorTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> normalReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->normalTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> roughnessReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->roughnessTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> metallicReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->metallicTexturePath)
                : std::nullopt;
        const bool matchesModel =
            modelReference && AssetPathMatches(*modelReference, relativePath, directory);
        const bool matchesTexture =
            textureReference && AssetPathMatches(*textureReference, relativePath, directory);
        const bool matchesNormal =
            normalReference && AssetPathMatches(*normalReference, relativePath, directory);
        const bool matchesRoughness =
            roughnessReference && AssetPathMatches(*roughnessReference, relativePath, directory);
        const bool matchesMetallic =
            metallicReference && AssetPathMatches(*metallicReference, relativePath, directory);
        const bool matchesScript = std::ranges::any_of(
            entity.scripts, [&](const BehaviorComponent& script) {
                const std::optional<std::filesystem::path> referenced =
                    AssetRelativeFromReference(script.scriptAssetPath);
                return referenced && AssetPathMatches(*referenced, relativePath, directory);
            });
        const std::optional<std::filesystem::path> audioReference =
            entity.audioSource
                ? AssetRelativeFromReference(entity.audioSource->clipPath)
                : std::nullopt;
        const bool matchesAudio =
            audioReference && AssetPathMatches(*audioReference, relativePath, directory);
        if (!matchesModel && !matchesTexture && !matchesNormal && !matchesRoughness &&
            !matchesMetallic && !matchesScript && !matchesAudio) {
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
        bool referencedByEntity = false;
        if (entity.meshRenderer && entity.meshRenderer->sourceType == MeshSourceType::Model) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.meshRenderer->modelPath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->baseColorTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->roughnessTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->metallicTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->normalTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity) {
            referencedByEntity = std::ranges::any_of(
                entity.scripts, [&](const BehaviorComponent& script) {
                    const std::optional<std::filesystem::path> referenced =
                        AssetRelativeFromReference(script.scriptAssetPath);
                    return referenced &&
                           AssetPathMatches(*referenced, relativePath, directory);
                });
        }
        if (!referencedByEntity && entity.audioSource) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.audioSource->clipPath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (referencedByEntity) {
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
        for (BehaviorComponent& script : entity->scripts) {
            updateReference(script.scriptAssetPath);
        }
    }
    return updated;
}

void EditorScene::DrawHierarchyPanel() {
    SynchronizeHierarchySelection();
    const bool editing = !IsInPlayMode();
    if (!editing) {
        ImGui::TextDisabled("Runtime World (Read Only)");
    }
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Create")) {
        ImGui::OpenPopup("CreateEntity");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("CreateEntity")) {
        ImGui::BeginDisabled(!editing);
        DrawCreateEntityMenu({0.0f, 0.0f, 0.0f});
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool canEditSelection = editing && !hierarchySelection_.empty();
    ImGui::BeginDisabled(!canEditSelection);
    if (ImGui::Button("Duplicate")) {
        DuplicateSelection();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canEditSelection);
    if (ImGui::Button("Delete")) {
        DeleteSelection();
    }
    ImGui::EndDisabled();
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
    if (ImGui::Selectable(editing ? "Scene Root (drop here)" : "Scene Root", false)) {
        ClearHierarchySelection();
    }
    if (editing && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize == sizeof(EntityId)) {
            EntityId child{};
            std::memcpy(&child, payload->Data, sizeof(child));
            ReparentSelection(child, {});
        }
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            InstantiatePrefabAsset(static_cast<const char*>(payload->Data));
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
    const bool editing = !IsInPlayMode();
    bool active = entity->active;
    ImGui::BeginDisabled(!editing);
    if (ImGui::Checkbox("##EntityActive", &active)) {
        SetSelectedEntitiesActive(id, active);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(editing ? (active ? "Deactivate Entity" : "Activate Entity")
                                  : "Entity active state (read-only in Play Mode)");
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const bool activeInHierarchy = world_.IsActiveInHierarchy(id);
    if (!activeInHierarchy) {
        const ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              {textColor.x, textColor.y, textColor.z, textColor.w * 0.45f});
    }
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    const ImVec2 nodeMin = ImGui::GetItemRectMin();
    const ImVec2 nodeMax = ImGui::GetItemRectMax();
    if (!activeInHierarchy) {
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
        if (ImGui::BeginMenu("Create Child", editing)) {
            hierarchyChanged = DrawCreateEntityMenu({0.0f, 0.0f, 0.0f}, id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2", false, editing)) {
            RequestEntityRename(id);
        }
        if (ImGui::MenuItem("Focus in Scene", "F")) {
            SelectHierarchyEntity(id, false, false);
            FocusSceneCameraOnSelection();
        }
        if (ImGui::MenuItem("Active", nullptr, entity->active, editing)) {
            SetSelectedEntitiesActive(id, !entity->active);
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
        if (ImGui::MenuItem("Move Up", "Alt+Up", false, editing && canMoveUp)) {
            hierarchyChanged = MoveEntityInHierarchy(id, -1);
        }
        if (ImGui::MenuItem("Move Down", "Alt+Down", false, editing && canMoveDown)) {
            hierarchyChanged = MoveEntityInHierarchy(id, 1);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editing)) {
            DuplicateSelection();
            hierarchyChanged = true;
        }
        if (ImGui::MenuItem("Save as Prefab...", nullptr, false,
                            editing && GetTopLevelSelectedEntities().size() == 1u)) {
            SaveSelectionAsPrefab();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            CopySelection();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, editing)) {
            CutSelection();
            hierarchyChanged = true;
        }
        if (ImGui::MenuItem("Paste as Child", nullptr, false,
                            editing && !entityClipboard_.empty())) {
            hierarchyChanged = PasteEntityClipboard(id);
        }
        if (ImGui::MenuItem("Delete", "Delete", false, editing)) {
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
    if (editing && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityDragPayload, &id, sizeof(id));
        if (IsHierarchyEntitySelected(id) && hierarchySelection_.size() > 1u) {
            ImGui::Text("Move %zu selected entities", hierarchySelection_.size());
        } else {
            ImGui::TextUnformatted(entity->name.c_str());
        }
        ImGui::EndDragDropSource();
    }
    if (editing && ImGui::BeginDragDropTarget()) {
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
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAudioAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignAudioAsset(id, static_cast<const char*>(payload->Data));
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignScriptAsset(id, static_cast<const char*>(payload->Data));
        }
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            InstantiatePrefabAsset(static_cast<const char*>(payload->Data), id);
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

    std::vector<EntityId> inspectedEntities;
    if (hierarchySelection_.size() > 1u && hierarchySelection_.contains(selection_)) {
        inspectedEntities.reserve(hierarchySelection_.size());
        for (const WorldEntity& candidate : world_.Entities()) {
            if (hierarchySelection_.contains(candidate.id)) {
                inspectedEntities.push_back(candidate.id);
            }
        }
    } else {
        inspectedEntities.push_back(selection_);
    }
    const bool multipleEntities = inspectedEntities.size() > 1u;

    bool displayedActive = entity->active;
    const bool mixedActive = std::ranges::any_of(
        inspectedEntities, [&](EntityId inspected) {
            const WorldEntity* target = world_.Find(inspected);
            return target != nullptr && target->active != displayedActive;
        });
    if (mixedActive) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    if (ImGui::Checkbox("Active", &displayedActive)) {
        SetSelectedEntitiesActive(selection_, displayedActive);
    }
    if (mixedActive) {
        ImGui::PopItemFlag();
    }

    if (multipleEntities) {
        ImGui::Text("%zu Entities Selected", inspectedEntities.size());
        ImGui::TextDisabled("Transform changes preserve relative offsets and scale ratios.");
    } else {
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
    }
    uint8_t displayedLayer = entity->layer;
    const bool mixedLayers = std::ranges::any_of(
        inspectedEntities, [&](EntityId inspected) {
            const WorldEntity* target = world_.Find(inspected);
            return target != nullptr && target->layer != displayedLayer;
        });
    std::string layerPreview = mixedLayers ? "Mixed" : physicsSettings_.layerNames[displayedLayer];
    if (layerPreview.empty()) {
        layerPreview = "Layer " + std::to_string(displayedLayer) + " (Undefined)";
    }
    if (ImGui::BeginCombo("Layer", layerPreview.c_str())) {
        for (size_t layer = 0u; layer < PhysicsSettings::kLayerCount; ++layer) {
            if (physicsSettings_.layerNames[layer].empty()) {
                continue;
            }
            const bool selected = !mixedLayers && displayedLayer == layer;
            const std::string label = std::to_string(layer) + ": " +
                                      physicsSettings_.layerNames[layer];
            if (ImGui::Selectable(label.c_str(), selected)) {
                CommitHistoryEdit();
                const std::string before = WorldSerializer::Serialize(world_);
                const EntityId selectionBefore = selection_;
                for (EntityId inspected : inspectedEntities) {
                    if (WorldEntity* target = world_.Find(inspected)) {
                        target->layer = static_cast<uint8_t>(layer);
                    }
                }
                RecordImmediateEdit(inspectedEntities.size() > 1u ? "Set Entity Layers"
                                                                  : "Set Entity Layer",
                                    before, selectionBefore);
                status_ = inspectedEntities.size() > 1u
                              ? "Changed the selected Entity Layers."
                              : "Changed the Entity Layer.";
                displayedLayer = static_cast<uint8_t>(layer);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
    ImGui::TextUnformatted(multipleEntities ? "Transforms" : "Transform");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##Transform")) {
        CommitHistoryEdit();
        const std::string before = WorldSerializer::Serialize(world_);
        const EntityId selectionBefore = selection_;
        for (EntityId inspected : inspectedEntities) {
            if (WorldEntity* target = world_.Find(inspected)) {
                target->transform = TransformComponent{};
            }
        }
        RecordImmediateEdit(multipleEntities ? "Reset Transforms" : "Reset Transform",
                            before, selectionBefore);
        status_ = multipleEntities ? "Reset selected Transforms." : "Reset Transform.";
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
        for (EntityId inspected : inspectedEntities) {
            if (WorldEntity* target = world_.Find(inspected)) {
                target->transform = *transformClipboard_;
            }
        }
        RecordImmediateEdit(multipleEntities ? "Paste Transforms" : "Paste Transform",
                            before, selectionBefore);
        status_ = multipleEntities ? "Pasted Transform to selected entities."
                                   : "Pasted Transform.";
    }
    if (!transformClipboard_) {
        ImGui::EndDisabled();
    }
    auto drawTransform = [&](const char* label,
                             DirectX::XMFLOAT3 TransformComponent::*member,
                             float speed, bool scale) {
        const DirectX::XMFLOAT3 previous = entity->transform.*member;
        DirectX::XMFLOAT3 edited = previous;
        const bool changed = ImGui::DragFloat3(label, &edited.x, speed);
        if (ImGui::IsItemActivated()) {
            BeginHistoryEdit(std::string(multipleEntities ? "Modify Transforms "
                                                          : "Modify Transform ") +
                             label);
        }
        if (changed) {
            const DirectX::XMFLOAT3 delta{edited.x - previous.x, edited.y - previous.y,
                                          edited.z - previous.z};
            auto applyComponent = [scale](float current, float oldActive,
                                          float newActive, float additiveDelta) {
                constexpr float epsilon = 1.0e-6f;
                if (scale && std::abs(oldActive) > epsilon) {
                    return current * (newActive / oldActive);
                }
                return current + additiveDelta;
            };
            for (EntityId inspected : inspectedEntities) {
                WorldEntity* target = world_.Find(inspected);
                if (target == nullptr) {
                    continue;
                }
                DirectX::XMFLOAT3& value = target->transform.*member;
                if (inspected == selection_) {
                    value = edited;
                } else {
                    value = {applyComponent(value.x, previous.x, edited.x, delta.x),
                             applyComponent(value.y, previous.y, edited.y, delta.y),
                             applyComponent(value.z, previous.z, edited.z, delta.z)};
                }
            }
            RefreshDirty();
            status_ = multipleEntities ? "Modified selected Transforms."
                                       : "Modified Transform.";
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            CommitHistoryEdit();
        }
    };
    drawTransform("Position", &TransformComponent::position, 0.05f, false);
    drawTransform("Rotation", &TransformComponent::rotationDegrees, 0.25f, false);
    drawTransform("Scale", &TransformComponent::scale, 0.02f, true);

    if (multipleEntities) {
        ImGui::Separator();
        ImGui::TextDisabled("Component editing is available when one Entity is selected.");
        return;
    }

    ImGui::Separator();
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentMenu");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignScriptAsset(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup("AddComponentMenu")) {
        if (!entity->meshRenderer && ImGui::MenuItem("Mesh Renderer")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->meshRenderer = MeshRendererComponent{};
            RecordImmediateEdit("Add MeshRenderer", before, selectionBefore);
            status_ = "Added MeshRenderer.";
        }
        if (!entity->materialOverride && ImGui::MenuItem("Material Override")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->materialOverride = MaterialOverrideComponent{};
            RecordImmediateEdit("Add Material Override", before, selectionBefore);
            status_ = "Added Material Override.";
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
        if (!entity->audioSource && ImGui::MenuItem("Audio Source")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioSource = AudioSourceComponent{};
            RecordImmediateEdit("Add AudioSource", before, selectionBefore);
            status_ = "Added AudioSource.";
        }
        if (!entity->audioListener && ImGui::MenuItem("Audio Listener")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioListener = AudioListenerComponent{};
            RecordImmediateEdit("Add AudioListener", before, selectionBefore);
            status_ = "Added AudioListener.";
        }
        if (!entity->animator && ImGui::MenuItem("Animator")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->animator = AnimatorComponent{};
            RecordImmediateEdit("Add Animator", before, selectionBefore);
            status_ = "Added Animator.";
        }
        if (!entity->boxCollider && ImGui::MenuItem("Box Collider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->boxCollider = BoxColliderComponent{};
            RecordImmediateEdit("Add BoxCollider", before, selectionBefore);
            status_ = "Added BoxCollider.";
        }
        if (!entity->characterController && ImGui::MenuItem("Character Controller")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->characterController = CharacterControllerComponent{};
            RecordImmediateEdit("Add CharacterController", before, selectionBefore);
            status_ = "Added CharacterController.";
        }
        if (ImGui::MenuItem("Script")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->scripts.emplace_back();
            RecordImmediateEdit("Add Script", before, selectionBefore);
            status_ = "Added an empty Script component.";
        }
        ImGui::EndPopup();
    }

    for (size_t scriptIndex = 0; scriptIndex < entity->scripts.size(); ++scriptIndex) {
        ImGui::PushID(static_cast<int>(scriptIndex));
        ImGui::SeparatorText("Script");
        if (ImGui::Button("Remove Script")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->scripts.erase(entity->scripts.begin() +
                                  static_cast<std::ptrdiff_t>(scriptIndex));
            RecordImmediateEdit("Remove Script", before, selectionBefore);
            status_ = "Removed Script component.";
            ImGui::PopID();
            break;
        } else {
            BehaviorComponent& behavior = entity->scripts[scriptIndex];
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled", &behavior.enabled)) {
                RecordImmediateEdit("Toggle Script", std::move(before), selectionBefore);
            }
            const std::string scriptLabel = behavior.scriptAssetPath.empty()
                                                ? "None (drop a Script asset)"
                                                : behavior.scriptAssetPath;
            ImGui::TextUnformatted("Script");
            ImGui::SameLine();
            if (ImGui::Button(scriptLabel.c_str(), {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("ScriptAssetPicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignScriptAsset(selection_, static_cast<const char*>(payload->Data),
                                      scriptIndex);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("ScriptAssetPicker")) {
                if (ImGui::MenuItem("None", nullptr, behavior.scriptAssetPath.empty(),
                                    !behavior.scriptAssetPath.empty())) {
                    ClearScriptAsset(selection_, scriptIndex);
                }
                ImGui::Separator();
                if (scriptAssets_.empty()) {
                    ImGui::TextDisabled("No Script assets found.");
                } else {
                    for (const std::filesystem::path& scriptAsset : scriptAssets_) {
                        const std::string assetPath = scriptAsset.generic_string();
                        const std::string assetReference =
                            "asset://" +
                            scriptAsset.lexically_relative("assets").generic_string();
                        const std::string label =
                            scriptAsset.filename().generic_string() + "##" + assetPath;
                        if (ImGui::MenuItem(label.c_str(), nullptr,
                                            behavior.scriptAssetPath == assetReference)) {
                            AssignScriptAsset(selection_, scriptAsset, scriptIndex);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", assetPath.c_str());
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::TextDisabled("Runtime type: %s",
                                behavior.type.empty() ? "None" : behavior.type.c_str());
            const std::vector<ScriptPropertyDefinition>* propertyDefinitions =
                behaviorRegistry_.Properties(behavior.type);
            if (propertyDefinitions != nullptr) {
                for (size_t propertyIndex = 0u;
                     propertyIndex < propertyDefinitions->size(); ++propertyIndex) {
                    const ScriptPropertyDefinition& definition =
                        (*propertyDefinitions)[propertyIndex];
                    ImGui::PushID(static_cast<int>(propertyIndex));
                    auto stored = std::ranges::find(behavior.properties, definition.name,
                                                    &ScriptPropertyValue::name);
                    if (definition.type == ScriptPropertyType::Float) {
                        float value = stored != behavior.properties.end() &&
                                              stored->type == definition.type
                                          ? stored->floatValue
                                          : definition.defaultFloat;
                        const float speed = (std::max)(
                            0.001f,
                            (definition.maximumFloat - definition.minimumFloat) * 0.005f);
                        if (ImGui::DragFloat(definition.name.c_str(), &value, speed,
                                             definition.minimumFloat,
                                             definition.maximumFloat, "%.3f",
                                             ImGuiSliderFlags_AlwaysClamp)) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back(
                                    {definition.name, definition.type, value, {}});
                            } else if (stored->type != definition.type) {
                                *stored = {definition.name, definition.type, value, {}};
                            } else {
                                stored->floatValue = value;
                            }
                            RefreshDirty();
                            status_ = "Modified Script property.";
                        }
                        if (ImGui::IsItemActivated()) {
                            BeginHistoryEdit("Modify Script Property");
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            CommitHistoryEdit();
                        }
                    } else if (definition.type == ScriptPropertyType::Boolean) {
                        bool value = stored != behavior.properties.end() &&
                                             stored->type == definition.type
                                         ? stored->booleanValue
                                         : definition.defaultBoolean;
                        if (ImGui::Checkbox(definition.name.c_str(), &value)) {
                            const std::string propertyBefore =
                                WorldSerializer::Serialize(world_);
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            *stored = {};
                            stored->name = definition.name;
                            stored->type = definition.type;
                            stored->booleanValue = value;
                            RecordImmediateEdit("Modify Script Property", propertyBefore,
                                                selectionBefore);
                            status_ = "Modified Script property.";
                        }
                    } else if (definition.type == ScriptPropertyType::Integer) {
                        int value = stored != behavior.properties.end() &&
                                            stored->type == definition.type
                                        ? stored->integerValue
                                        : definition.defaultInteger;
                        if (ImGui::DragInt(definition.name.c_str(), &value, 1.0f,
                                           definition.minimumInteger,
                                           definition.maximumInteger, "%d",
                                           ImGuiSliderFlags_AlwaysClamp)) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            if (stored->type != definition.type) {
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                            }
                            stored->integerValue = value;
                            RefreshDirty();
                            status_ = "Modified Script property.";
                        }
                        if (ImGui::IsItemActivated()) {
                            BeginHistoryEdit("Modify Script Property");
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            CommitHistoryEdit();
                        }
                    } else if (definition.type == ScriptPropertyType::Vector3) {
                        ScriptVector3 value = stored != behavior.properties.end() &&
                                                      stored->type == definition.type
                                                  ? stored->vector3Value
                                                  : definition.defaultVector3;
                        float components[3]{value.x, value.y, value.z};
                        if (ImGui::DragFloat3(definition.name.c_str(), components, 0.1f,
                                              0.0f, 0.0f, "%.3f")) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            if (stored->type != definition.type) {
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                            }
                            stored->vector3Value =
                                {components[0], components[1], components[2]};
                            RefreshDirty();
                            status_ = "Modified Script property.";
                        }
                        if (ImGui::IsItemActivated()) {
                            BeginHistoryEdit("Modify Script Property");
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            CommitHistoryEdit();
                        }
                    } else if (definition.type == ScriptPropertyType::AnimationClip) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        const ModelHandle modelHandle = entity->meshRenderer
                                                            ? ResolveModel(*entity->meshRenderer)
                                                            : ModelHandle{};
                        const Model* animationModel =
                            modelHandle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
                                ? ctx_->rendering.model->GetModel(modelHandle)
                                : nullptr;
                        const std::string preview = value.empty() ? "None" : value;
                        if (ImGui::BeginCombo(definition.name.c_str(), preview.c_str())) {
                            const auto assignClip = [&](const std::string& clip) {
                                const std::string propertyBefore =
                                    WorldSerializer::Serialize(world_);
                                if (stored == behavior.properties.end()) {
                                    behavior.properties.push_back({});
                                    stored = std::prev(behavior.properties.end());
                                }
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                                stored->stringValue = clip;
                                RecordImmediateEdit("Modify Script Property", propertyBefore,
                                                    selectionBefore);
                                status_ = "Modified Script Animation Clip property.";
                            };
                            if (ImGui::Selectable("None", value.empty())) {
                                assignClip({});
                            }
                            if (animationModel != nullptr) {
                                std::vector<std::string> clips;
                                clips.reserve(animationModel->animations.size());
                                for (const auto& [name, clip] : animationModel->animations) {
                                    (void)clip;
                                    clips.push_back(name);
                                }
                                std::ranges::sort(clips);
                                for (const std::string& clip : clips) {
                                    if (ImGui::Selectable(clip.c_str(), value == clip)) {
                                        assignClip(clip);
                                    }
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if (animationModel == nullptr || animationModel->animations.empty()) {
                            ImGui::TextDisabled(
                                "Assign an animated Model to choose an Animation Clip.");
                        } else if (!value.empty() &&
                                   !animationModel->animations.contains(value)) {
                            ImGui::TextDisabled("The selected Animation Clip was not found.");
                        }
                    } else if (definition.type == ScriptPropertyType::String) {
                        const std::string value =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->stringValue
                                : definition.defaultString;
                        std::array<char, 1025> buffer{};
                        std::memcpy(buffer.data(), value.data(),
                                    (std::min)(value.size(), buffer.size() - 1u));
                        if (ImGui::InputText(definition.name.c_str(), buffer.data(),
                                             buffer.size())) {
                            if (stored == behavior.properties.end()) {
                                behavior.properties.push_back({});
                                stored = std::prev(behavior.properties.end());
                            }
                            if (stored->type != definition.type) {
                                *stored = {};
                                stored->name = definition.name;
                                stored->type = definition.type;
                            }
                            stored->stringValue = buffer.data();
                            RefreshDirty();
                            status_ = "Modified Script property.";
                        }
                        if (ImGui::IsItemActivated()) {
                            BeginHistoryEdit("Modify Script Property");
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            CommitHistoryEdit();
                        }
                    } else if (definition.type == ScriptPropertyType::Entity) {
                        const EntityId referenced =
                            stored != behavior.properties.end() &&
                                    stored->type == definition.type
                                ? stored->entityValue
                                : EntityId{};
                        const WorldEntity* referencedEntity = world_.Find(referenced);
                        std::string label = referencedEntity != nullptr
                                                ? referencedEntity->name
                                                : referenced.IsValid() ? "Missing Entity"
                                                                       : "None";
                        label += "##EntityProperty";
                        ImGui::TextUnformatted(definition.name.c_str());
                        ImGui::SameLine();
                        if (ImGui::Button(label.c_str(), {-FLT_MIN, 0.0f})) {
                            ImGui::OpenPopup("EntityPropertyPicker");
                        }
                        const auto assignEntityProperty = [&](EntityId value) {
                            auto destination = std::ranges::find(
                                behavior.properties, definition.name,
                                &ScriptPropertyValue::name);
                            if ((destination == behavior.properties.end() &&
                                 !value.IsValid()) ||
                                (destination != behavior.properties.end() &&
                                 destination->type == definition.type &&
                                 destination->entityValue == value)) {
                                return;
                            }
                            const std::string propertyBefore =
                                WorldSerializer::Serialize(world_);
                            if (destination == behavior.properties.end()) {
                                behavior.properties.push_back(
                                    {definition.name, definition.type, 0.0f, value});
                            } else if (destination->type != definition.type) {
                                *destination =
                                    {definition.name, definition.type, 0.0f, value};
                            } else {
                                destination->entityValue = value;
                            }
                            RecordImmediateEdit("Assign Script Entity Property",
                                                propertyBefore, selectionBefore);
                            status_ = "Assigned Script Entity property.";
                        };
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(kEntityDragPayload);
                                payload != nullptr && payload->IsDelivery() &&
                                payload->DataSize == sizeof(EntityId)) {
                                const EntityId dropped =
                                    *static_cast<const EntityId*>(payload->Data);
                                if (world_.Contains(dropped)) {
                                    assignEntityProperty(dropped);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (ImGui::BeginPopup("EntityPropertyPicker")) {
                            if (ImGui::MenuItem("None", nullptr,
                                                !referenced.IsValid())) {
                                assignEntityProperty({});
                            }
                            ImGui::Separator();
                            for (const WorldEntity& candidate : world_.Entities()) {
                                const std::string candidateLabel =
                                    candidate.name + "##" + candidate.id.ToString();
                                if (ImGui::MenuItem(candidateLabel.c_str(), nullptr,
                                                    candidate.id == referenced)) {
                                    assignEntityProperty(candidate.id);
                                }
                            }
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::PopID();
                }
            }
            if (behavior.type == "FirstPersonController" && !entity->characterController) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                                   "Character Controller is required for collision movement.");
            }
        }
        ImGui::PopID();
    }
    if (entity->boxCollider) {
        ImGui::SeparatorText("Box Collider");
        if (ImGui::Button("Remove Box Collider")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->boxCollider.reset();
            if (boxColliderGizmoEntity_ == selection_) {
                boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
                boxColliderGizmoEntity_ = {};
            }
            RecordImmediateEdit("Remove BoxCollider", before, selectionBefore);
            status_ = "Removed BoxCollider.";
        } else {
            BoxColliderComponent& collider = *entity->boxCollider;
            auto colliderGizmoButton = [&](const char* label, BoxColliderGizmoMode mode) {
                const bool selected = boxColliderGizmoEntity_ == selection_ &&
                                      boxColliderGizmoMode_ == mode;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(label)) {
                    boxColliderGizmoMode_ = selected ? BoxColliderGizmoMode::None : mode;
                    boxColliderGizmoEntity_ = selected ? EntityId{} : selection_;
                    characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
                    characterControllerGizmoEntity_ = {};
                }
                if (selected) {
                    ImGui::PopStyleColor();
                }
            };
            colliderGizmoButton("Edit Center", BoxColliderGizmoMode::Center);
            ImGui::SameLine();
            colliderGizmoButton("Edit Size", BoxColliderGizmoMode::Size);
            if (boxColliderGizmoEntity_ == selection_ &&
                boxColliderGizmoMode_ != BoxColliderGizmoMode::None) {
                ImGui::TextDisabled("Editing in Scene View. Click the active button to finish.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##BoxCollider", &collider.enabled)) {
                RecordImmediateEdit("Toggle BoxCollider", std::move(before),
                                    selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Is Trigger##BoxCollider", &collider.isTrigger)) {
                RecordImmediateEdit("Toggle BoxCollider Trigger", std::move(before),
                                    selectionBefore);
            }
            if (ImGui::DragFloat3("Center##BoxCollider", &collider.center.x, 0.02f)) {
                RefreshDirty();
                status_ = "Modified BoxCollider.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify BoxCollider Center");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat3("Size##BoxCollider", &collider.size.x, 0.02f, 0.001f,
                                  1000000.0f, "%.3f",
                                  ImGuiSliderFlags_AlwaysClamp)) {
                collider.size.x = (std::max)(0.001f, collider.size.x);
                collider.size.y = (std::max)(0.001f, collider.size.y);
                collider.size.z = (std::max)(0.001f, collider.size.z);
                RefreshDirty();
                status_ = "Modified BoxCollider.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify BoxCollider Size");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
        }
    }

    if (entity->characterController) {
        ImGui::SeparatorText("Character Controller");
        const bool requiredByBehavior = std::ranges::any_of(
            entity->scripts, [this](const BehaviorComponent& script) {
                const BehaviorRequirements* requirements =
                    behaviorRegistry_.Requirements(script.type);
                return requirements != nullptr && requirements->characterController;
            });
        ImGui::BeginDisabled(requiredByBehavior);
        const bool removeRequested = ImGui::Button("Remove Character Controller");
        ImGui::EndDisabled();
        if (requiredByBehavior &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Required by the assigned Behavior.");
        }
        if (removeRequested) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->characterController.reset();
            if (characterControllerGizmoEntity_ == selection_) {
                characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
                characterControllerGizmoEntity_ = {};
            }
            RecordImmediateEdit("Remove CharacterController", before, selectionBefore);
            status_ = "Removed CharacterController.";
        } else {
            CharacterControllerComponent& controller = *entity->characterController;
            auto controllerGizmoButton = [&](const char* label,
                                             CharacterControllerGizmoMode mode) {
                const bool selected = characterControllerGizmoEntity_ == selection_ &&
                                      characterControllerGizmoMode_ == mode;
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(label)) {
                    characterControllerGizmoMode_ =
                        selected ? CharacterControllerGizmoMode::None : mode;
                    characterControllerGizmoEntity_ = selected ? EntityId{} : selection_;
                    boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
                    boxColliderGizmoEntity_ = {};
                }
                if (selected) {
                    ImGui::PopStyleColor();
                }
            };
            controllerGizmoButton("Edit Center##CharacterController",
                                  CharacterControllerGizmoMode::Center);
            ImGui::SameLine();
            controllerGizmoButton("Edit Radius##CharacterController",
                                  CharacterControllerGizmoMode::Radius);
            ImGui::SameLine();
            controllerGizmoButton("Edit Height##CharacterController",
                                  CharacterControllerGizmoMode::Height);
            if (characterControllerGizmoEntity_ == selection_ &&
                characterControllerGizmoMode_ != CharacterControllerGizmoMode::None) {
                ImGui::TextDisabled("Editing in Scene View. Click the active button to finish.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##CharacterController", &controller.enabled)) {
                RecordImmediateEdit("Toggle CharacterController", std::move(before),
                                    selectionBefore);
            }
            if (ImGui::DragFloat3("Center##CharacterController", &controller.center.x,
                                  0.02f)) {
                RefreshDirty();
                status_ = "Modified CharacterController.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify CharacterController Center");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawControllerFloat = [&](const char* label, float& value, float minimum,
                                           float maximum) {
                if (ImGui::DragFloat(label, &value, 0.01f, minimum, maximum, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified CharacterController.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify CharacterController");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawControllerFloat("Radius##CharacterController", controller.radius, 0.001f,
                                1000000.0f);
            controller.height = (std::max)(controller.height, controller.radius * 2.0f);
            controller.skinWidth =
                (std::min)(controller.skinWidth, (std::max)(0.0f, controller.radius - 0.001f));
            drawControllerFloat("Height##CharacterController", controller.height,
                                controller.radius * 2.0f, 1000000.0f);
            controller.stepOffset = (std::min)(controller.stepOffset, controller.height);
            drawControllerFloat("Slope Limit##CharacterController",
                                controller.slopeLimitDegrees, 0.0f, 90.0f);
            drawControllerFloat("Step Offset##CharacterController", controller.stepOffset,
                                0.0f, controller.height);
            drawControllerFloat("Skin Width##CharacterController", controller.skinWidth,
                                0.0f, (std::max)(0.0f, controller.radius - 0.001f));
            drawControllerFloat("Min Move Distance##CharacterController",
                                controller.minMoveDistance, 0.0f, 1000000.0f);
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
            if (ImGui::Button("Align to Scene View")) {
                AlignSelectedCameraToSceneView();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move and rotate this Camera to match the Scene View.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Move View to Camera")) {
                AlignSceneViewToSelectedCamera();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move the Scene View to this Camera's position and rotation.");
            }
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

    if (entity->audioSource) {
        ImGui::SeparatorText("Audio Source");
        if (ImGui::Button("Remove Audio Source")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioSource.reset();
            RecordImmediateEdit("Remove AudioSource", before, selectionBefore);
            status_ = "Removed AudioSource.";
        } else {
            AudioSourceComponent& source = *entity->audioSource;
            if (IsInPlayMode()) {
                ImGui::TextDisabled("Runtime: %s",
                                    source.runtimePlaying ? "Playing" : "Stopped");
            } else {
                ImGui::TextDisabled(
                    "Script API: PlayAudioSource / PlayAudioSourceOneShot / StopAudioSource");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##AudioSource", &source.enabled)) {
                RecordImmediateEdit("Toggle AudioSource", std::move(before), selectionBefore);
            }

            const std::string clipLabel = source.clipPath.empty()
                                              ? "None (drop an Audio asset)"
                                              : source.clipPath;
            ImGui::TextUnformatted("Clip");
            ImGui::SameLine();
            if (ImGui::Button(clipLabel.c_str(), {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("AudioAssetPicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kAudioAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignAudioAsset(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("AudioAssetPicker")) {
                if (ImGui::Selectable("None", source.clipPath.empty())) {
                    before = WorldSerializer::Serialize(world_);
                    source.clipPath.clear();
                    RecordImmediateEdit("Clear Audio Clip", std::move(before),
                                        selectionBefore);
                }
                for (const std::filesystem::path& audioAsset : audioAssets_) {
                    const std::string label = audioAsset.generic_string();
                    if (ImGui::Selectable(label.c_str(), source.clipPath == label)) {
                        AssignAudioAsset(selection_, audioAsset);
                    }
                }
                ImGui::EndPopup();
            }

            auto drawAudioCheckbox = [&](const char* label, bool& value,
                                         const char* historyLabel) {
                before = WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox(label, &value)) {
                    RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
                }
            };
            drawAudioCheckbox("Play On Awake##AudioSource", source.playOnAwake,
                              "Toggle AudioSource Play On Awake");
            drawAudioCheckbox("Loop##AudioSource", source.loop, "Toggle AudioSource Loop");
            drawAudioCheckbox("Spatial##AudioSource", source.spatial,
                              "Toggle AudioSource Spatial");

            auto drawAudioFloat = [&](const char* label, float& value, float speed,
                                      float minimum, float maximum) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, "%.2f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified AudioSource.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify AudioSource");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawAudioFloat("Volume##AudioSource", source.volume, 0.01f, 0.0f, 1.0f);
            drawAudioFloat("Pitch##AudioSource", source.pitch, 0.01f,
                           AudioSourceComponent::kMinPitch, AudioSourceComponent::kMaxPitch);
            if (source.spatial) {
                drawAudioFloat("Min Distance##AudioSource", source.minDistance, 0.05f,
                               0.0f, (std::max)(0.0f, source.maxDistance - 0.01f));
                drawAudioFloat("Max Distance##AudioSource", source.maxDistance, 0.1f,
                               source.minDistance + 0.01f, 1000000.0f);
            }
        }
    }

    if (entity->audioListener) {
        ImGui::SeparatorText("Audio Listener");
        if (ImGui::Button("Remove Audio Listener")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->audioListener.reset();
            RecordImmediateEdit("Remove AudioListener", before, selectionBefore);
            status_ = "Removed AudioListener.";
        } else {
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##AudioListener", &entity->audioListener->enabled)) {
                RecordImmediateEdit("Toggle AudioListener", std::move(before), selectionBefore);
            }
            ImGui::TextDisabled("Receives 3D audio at this Entity's Transform.");
            if (!entity->camera) {
                ImGui::TextDisabled("A Camera component is not required.");
            }
        }
    }

    if (entity->animator) {
        ImGui::SeparatorText("Animator");
        if (ImGui::Button("Remove Animator")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->animator.reset();
            RecordImmediateEdit("Remove Animator", before, selectionBefore);
            status_ = "Removed Animator.";
        } else {
            AnimatorComponent& animator = *entity->animator;
            if (IsInPlayMode()) {
                ImGui::TextDisabled("Runtime: %s%s",
                                    animator.runtimePlaying ? "Playing" : "Stopped",
                                    animator.runtimeFinished ? " (Finished)" : "");
                if (!animator.runtimeClip.empty()) {
                    ImGui::TextDisabled("Runtime Clip: %s", animator.runtimeClip.c_str());
                }
                char animationProgress[64]{};
                std::snprintf(animationProgress, std::size(animationProgress), "%.2f / %.2f s",
                              animator.runtimeTime, animator.runtimeDuration);
                ImGui::ProgressBar(animator.runtimeNormalizedTime, {-FLT_MIN, 0.0f},
                                   animationProgress);
                if (animator.runtimeTransitioning) {
                    ImGui::ProgressBar(animator.runtimeTransitionProgress, {-FLT_MIN, 0.0f},
                                       "Cross Fade");
                }
            } else {
                ImGui::TextDisabled(
                    "Script API: PlayAnimation / CrossFadeAnimation / StopAnimation");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Animator", &animator.enabled)) {
                RecordImmediateEdit("Toggle Animator", std::move(before), selectionBefore);
            }

            const ModelHandle handle = entity->meshRenderer
                                           ? ResolveModel(*entity->meshRenderer)
                                           : ModelHandle{};
            const Model* model = handle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
                                     ? ctx_->rendering.model->GetModel(handle)
                                     : nullptr;
            const char* clipLabel = animator.clip.empty() ? "First Clip" : animator.clip.c_str();
            if (ImGui::BeginCombo("Clip##Animator", clipLabel)) {
                if (ImGui::Selectable("First Clip", animator.clip.empty())) {
                    before = WorldSerializer::Serialize(world_);
                    animator.clip.clear();
                    RecordImmediateEdit("Change Animator Clip", std::move(before),
                                        selectionBefore);
                }
                if (model != nullptr) {
                    std::vector<std::string> clips;
                    clips.reserve(model->animations.size());
                    for (const auto& [name, clip] : model->animations) {
                        (void)clip;
                        clips.push_back(name);
                    }
                    std::ranges::sort(clips);
                    for (const std::string& clip : clips) {
                        if (ImGui::Selectable(clip.c_str(), animator.clip == clip)) {
                            before = WorldSerializer::Serialize(world_);
                            animator.clip = clip;
                            RecordImmediateEdit("Change Animator Clip", std::move(before),
                                                selectionBefore);
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (model == nullptr || model->animations.empty()) {
                ImGui::TextDisabled("Assign an animated Model to Mesh Renderer.");
            }
            auto drawAnimatorCheckbox = [&](const char* label, bool& value,
                                            const char* historyLabel) {
                before = WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox(label, &value)) {
                    RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
                }
            };
            drawAnimatorCheckbox("Play On Awake##Animator", animator.playOnAwake,
                                 "Toggle Animator Play On Awake");
            drawAnimatorCheckbox("Loop##Animator", animator.loop, "Toggle Animator Loop");
            if (ImGui::DragFloat("Speed##Animator", &animator.speed, 0.01f, 0.0f, 100.0f,
                                 "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Animator.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Animator Speed");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
        }
    }

    if (entity->materialOverride) {
        ImGui::SeparatorText("Material Override");
        if (ImGui::Button("Remove Material Override")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->materialOverride.reset();
            RecordImmediateEdit("Remove Material Override", before, selectionBefore);
            status_ = "Removed Material Override.";
        } else {
            MaterialOverrideComponent& material = *entity->materialOverride;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##MaterialOverride", &material.enabled)) {
                RecordImmediateEdit("Toggle Material Override", std::move(before),
                                    selectionBefore);
            }
            int blendMode = static_cast<int>(material.blendMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Blend Mode##MaterialOverride", &blendMode,
                             "Opaque\0Cutout\0Transparent\0")) {
                material.blendMode = static_cast<MaterialSurfaceBlendMode>(blendMode);
                RecordImmediateEdit("Change Material Blend Mode", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Cutout) {
                if (ImGui::DragFloat("Alpha Cutoff##MaterialOverride", &material.alphaCutoff,
                                     0.01f, 0.0f, 1.0f, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Alpha Cutoff");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            }
            int cullMode = static_cast<int>(material.cullMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Cull Mode##MaterialOverride", &cullMode,
                             "None (Double-Sided)\0Front\0Back\0")) {
                material.cullMode = static_cast<MaterialSurfaceCullMode>(cullMode);
                RecordImmediateEdit("Change Material Cull Mode", std::move(before),
                                    selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Depth Write##MaterialOverride", &material.depthWrite)) {
                RecordImmediateEdit("Toggle Material Depth Write", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Transparent &&
                ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Transparent materials always disable depth writes at draw time.");
            }
            if (ImGui::ColorEdit4("Base Color##MaterialOverride", &material.baseColor.x,
                                  ImGuiColorEditFlags_Float)) {
                RefreshDirty();
                status_ = "Modified Material Override.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Material Base Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawMaterialFloat = [&](const char* label, float& value) {
                if (ImGui::DragFloat(label, &value, 0.01f, 0.0f, 1.0f, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Material Override");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawMaterialFloat("Metallic##MaterialOverride", material.metallic);
            drawMaterialFloat("Roughness##MaterialOverride", material.roughness);
            std::array<char, 512> texturePathBuffer{};
            strncpy_s(texturePathBuffer.data(), texturePathBuffer.size(),
                      material.baseColorTexturePath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Base Color Texture", texturePathBuffer.data(),
                                 texturePathBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (texturePathBuffer[0] == '\0') {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.baseColorTexturePath;
                    material.baseColorTexturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Base Color Texture", clearBefore,
                                        selectionBefore);
                    status_ = "Cleared Base Color texture.";
                } else {
                    AssignBaseColorTexture(selection_, texturePathBuffer.data());
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignBaseColorTexture(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (!material.baseColorTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##BaseColorTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.baseColorTexturePath;
                    material.baseColorTexturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Base Color Texture", clearBefore,
                                        selectionBefore);
                    status_ = "Cleared Base Color texture.";
                }
                const TextureHandle texture = ResolveBaseColorTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            }
            if (ImGui::DragFloat("Normal Strength##MaterialOverride", &material.normalStrength,
                                 0.01f, 0.0f, 4.0f, "%.3f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Material Override.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Normal Strength");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            std::array<char, 512> normalPathBuffer{};
            strncpy_s(normalPathBuffer.data(), normalPathBuffer.size(),
                      material.normalTexturePath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Normal Texture", normalPathBuffer.data(),
                                 normalPathBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (normalPathBuffer[0] == '\0') {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.normalTexturePath;
                    material.normalTexturePath.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Normal texture.";
                } else {
                    AssignNormalTexture(selection_, normalPathBuffer.data());
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignNormalTexture(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (!material.normalTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##NormalTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.normalTexturePath;
                    material.normalTexturePath.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Normal texture.";
                }
                const TextureHandle texture = ResolveNormalTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Normal texture is loading or unavailable.");
                }
            }
            int packing = static_cast<int>(material.pbrTexturePacking);
            const std::string packingBefore = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("PBR Texture Packing", &packing,
                             "Separate\0ORM (R=AO, G=Roughness, B=Metallic)\0Metallic-Roughness (G=Roughness, B=Metallic)\0")) {
                material.pbrTexturePacking =
                    static_cast<MaterialPbrTexturePacking>(packing);
                RecordImmediateEdit("Change PBR Texture Packing", packingBefore,
                                    selectionBefore);
            }
            using TextureAssignFunction =
                void (EditorScene::*)(EntityId, const std::filesystem::path&);
            auto drawLinearTextureSlot = [&](const char* label, const char* id,
                                             std::string& path,
                                             TextureAssignFunction assignTexture) {
                std::array<char, 512> pathBuffer{};
                strncpy_s(pathBuffer.data(), pathBuffer.size(), path.c_str(), _TRUNCATE);
                const std::string inputLabel = std::string(label) + "##" + id;
                if (ImGui::InputText(inputLabel.c_str(), pathBuffer.data(), pathBuffer.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (pathBuffer[0] == '\0') {
                        const std::string clearBefore = WorldSerializer::Serialize(world_);
                        const std::string previousPath = path;
                        path.clear();
                        loadedLinearTextures_.erase(previousPath);
                        RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                            selectionBefore);
                        status_ = std::string("Cleared ") + label + ".";
                    } else {
                        (this->*assignTexture)(selection_, pathBuffer.data());
                    }
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
                        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                        (this->*assignTexture)(selection_,
                                               static_cast<const char*>(payload->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
                if (path.empty()) {
                    return;
                }
                ImGui::SameLine();
                const std::string clearId = std::string("Clear##") + id;
                if (ImGui::SmallButton(clearId.c_str())) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = path;
                    path.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                        selectionBefore);
                    status_ = std::string("Cleared ") + label + ".";
                    return;
                }
                const TextureHandle texture = ResolveLinearTexture(path);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            };
            drawLinearTextureSlot("Roughness Texture", "RoughnessTexture",
                                  material.roughnessTexturePath,
                                  &EditorScene::AssignRoughnessTexture);
            drawLinearTextureSlot("Metallic Texture", "MetallicTexture",
                                  material.metallicTexturePath,
                                  &EditorScene::AssignMetallicTexture);
            if (material.pbrTexturePacking != MaterialPbrTexturePacking::Separate &&
                (material.roughnessTexturePath.empty() ||
                 material.roughnessTexturePath != material.metallicTexturePath)) {
                ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f},
                                   "Packed PBR textures must use the same asset in both slots.");
            }
            if (!entity->meshRenderer) {
                ImGui::TextDisabled("Add a Mesh Renderer to display this material.");
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
    if (pendingSceneAction_ != PendingSceneAction::None) {
        return;
    }
    if (gameInputCaptured_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ReleaseGameInputCapture();
        status_ = "Released Game input.";
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (IsInPlayMode()) {
            StopPlayMode();
        } else {
            EnterPlayMode();
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        if (IsInPlayMode()) {
            TogglePlayPause();
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
        StepRuntimeWorld();
        return;
    }
    if (io.WantTextInput || sceneCameraNavigating_ || sceneCameraPanning_) {
        return;
    }
    if (IsInPlayMode()) {
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
        if (!entity->materialOverride) {
            entity->materialOverride = MaterialOverrideComponent{};
        }
    }
    entity->meshRenderer->sourceType = MeshSourceType::Model;
    entity->meshRenderer->modelPath = assetPath;
    loadedModels_.erase(previousPath);
    loadedModels_.erase(assetPath);
    animatorModels_.clear();
    selection_ = entityId;
    RecordImmediateEdit("Assign Model Asset", before, selectionBefore);
    status_ = "Assigned model asset: " + assetPath;
}

void EditorScene::AssignAudioAsset(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeAudioAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->audioSource) {
        entity->audioSource = AudioSourceComponent{};
    }
    entity->audioSource->clipPath = assetPath;
    selection_ = entityId;
    RecordImmediateEdit("Assign Audio Asset", before, selectionBefore);
    status_ = "Assigned audio asset: " + assetPath;
}

void EditorScene::DrawAudioAssetPreview(const std::filesystem::path& physicalPath) {
    const std::filesystem::path selected = selectedAsset_.lexically_normal();
    if (assetPreviewAsset_ != selected) {
        StopAudioAssetPreview();
        audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
        assetPreviewAsset_ = selected;
        assetPreviewModel_ = {};
        assetPreviewPlan_.clear();
        assetPreviewError_.clear();
    }
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    const bool playing = sound != nullptr &&
                         audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle &&
                         sound->IsPlaying(audioPreviewVoice_);
    ImGui::BeginDisabled(sound == nullptr);
    if (ImGui::SmallButton(playing ? "Restart Preview" : "Play Preview")) {
        StopAudioAssetPreview();
        uint32_t soundId = ISoundService::kInvalidSoundId;
        if (!sound->TryLoad(physicalPath.wstring(), soundId)) {
            status_ = "Audio preview failed: the file could not be decoded.";
        } else {
            audioPreviewSoundId_ = soundId;
            audioPreviewVoice_ = sound->Play(soundId);
            status_ = audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle
                          ? "Playing audio preview: " + physicalPath.filename().string()
                          : "Audio preview failed: the audio device is unavailable.";
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::SmallButton("Stop Preview")) {
        StopAudioAssetPreview();
        status_ = "Stopped audio preview.";
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (sound != nullptr && audioPreviewSoundId_ != ISoundService::kInvalidSoundId) {
        if (const ISoundService::SoundInfo* info = sound->GetInfo(audioPreviewSoundId_)) {
            ImGui::TextDisabled("Duration: %.2f s   Channels: %u   Sample Rate: %u Hz",
                                info->durationSeconds, static_cast<unsigned>(info->channels),
                                info->sampleRate);
        }
    }
}

void EditorScene::StopAudioAssetPreview() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound != nullptr && audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle) {
        sound->Stop(audioPreviewVoice_);
    }
    audioPreviewVoice_ = ISoundService::kInvalidVoiceHandle;
}

void EditorScene::AssignScriptAsset(EntityId entityId, const std::filesystem::path& path,
                                    std::optional<size_t> scriptIndex) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    std::filesystem::path physicalPath;
    if (!TryNormalizeScriptAssetReference(path, assetPath, physicalPath)) {
        return;
    }
    const std::string_view scriptType =
        behaviorRegistry_.TypeFromSourceAsset(assetPath);
    if (scriptType.empty()) {
        status_ = "C++ Script source is not registered by the Project Script module: " +
                  assetPath;
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    BehaviorComponent component{};
    component.type = scriptType;
    component.scriptAssetPath = assetPath;
    if (scriptIndex) {
        if (*scriptIndex >= entity->scripts.size()) {
            status_ = "The target Script component no longer exists.";
            return;
        }
        component.enabled = entity->scripts[*scriptIndex].enabled;
        entity->scripts[*scriptIndex] = std::move(component);
    } else {
        entity->scripts.push_back(std::move(component));
    }
    (void)behaviorRegistry_.EnsureRequirements(scriptType, *entity);
    selection_ = entityId;
    RecordImmediateEdit(scriptIndex ? "Replace Script" : "Add Script", before,
                        selectionBefore);
    status_ = std::string(scriptIndex ? "Replaced" : "Added") +
              " Script component: " + assetPath;
}

void EditorScene::ClearScriptAsset(EntityId entityId, size_t scriptIndex) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || scriptIndex >= entity->scripts.size()) {
        status_ = "The target Script component no longer exists.";
        return;
    }
    BehaviorComponent& component = entity->scripts[scriptIndex];
    if (component.type.empty() && component.scriptAssetPath.empty()) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const bool enabled = component.enabled;
    component = BehaviorComponent{};
    component.enabled = enabled;
    RecordImmediateEdit("Clear Script", before, selectionBefore);
    status_ = "Cleared Script component assignment.";
}

void EditorScene::AssignBaseColorTexture(EntityId entityId,
                                         const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->materialOverride
                                         ? entity->materialOverride->baseColorTexturePath
                                         : std::string{};
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->baseColorTexturePath = assetPath;
    loadedTextures_.erase(previousPath);
    loadedTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Base Color Texture", before, selectionBefore);
    status_ = "Assigned Base Color texture: " + assetPath;
}

void EditorScene::AssignNormalTexture(EntityId entityId,
                                      const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->materialOverride
                                         ? entity->materialOverride->normalTexturePath
                                         : std::string{};
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->normalTexturePath = assetPath;
    loadedLinearTextures_.erase(previousPath);
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Normal Texture", before, selectionBefore);
    status_ = "Assigned Normal texture: " + assetPath;
}

void EditorScene::AssignRoughnessTexture(EntityId entityId,
                                         const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->roughnessTexturePath = assetPath;
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Roughness Texture", before, selectionBefore);
    status_ = "Assigned Roughness texture: " + assetPath;
}

void EditorScene::AssignMetallicTexture(EntityId entityId,
                                        const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr) {
        status_ = "The target entity no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (!entity->materialOverride) {
        entity->materialOverride = MaterialOverrideComponent{};
    }
    entity->materialOverride->metallicTexturePath = assetPath;
    loadedLinearTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Metallic Texture", before, selectionBefore);
    status_ = "Assigned Metallic texture: " + assetPath;
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

bool EditorScene::TryNormalizeTextureAssetReference(const std::filesystem::path& path,
                                                    std::string& assetPath) {
    if (!AssetImport::IsTextureFile(path)) {
        status_ = "The dropped texture asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped texture asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped texture asset path is too long.";
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
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        InstantiatePrefabAsset(static_cast<const char*>(payload->Data), {}, position);
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

bool EditorScene::AlignSelectedCameraToSceneView() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before aligning it to the Scene View.";
        return false;
    }

    using namespace DirectX;
    XMFLOAT4X4 currentWorld{};
    TransformComponent currentWorldTransform{};
    if (!world_.TryGetWorldMatrix(entity->id, currentWorld) ||
        !TryDecomposeTransformComponent(XMLoadFloat4x4(&currentWorld),
                                        currentWorldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }

    const XMFLOAT3 position = sceneViewCamera_.GetPosition();
    const XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    XMMATRIX local =
        XMMatrixScaling(currentWorldTransform.scale.x, currentWorldTransform.scale.y,
                        currentWorldTransform.scale.z) *
        XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
        XMMatrixTranslation(position.x, position.y, position.z);
    if (entity->parent.IsValid()) {
        XMFLOAT4X4 parentWorld{};
        if (!world_.TryGetWorldMatrix(entity->parent, parentWorld)) {
            status_ = "Could not read the Camera parent transform.";
            return false;
        }
        XMVECTOR determinant{};
        const XMMATRIX inverseParent =
            XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
            status_ = "Cannot align a Camera under a singular parent transform.";
            return false;
        }
        local *= inverseParent;
    }

    TransformComponent aligned{};
    if (!TryDecomposeTransformComponent(local, aligned)) {
        status_ = "Could not calculate the aligned Camera transform.";
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->transform = aligned;
    RecordImmediateEdit("Align Camera to Scene View", before, selectionBefore);
    status_ = "Aligned " + entity->name + " to the Scene View.";
    return true;
}

bool EditorScene::AlignSceneViewToSelectedCamera() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before moving the Scene View.";
        return false;
    }
    if (!world_.TryGetWorldMatrix(entity->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix),
                                        worldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }
    sceneViewCamera_.SetPosition(worldTransform.position);
    sceneViewCamera_.SetRotation(
        {DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    status_ = "Moved the Scene View to " + entity->name + ".";
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
    entity->materialOverride = MaterialOverrideComponent{};
    loadedModels_.erase(assetPath);
    animatorModels_.clear();
    selection_ = entityId;
    RecordImmediateEdit("Create Model Entity", before, selectionBefore);
    status_ = "Created model entity: " + assetPath;
}

bool EditorScene::SaveSelectionAsPrefab() {
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
    const std::optional<std::filesystem::path> destination =
        ShowSavePrefabDialog(rootEntity->name);
    if (!destination) {
        status_ = "Prefab save cancelled.";
        return false;
    }

    std::unordered_set<EntityId, EntityIdHash> includedIds;
    includedIds.insert(roots.front());
    for (const WorldEntity& candidate : world_.Entities()) {
        EntityId current = candidate.parent;
        for (size_t depth = 0u; current.IsValid() && depth <= world_.Entities().size();
             ++depth) {
            if (current == roots.front()) {
                includedIds.insert(candidate.id);
                break;
            }
            const WorldEntity* parent = world_.Find(current);
            current = parent != nullptr ? parent->parent : EntityId{};
        }
    }

    std::vector<WorldEntity> entities;
    entities.reserve(includedIds.size());
    for (const WorldEntity& source : world_.Entities()) {
        if (!includedIds.contains(source.id)) {
            continue;
        }
        WorldEntity prefabEntity = source;
        if (prefabEntity.id == roots.front()) {
            prefabEntity.parent = {};
        }
        for (BehaviorComponent& script : prefabEntity.scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type == ScriptPropertyType::Entity &&
                    property.entityValue.IsValid() &&
                    !includedIds.contains(property.entityValue)) {
                    property.entityValue = {};
                }
            }
        }
        entities.push_back(std::move(prefabEntity));
    }
    World prefab;
    std::string error;
    if (!prefab.ReplaceEntities(std::move(entities), &error) ||
        !WorldSerializer::Save(prefab, *destination, &error)) {
        status_ = "Prefab save failed: " + error;
        return false;
    }
    RefreshAssetBrowser();
    std::error_code relativeError;
    selectedAsset_ = std::filesystem::relative(*destination, assetRoot_, relativeError);
    if (relativeError) {
        selectedAsset_.clear();
    }
    status_ = "Saved Prefab: " + destination->string();
    return true;
}

bool EditorScene::TryNormalizeAudioAssetReference(const std::filesystem::path& path,
                                                  std::string& assetPath) {
    if (!AssetImport::IsAudioFile(path)) {
        status_ = "The dropped audio asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped audio asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped audio asset path is too long.";
        return false;
    }
    return true;
}

bool EditorScene::InstantiatePrefabAsset(
    const std::filesystem::path& path, EntityId parent,
    std::optional<DirectX::XMFLOAT3> position) {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before instantiating a Prefab.";
        return false;
    }
    const std::optional<std::filesystem::path> resolved = ResolveProjectAssetPath(path);
    std::error_code filesystemError;
    if (!resolved || !IsPrefabAsset(*resolved) ||
        !std::filesystem::is_regular_file(*resolved, filesystemError) || filesystemError ||
        !IsPathWithinRoot(assetRoot_, *resolved)) {
        status_ = "The Prefab asset is invalid or outside the project assets directory.";
        return false;
    }
    World prefab;
    std::string error;
    if (!WorldSerializer::Load(*resolved, prefab, &error)) {
        status_ = "Prefab load failed: " + error;
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    std::vector<EntityId> roots;
    if (!world_.InstantiateEntityHierarchies(prefab, parent, roots, &error) ||
        roots.empty()) {
        status_ = "Prefab instantiate failed: " + error;
        return false;
    }
    if (position && roots.size() == 1u) {
        if (WorldEntity* root = world_.Find(roots.front())) {
            root->transform.position = *position;
        }
    }
    hierarchySelection_.clear();
    hierarchySelection_.insert(roots.begin(), roots.end());
    selection_ = roots.front();
    hierarchySelectionAnchor_ = selection_;
    RecordImmediateEdit("Instantiate Prefab", before, selectionBefore);
    status_ = "Instantiated Prefab: " + resolved->filename().string();
    return true;
}

void EditorScene::RefreshAssetBrowser() {
    assetPreviewAsset_.clear();
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
    modelAssets_.clear();
    textureAssets_.clear();
    audioAssets_.clear();
    scriptAssets_.clear();
    prefabAssets_.clear();
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
                   (AssetImport::IsModelFile(entry.path()) ||
                    AssetImport::IsTextureFile(entry.path()) ||
                    AssetImport::IsAudioFile(entry.path()) ||
                    IsPrefabAsset(entry.path()) ||
                    ScriptAssets::IsScriptFile(entry.path()) ||
                    ScriptAssets::IsScriptSourceFile(entry.path()))) {
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
            (AssetImport::IsModelFile(iterator->path()) ||
             AssetImport::IsTextureFile(iterator->path()) ||
             AssetImport::IsAudioFile(iterator->path()) ||
             IsPrefabAsset(iterator->path()) ||
             ScriptAssets::IsScriptFile(iterator->path()))) {
            std::filesystem::path relative =
                std::filesystem::relative(iterator->path(), assetRoot_, error);
            if (!error) {
                auto& assets = IsPrefabAsset(iterator->path())
                                   ? prefabAssets_
                               : AssetImport::IsTextureFile(iterator->path())
                                   ? textureAssets_
                               : AssetImport::IsAudioFile(iterator->path())
                                   ? audioAssets_
                               : ScriptAssets::IsScriptFile(iterator->path())
                                         ? scriptAssets_
                                         : modelAssets_;
                assets.push_back((std::filesystem::path("assets") / relative).lexically_normal());
            }
        }
        iterator.increment(error);
    }
    std::ranges::sort(modelAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(textureAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(audioAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(scriptAssets_, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    std::ranges::sort(prefabAssets_, {}, [](const std::filesystem::path& path) {
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
    world_.SetPhysicsSettings(physicsSettings_);
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
    if (IsInPlayMode()) {
        return;
    }
    if (!pendingHistoryEdit_) {
        pendingHistoryEdit_ = PendingHistoryEdit{std::move(label), CaptureHistoryState()};
    }
}

void EditorScene::CommitHistoryEdit() {
    if (IsInPlayMode()) {
        pendingHistoryEdit_.reset();
        return;
    }
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
    if (IsInPlayMode()) {
        pendingHistoryEdit_.reset();
        return;
    }
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
    if (IsInPlayMode()) {
        return;
    }
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
    if (IsInPlayMode()) {
        return;
    }
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
    if (IsInPlayMode()) {
        return;
    }
    dirty_ = WorldSerializer::Serialize(world_) != savedWorldSnapshot_;
}

void EditorScene::ResolveMeshResources() {
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr ||
        ctx_->rendering.texture == nullptr) {
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
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride || entity.materialOverride->baseColorTexturePath.empty() ||
            loadedTextures_.contains(entity.materialOverride->baseColorTexturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.materialOverride->baseColorTexturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.materialOverride->baseColorTexturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.materialOverride) {
            continue;
        }
        const std::string* paths[] = {
            &entity.materialOverride->normalTexturePath,
            &entity.materialOverride->roughnessTexturePath,
            &entity.materialOverride->metallicTexturePath,
        };
        for (const std::string* path : paths) {
            if (path->empty() || loadedLinearTextures_.contains(*path)) {
                continue;
            }
            const std::optional<std::filesystem::path> resolved =
                ResolveProjectAssetPath(*path);
            if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
                continue;
            }
            loadedLinearTextures_.emplace(
                *path, TextureHandle(ctx_->rendering.texture->LoadLinear(resolved->wstring())));
        }
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

TextureHandle EditorScene::ResolveBaseColorTexture(
    const MaterialOverrideComponent& component) const {
    const auto found = loadedTextures_.find(component.baseColorTexturePath);
    return found != loadedTextures_.end() ? found->second : TextureHandle{};
}

TextureHandle EditorScene::ResolveNormalTexture(
    const MaterialOverrideComponent& component) const {
    return ResolveLinearTexture(component.normalTexturePath);
}

TextureHandle EditorScene::ResolveLinearTexture(const std::string& path) const {
    const auto found = loadedLinearTextures_.find(path);
    return found != loadedLinearTextures_.end() ? found->second : TextureHandle{};
}

bool EditorScene::UpdateGameViewCamera() {
    const WorldEntity* primaryCamera = nullptr;
    for (const WorldEntity& entity : world_.Entities()) {
        if (world_.IsActiveInHierarchy(entity.id) && entity.camera &&
            entity.camera->enabled && entity.camera->primary) {
            primaryCamera = &entity;
            break;
        }
    }
    if (primaryCamera == nullptr) {
        return false;
    }

    return UpdateCameraFromEntity(primaryCamera->id, gameViewCamera_,
                                  gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight());
}

bool EditorScene::UpdateCameraFromEntity(EntityId entityId, Camera& targetCamera, int width,
                                         int height) const {
    const WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !world_.IsActiveInHierarchy(entityId) || !entity->camera) {
        return false;
    }

    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (!world_.TryGetWorldMatrix(entity->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix),
                                        worldTransform)) {
        return false;
    }
    targetCamera.SetPosition(worldTransform.position);
    targetCamera.SetRotation(
        {DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
         DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    const CameraComponent& component = *entity->camera;
    targetCamera.SetAspect(static_cast<float>((std::max)(1, width)) /
                           static_cast<float>((std::max)(1, height)));
    if (component.projection == CameraProjection::Perspective) {
        targetCamera.SetPerspectiveFovDeg(component.fieldOfViewDegrees);
    } else {
        targetCamera.SetOrthographicHeight(component.orthographicHeight);
    }
    targetCamera.SetClipRange(component.nearClip, component.farClip);
    return true;
}

bool EditorScene::DrawSelectedCameraPreview(const ImVec2& imageMin,
                                            const ImVec2& imageMax) {
    const WorldEntity* entity = world_.Find(selection_);
    ImVec2 previewMin{};
    ImVec2 previewMax{};
    if (entity == nullptr || !world_.IsActiveInHierarchy(selection_) || !entity->camera ||
        !cameraPreviewSurface_.IsReady() ||
        !cameraPreviewPostProcess_.IsReady() || ctx_ == nullptr ||
        ctx_->rendering.dxCommon == nullptr || ctx_->rendering.model == nullptr ||
        !TryGetCameraPreviewRect(imageMin, imageMax, previewMin, previewMax) ||
        !UpdateCameraFromEntity(entity->id, cameraPreviewCamera_,
                                cameraPreviewSurface_.GetWidth(),
                                cameraPreviewSurface_.GetHeight())) {
        return false;
    }

    sceneRenderer_.Render(renderScene_, cameraPreviewCamera_, cameraPreviewSurface_,
                          {0.025f, 0.035f, 0.055f, 1.0f});
    cameraPreviewSurface_.TransitionDepthToShaderResource();
    cameraPreviewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
    const PostProcessOutputTarget target{
        cameraPreviewSurface_.GetOutputRtvHandle(),
        static_cast<uint32_t>(cameraPreviewSurface_.GetWidth()),
        static_cast<uint32_t>(cameraPreviewSurface_.GetHeight()),
        DirectXCommon::kBackBufferFormat,
    };
    cameraPreviewPostProcess_.DrawToTarget(cameraPreviewSurface_.GetSceneColorGpuHandle(),
                                           cameraPreviewSurface_.GetDepthGpuHandle(), target);
    cameraPreviewSurface_.EndOutputPass();
    cameraPreviewSurface_.TransitionDepthToWrite();
    ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const D3D12_GPU_DESCRIPTOR_HANDLE output = cameraPreviewSurface_.GetOutputGpuHandle();
    drawList->AddImage(static_cast<ImTextureID>(output.ptr), previewMin, previewMax);
    drawList->AddRect(previewMin, previewMax, IM_COL32(255, 184, 56, 255), 3.0f, 0, 2.0f);
    const std::string label = "Camera Preview  |  " + entity->name;
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    drawList->AddRectFilled(previewMin,
                            {previewMin.x + (std::min)(previewMax.x - previewMin.x,
                                                      textSize.x + 12.0f),
                             previewMin.y + textSize.y + 8.0f},
                            IM_COL32(18, 22, 30, 220), 3.0f,
                            ImDrawFlags_RoundCornersTopLeft);
    drawList->AddText({previewMin.x + 6.0f, previewMin.y + 4.0f},
                      IM_COL32(240, 242, 248, 255), label.c_str());
    return ImGui::IsMouseHoveringRect(previewMin, previewMax);
}

void EditorScene::UpdateAssetPreview() {
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    if (relative == assetPreviewAsset_) {
        return;
    }
    StopAudioAssetPreview();
    audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
    assetPreviewAsset_ = relative;
    assetPreviewModel_ = {};
    assetPreviewAnimation_.clear();
    assetPreviewAnimationLoop_ = true;
    assetPreviewAnimationSpeed_ = 1.0f;
    assetPreviewRotationDegrees_ = {0.0f, 180.0f};
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
    const std::string previewKey = physical.lexically_normal().generic_string();
    const auto cachedPreview = assetPreviewModels_.find(previewKey);
    if (cachedPreview != assetPreviewModels_.end()) {
        assetPreviewModel_ = cachedPreview->second;
    } else {
        assetPreviewModel_ = ctx_->rendering.model->LoadUniqueHandle(physical.wstring());
        if (assetPreviewModel_.IsValid()) {
            assetPreviewModels_.emplace(previewKey, assetPreviewModel_);
        }
    }
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

void EditorScene::BuildRenderScene() {
    renderScene_.BeginFrame();
    ModelManager* models = ctx_ ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.meshRenderer ||
            !entity.meshRenderer->enabled ||
            !entity.materialOverride || !entity.materialOverride->enabled) {
            continue;
        }
        const auto runtimeAnimator = std::ranges::find_if(
            runtimeAnimators_, [&entity](const RuntimeAnimator& runtime) {
                return runtime.entity == entity.id;
            });
        const bool animated = runtimeAnimator != runtimeAnimators_.end();
        const ModelHandle handle =
            animated ? runtimeAnimator->model : ResolveModel(*entity.meshRenderer);
        const Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        DirectX::XMFLOAT4X4 worldMatrix{};
        if (model == nullptr || !world_.TryGetWorldMatrix(entity.id, worldMatrix)) {
            continue;
        }
        if (animated) {
            models->PrepareSkinning(handle);
        }
        DirectX::XMMATRIX renderWorld = DirectX::XMLoadFloat4x4(&worldMatrix);
        if (animated && model->hasRootAnimation) {
            renderWorld = DirectX::XMLoadFloat4x4(&model->rootAnimationMatrix) * renderWorld;
        }
        DirectX::XMStoreFloat4x4(&worldMatrix, renderWorld);
        const Transform transform = DecomposeTransform(worldMatrix);
        auto submit = [&](uint32_t meshId, uint32_t materialId, uint32_t textureId,
                          uint32_t normalTextureId,
                          const D3D12_VERTEX_BUFFER_VIEW* vertexBufferOverride = nullptr) {
            if (!IsValidResourceId(meshId)) {
                return;
            }
            RenderMeshItem item{};
            item.mesh = &models->GetMesh(meshId);
            if (IsValidResourceId(materialId)) {
                item.material = models->GetMaterial(materialId);
            }
            if (entity.materialOverride && entity.materialOverride->enabled) {
                item.material.color = entity.materialOverride->baseColor;
                item.material.metallic = entity.materialOverride->metallic;
                item.material.roughness = entity.materialOverride->roughness;
                item.material.normalStrength = entity.materialOverride->normalStrength;
                switch (entity.materialOverride->blendMode) {
                case MaterialSurfaceBlendMode::Opaque:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Opaque);
                    break;
                case MaterialSurfaceBlendMode::Cutout:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Cutout);
                    break;
                case MaterialSurfaceBlendMode::Transparent:
                    item.material.blendMode = static_cast<int32_t>(BlendMode::Transparent);
                    break;
                }
                item.material.alphaCutoff = entity.materialOverride->alphaCutoff;
                switch (entity.materialOverride->cullMode) {
                case MaterialSurfaceCullMode::None:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::None);
                    break;
                case MaterialSurfaceCullMode::Front:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::Front);
                    break;
                case MaterialSurfaceCullMode::Back:
                    item.material.cullMode = static_cast<int32_t>(MaterialCullMode::Back);
                    break;
                }
                item.material.depthWrite = entity.materialOverride->depthWrite ? 1 : 0;
                const TextureHandle overrideTexture =
                    ResolveBaseColorTexture(*entity.materialOverride);
                if (overrideTexture.IsValid()) {
                    item.textureId = overrideTexture.Get();
                    item.material.baseColorTextureId = overrideTexture.Get();
                    item.material.enableTexture = 1;
                }
                const TextureHandle normalTexture =
                    ResolveNormalTexture(*entity.materialOverride);
                if (normalTexture.IsValid()) {
                    item.normalTextureId = normalTexture.Get();
                    item.material.normalTextureId = normalTexture.Get();
                    item.material.enableNormalMap = 1;
                }
                const TextureHandle roughnessTexture =
                    ResolveLinearTexture(entity.materialOverride->roughnessTexturePath);
                const TextureHandle metallicTexture =
                    ResolveLinearTexture(entity.materialOverride->metallicTexturePath);
                if (roughnessTexture.IsValid()) {
                    item.material.roughnessTextureId = roughnessTexture.Get();
                }
                if (metallicTexture.IsValid()) {
                    item.material.metallicTextureId = metallicTexture.Get();
                }
                switch (entity.materialOverride->pbrTexturePacking) {
                case MaterialPbrTexturePacking::Separate:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::Separate);
                    break;
                case MaterialPbrTexturePacking::OcclusionRoughnessMetallic:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::OcclusionRoughnessMetallic);
                    break;
                case MaterialPbrTexturePacking::MetallicRoughness:
                    item.material.pbrTexturePacking =
                        static_cast<int32_t>(PbrTexturePacking::MetallicRoughness);
                    break;
                }
            }
            item.transform = transform;
            if (!IsValidResourceId(item.textureId)) {
                item.textureId = textureId;
            }
            if (!IsValidResourceId(item.normalTextureId)) {
                item.normalTextureId = normalTextureId;
            }
            item.objectId = static_cast<uint32_t>(EntityIdHash{}(entity.id));
            if (vertexBufferOverride != nullptr) {
                item.vertexBufferOverride = *vertexBufferOverride;
            }
            renderScene_.SubmitMesh(item);
        };
        if (!model->subMeshes.empty()) {
            for (const ModelSubMesh& subMesh : model->subMeshes) {
                const D3D12_VERTEX_BUFFER_VIEW* animatedVertices =
                    animated && subMesh.skinCluster.skinnedVertexResource
                        ? &subMesh.skinCluster.skinnedVertexBufferView
                        : nullptr;
                submit(subMesh.meshId, subMesh.materialId, subMesh.textureId,
                       subMesh.normalTextureId, animatedVertices);
            }
        } else {
            submit(model->meshId, model->materialId, model->textureId, kInvalidResourceId);
        }
    }
}

bool EditorScene::IsInPlayMode() const {
    return playModeState_ != PlayModeState::Edit;
}

void EditorScene::EnterPlayMode() {
    if (IsInPlayMode()) {
        return;
    }
    CommitHistoryEdit();
    StopAudioAssetPreview();
    gizmoWasUsing_ = false;
    boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
    boxColliderGizmoEntity_ = {};
    characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
    characterControllerGizmoEntity_ = {};
    const std::string runtimeSnapshot = WorldSerializer::Serialize(world_);
    World runtimeWorld;
    std::string error;
    if (runtimeSnapshot.empty() ||
        !WorldSerializer::Deserialize(runtimeSnapshot, runtimeWorld, &error)) {
        status_ = "Could not enter Play Mode: " +
                  (error.empty() ? std::string("scene clone failed.") : error);
        return;
    }
    playModeSelectionSnapshot_ = selection_;
    playModeDirtySnapshot_ = dirty_;
    editModeWorld_.emplace(std::move(world_));
    world_ = std::move(runtimeWorld);
    world_.SetPhysicsSettings(physicsSettings_);
    std::string runtimeError;
    const bool allBehaviorsStarted = BeginRuntimeWorld(&runtimeError);
    playModeState_ = PlayModeState::Playing;
    showGamePanel_ = true;
    focusGamePanelRequested_ = true;
    status_ = allBehaviorsStarted
                  ? "Entered Play Mode. Runtime changes will be discarded on Stop."
                  : "Error: Entered Play Mode with runtime setup issue(s): " +
                        runtimeError;
}

void EditorScene::StopPlayMode() {
    if (!IsInPlayMode()) {
        return;
    }
    if (!editModeWorld_) {
        status_ = "Could not stop Play Mode: Edit World is unavailable.";
        return;
    }
    ReleaseGameInputCapture();
    EndRuntimeWorld();
    world_ = std::move(*editModeWorld_);
    editModeWorld_.reset();
    selection_ = world_.Contains(playModeSelectionSnapshot_) ? playModeSelectionSnapshot_
                                                              : EntityId{};
    hierarchySelection_.clear();
    if (selection_.IsValid()) {
        hierarchySelection_.insert(selection_);
    }
    hierarchySelectionAnchor_ = selection_;
    pendingHistoryEdit_.reset();
    activeGizmoEntity_ = {};
    activeGizmoWorldTransforms_.clear();
    gizmoWasUsing_ = false;
    boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
    boxColliderGizmoEntity_ = {};
    characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
    characterControllerGizmoEntity_ = {};
    dirty_ = playModeDirtySnapshot_;
    playModeSelectionSnapshot_ = {};
    playModeState_ = PlayModeState::Edit;
    status_ = "Stopped Play Mode and restored the Edit scene.";
}

void EditorScene::TogglePlayPause() {
    if (playModeState_ == PlayModeState::Playing) {
        ReleaseGameInputCapture();
        PauseRuntimeAudio(true);
        playModeState_ = PlayModeState::Paused;
        status_ = "Paused Play Mode.";
    } else if (playModeState_ == PlayModeState::Paused) {
        PauseRuntimeAudio(false);
        playModeState_ = PlayModeState::Playing;
        status_ = "Resumed Play Mode.";
    }
}

void EditorScene::ReleaseGameInputCapture() {
    if (!gameInputCaptured_) {
        return;
    }
    SetCursorPos(gameInputCursorRestoreX_, gameInputCursorRestoreY_);
    gameInputCaptured_ = false;
}

void EditorScene::StepRuntimeWorld() {
    if (playModeState_ != PlayModeState::Paused) {
        return;
    }
    UpdateRuntimeWorld(kRuntimeStepDeltaTime);
    status_ = "Advanced the paused Runtime World by one frame.";
}

bool EditorScene::BeginRuntimeWorld(std::string* error) {
    runtimeFrameCount_ = 0;
    runtimeElapsedSeconds_ = 0.0;
    runtimeTriggers_.Clear();
    runtimeBehaviors_.Clear();
    bool valid = true;
    for (const WorldEntity& entity : world_.Entities()) {
        for (const BehaviorComponent& script : entity.scripts) {
            if (!script.enabled || script.type.empty()) {
                continue;
            }
            std::string requirementError;
            if (!behaviorRegistry_.ValidateRequirements(script.type, entity,
                                                         &requirementError)) {
                valid = false;
                if (error != nullptr && error->empty()) {
                    *error = entity.name + " (" + script.type + "): " + requirementError;
                }
                continue;
            }
            std::unique_ptr<Behavior> behavior = behaviorRegistry_.Create(script.type);
            if (behavior != nullptr) {
                if (!behaviorRegistry_.Configure(script.type, script, *behavior)) {
                    valid = false;
                    if (error != nullptr && error->empty()) {
                        *error = entity.name + " (" + script.type +
                                 "): Script properties could not be configured.";
                    }
                    continue;
                }
                runtimeBehaviors_.Attach(entity.id, std::move(behavior));
            } else {
                valid = false;
                if (error != nullptr && error->empty()) {
                    *error = entity.name + " (" + script.type +
                             "): Behavior creation failed.";
                }
            }
        }
    }
    runtimeBehaviors_.Start(world_);
    std::string audioError;
    if (!BeginRuntimeAudio(&audioError)) {
        valid = false;
        if (error != nullptr && error->empty()) {
            *error = audioError;
        }
    }
    std::string animatorError;
    if (!BeginRuntimeAnimators(&animatorError)) {
        valid = false;
        if (error != nullptr && error->empty()) {
            *error = animatorError;
        }
    }
    if (valid && error != nullptr) {
        error->clear();
    }
    return valid;
}

bool EditorScene::BeginRuntimeAnimators(std::string* error) {
    EndRuntimeAnimators();
    if (std::ranges::none_of(world_.Entities(), [](const WorldEntity& entity) {
            return entity.animator && entity.animator->enabled;
        })) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        if (error != nullptr) {
            *error = "Model service is unavailable.";
        }
        return false;
    }
    bool valid = true;
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.animator || !entity.animator->enabled) {
            continue;
        }
        if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model ||
            entity.meshRenderer->modelPath.empty()) {
            valid = false;
            if (error != nullptr && error->empty()) {
                *error = entity.name + ": Animator requires a Model MeshRenderer.";
            }
            continue;
        }
        const std::optional<std::filesystem::path> path =
            ResolveProjectAssetPath(entity.meshRenderer->modelPath);
        const std::string cacheKey = entity.id.ToString() + "|" + entity.meshRenderer->modelPath;
        const auto cached = animatorModels_.find(cacheKey);
        const ModelHandle handle =
            cached != animatorModels_.end()
                ? cached->second
                : (path ? models->LoadUniqueHandle(path->wstring()) : ModelHandle{});
        Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
        if (model == nullptr || model->animations.empty()) {
            valid = false;
            if (error != nullptr && error->empty()) {
                *error = entity.name + ": Animator model has no animation clips.";
            }
            continue;
        }
        animatorModels_.insert_or_assign(cacheKey, handle);
        const AnimatorComponent& animator = *entity.animator;
        const std::string clip = animator.clip.empty() ? model->animations.begin()->first
                                                       : animator.clip;
        if (!model->animations.contains(clip)) {
            valid = false;
            if (error != nullptr && error->empty()) {
                *error = entity.name + ": Animator clip was not found: " + clip;
            }
            continue;
        }
        models->PlayAnimation(handle, clip, animator.loop);
        if (!animator.playOnAwake) {
            model->isPlaying = false;
            models->UpdateAnimation(handle, 0.0f);
        }
        if (WorldEntity* runtimeEntity = world_.Find(entity.id); runtimeEntity != nullptr &&
            runtimeEntity->animator) {
            runtimeEntity->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::None;
            runtimeEntity->animator->runtimeRequestedClip.clear();
            runtimeEntity->animator->runtimeClip = clip;
            runtimeEntity->animator->runtimeLoop = animator.loop;
            runtimeEntity->animator->runtimeFadeDuration = 0.0f;
            runtimeEntity->animator->runtimePlaying = model->isPlaying;
            runtimeEntity->animator->runtimeFinished = model->animationFinished;
            runtimeEntity->animator->runtimeTime = model->animationTime;
            runtimeEntity->animator->runtimeDuration = model->animations.at(clip).duration;
            runtimeEntity->animator->runtimeNormalizedTime = 0.0f;
            runtimeEntity->animator->runtimeTransitioning = false;
            runtimeEntity->animator->runtimeTransitionProgress = 0.0f;
        }
        runtimeAnimators_.push_back({entity.id, handle});
    }
    if (valid && error != nullptr) {
        error->clear();
    }
    return valid;
}

bool EditorScene::ValidateWorldBehaviorRequirements(std::string* error) const {
    for (const WorldEntity& entity : world_.Entities()) {
        for (const BehaviorComponent& script : entity.scripts) {
            if (!script.enabled || script.type.empty()) {
                continue;
            }
            std::string requirementError;
            if (!behaviorRegistry_.ValidateRequirements(script.type, entity,
                                                         &requirementError)) {
                if (error != nullptr) {
                    *error = entity.name + " (" + script.type + "): " + requirementError;
                }
                return false;
            }
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool EditorScene::TryNormalizeScriptAssetReference(
    const std::filesystem::path& path, std::string& assetPath,
    std::filesystem::path& physicalPath) {
    if (!ScriptAssets::IsScriptFile(path)) {
        status_ = "The dropped Script asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped Script asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped Script asset path is too long.";
        return false;
    }
    physicalPath = *resolvedPath;
    return true;
}

void EditorScene::UpdateRuntimeWorld(float deltaTime) {
    const float safeDeltaTime =
        std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 0.1f) : 0.0f;
    runtimeBehaviors_.Update(safeDeltaTime);
    runtimeTriggers_.Update(world_, runtimeBehaviors_);
    UpdateRuntimeAnimators(safeDeltaTime);
    UpdateRuntimeAudio();
    ++runtimeFrameCount_;
    runtimeElapsedSeconds_ += static_cast<double>(safeDeltaTime);
}

void EditorScene::EndRuntimeWorld() {
    EndRuntimeAnimators();
    EndRuntimeAudio();
    runtimeTriggers_.Clear();
    runtimeBehaviors_.Clear();
    runtimeFrameCount_ = 0;
    runtimeElapsedSeconds_ = 0.0;
}

void EditorScene::UpdateRuntimeAnimators(float deltaTime) {
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const RuntimeAnimator& runtime : runtimeAnimators_) {
        WorldEntity* entity = world_.Find(runtime.entity);
        if (entity == nullptr || !entity->animator) {
            continue;
        }
        AnimatorComponent& animator = *entity->animator;
        Model* model = models->GetModel(runtime.model);
        if (model == nullptr) {
            animator.runtimePlaying = false;
            animator.runtimeFinished = false;
            animator.runtimeClip.clear();
            animator.runtimeTime = 0.0f;
            animator.runtimeDuration = 0.0f;
            animator.runtimeNormalizedTime = 0.0f;
            animator.runtimeTransitioning = false;
            animator.runtimeTransitionProgress = 0.0f;
            continue;
        }
        const AnimatorComponent::RuntimeCommand command = animator.runtimeCommand;
        animator.runtimeCommand = AnimatorComponent::RuntimeCommand::None;
        if (command == AnimatorComponent::RuntimeCommand::Stop) {
            const std::string clip = model->currentAnimation;
            if (!clip.empty() && model->animations.contains(clip)) {
                models->PlayAnimation(runtime.model, clip, model->isLoop);
                model->isPlaying = false;
                model->animationFinished = false;
                models->UpdateAnimation(runtime.model, 0.0f);
            }
        } else if (command == AnimatorComponent::RuntimeCommand::Play &&
                   model->animations.contains(animator.runtimeRequestedClip)) {
            if (model->currentAnimation != animator.runtimeRequestedClip ||
                (!model->isPlaying && !model->animationFinished)) {
                models->PlayAnimation(runtime.model, animator.runtimeRequestedClip,
                                      animator.runtimeLoop);
            } else {
                model->isLoop = animator.runtimeLoop;
            }
        } else if (command == AnimatorComponent::RuntimeCommand::CrossFade &&
                   model->animations.contains(animator.runtimeRequestedClip)) {
            models->CrossFadeAnimation(runtime.model, animator.runtimeRequestedClip,
                                       animator.runtimeFadeDuration, animator.runtimeLoop);
        }
        if (animator.enabled && world_.IsActiveInHierarchy(runtime.entity) && model->isPlaying) {
            models->UpdateAnimation(runtime.model, deltaTime * animator.speed);
        }
        animator.runtimePlaying = animator.enabled && world_.IsActiveInHierarchy(runtime.entity) &&
                                  model->isPlaying;
        animator.runtimeFinished = model->animationFinished;
        animator.runtimeClip = model->currentAnimation;
        animator.runtimeTime = model->animationTime;
        const auto currentClip = model->animations.find(model->currentAnimation);
        animator.runtimeDuration = currentClip != model->animations.end()
                                       ? (std::max)(currentClip->second.duration, 0.0f)
                                       : 0.0f;
        animator.runtimeNormalizedTime = animator.runtimeDuration > 0.0f
                                             ? std::clamp(animator.runtimeTime /
                                                              animator.runtimeDuration,
                                                          0.0f, 1.0f)
                                             : 0.0f;
        animator.runtimeTransitioning = !model->blendSourceAnimation.empty();
        animator.runtimeTransitionProgress =
            animator.runtimeTransitioning && model->blendDuration > 0.0f
                ? std::clamp(model->blendTime / model->blendDuration, 0.0f, 1.0f)
                : 0.0f;
    }
}

void EditorScene::EndRuntimeAnimators() {
    for (const RuntimeAnimator& runtime : runtimeAnimators_) {
        if (WorldEntity* entity = world_.Find(runtime.entity); entity != nullptr &&
            entity->animator) {
            entity->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::None;
            entity->animator->runtimeRequestedClip.clear();
            entity->animator->runtimeClip.clear();
            entity->animator->runtimeLoop = true;
            entity->animator->runtimeFadeDuration = 0.0f;
            entity->animator->runtimePlaying = false;
            entity->animator->runtimeFinished = false;
            entity->animator->runtimeTime = 0.0f;
            entity->animator->runtimeDuration = 0.0f;
            entity->animator->runtimeNormalizedTime = 0.0f;
            entity->animator->runtimeTransitioning = false;
            entity->animator->runtimeTransitionProgress = 0.0f;
        }
    }
    runtimeAnimators_.clear();
}

bool EditorScene::BeginRuntimeAudio(std::string* error) {
    EndRuntimeAudio();
    if (std::ranges::none_of(world_.Entities(), [](const WorldEntity& entity) {
            return entity.audioSource.has_value();
        })) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        if (error != nullptr) {
            *error = "Audio service is unavailable.";
        }
        return false;
    }
    bool valid = true;
    const size_t activeListenerCount = static_cast<size_t>(std::ranges::count_if(
        world_.Entities(), [this](const WorldEntity& entity) {
            return world_.IsActiveInHierarchy(entity.id) && entity.audioListener &&
                   entity.audioListener->enabled;
        }));
    if (activeListenerCount > 1u) {
        AddConsoleEntry("Multiple enabled Audio Listeners found. The first one will be used.",
                        ConsoleSeverity::Warning);
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.audioSource) {
            continue;
        }
        RuntimeAudioSource runtime{};
        runtime.entity = entity.id;
        if (!entity.audioSource->clipPath.empty()) {
            const std::optional<std::filesystem::path> clip =
                ResolveProjectAssetPath(entity.audioSource->clipPath);
            if (!clip || !AssetImport::IsAudioFile(*clip) ||
                !sound->TryLoad(clip->wstring(), runtime.soundId)) {
                valid = false;
                if (error != nullptr && error->empty()) {
                    *error = entity.name + ": AudioSource clip could not be loaded.";
                }
            }
        }
        runtimeAudioSources_.push_back(runtime);
    }
    UpdateRuntimeAudio();
    if (valid && error != nullptr) {
        error->clear();
    }
    return valid;
}

void EditorScene::UpdateRuntimeAudio() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        return;
    }

    const WorldEntity* listener = nullptr;
    for (const WorldEntity& entity : world_.Entities()) {
        if (world_.IsActiveInHierarchy(entity.id) && entity.audioListener &&
            entity.audioListener->enabled) {
            listener = &entity;
            break;
        }
    }
    if (listener == nullptr) {
        for (const WorldEntity& entity : world_.Entities()) {
            if (world_.IsActiveInHierarchy(entity.id) && entity.camera &&
                entity.camera->enabled && entity.camera->primary) {
                listener = &entity;
                break;
            }
        }
    }
    if (listener != nullptr) {
        DirectX::XMFLOAT4X4 matrix{};
        if (world_.TryGetWorldMatrix(listener->id, matrix)) {
            using namespace DirectX;
            const XMMATRIX world = XMLoadFloat4x4(&matrix);
            XMVECTOR forward = XMVector3TransformNormal(g_XMIdentityR2, world);
            XMVECTOR up = XMVector3TransformNormal(g_XMIdentityR1, world);
            forward = XMVectorGetX(XMVector3LengthSq(forward)) > 1.0e-8f
                          ? XMVector3Normalize(forward)
                          : g_XMIdentityR2;
            up = XMVectorGetX(XMVector3LengthSq(up)) > 1.0e-8f
                     ? XMVector3Normalize(up)
                     : g_XMIdentityR1;
            XMFLOAT3 storedForward{};
            XMFLOAT3 storedUp{};
            XMStoreFloat3(&storedForward, forward);
            XMStoreFloat3(&storedUp, up);
            sound->SetListener({matrix._41, matrix._42, matrix._43}, storedForward, storedUp);
        }
    }

    for (RuntimeAudioSource& runtime : runtimeAudioSources_) {
        WorldEntity* entity = world_.Find(runtime.entity);
        AudioSourceComponent* source =
            entity != nullptr && entity->audioSource ? &*entity->audioSource : nullptr;
        const AudioSourceComponent::RuntimeCommand command =
            source != nullptr ? source->runtimeCommand
                              : AudioSourceComponent::RuntimeCommand::None;
        const uint32_t pendingOneShots = source != nullptr ? source->pendingOneShots : 0u;
        if (source != nullptr) {
            source->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
            source->pendingOneShots = 0u;
        }
        const auto stopOneShots = [&] {
            for (const uint32_t voice : runtime.oneShotVoices) {
                sound->Stop(voice);
            }
            runtime.oneShotVoices.clear();
        };
        const bool active = source != nullptr && source->enabled &&
                            world_.IsActiveInHierarchy(runtime.entity) &&
                            runtime.soundId != ISoundService::kInvalidSoundId;
        if (!active) {
            if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
                sound->Stop(runtime.voice);
            }
            runtime.voice = ISoundService::kInvalidVoiceHandle;
            stopOneShots();
            runtime.activated = false;
            if (source != nullptr) {
                source->runtimePlaying = false;
            }
            continue;
        }

        if (command == AudioSourceComponent::RuntimeCommand::Stop) {
            if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
                sound->Stop(runtime.voice);
            }
            runtime.voice = ISoundService::kInvalidVoiceHandle;
            stopOneShots();
            runtime.activated = true;
        }
        std::erase_if(runtime.oneShotVoices,
                      [sound](uint32_t voice) { return !sound->IsPlaying(voice); });
        const auto playVoice = [&](bool loop) {
            uint32_t voice = ISoundService::kInvalidVoiceHandle;
            if (source->spatial) {
                DirectX::XMFLOAT4X4 matrix{};
                if (world_.TryGetWorldMatrix(runtime.entity, matrix)) {
                    voice = sound->Play3D(runtime.soundId,
                                          {matrix._41, matrix._42, matrix._43},
                                          source->volume, loop);
                }
            } else {
                voice = sound->Play(runtime.soundId, source->volume, loop);
            }
            return voice;
        };
        const auto startVoice = [&] {
            if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
                sound->Stop(runtime.voice);
            }
            runtime.voice = playVoice(source->loop);
        };
        if (command == AudioSourceComponent::RuntimeCommand::Play) {
            runtime.activated = true;
            startVoice();
        } else if (!runtime.activated) {
            runtime.activated = true;
            if (source->playOnAwake) {
                startVoice();
            }
        }
        for (uint32_t index = 0;
             index < pendingOneShots &&
             runtime.oneShotVoices.size() < AudioSourceComponent::kMaxOneShotVoices;
             ++index) {
            const uint32_t voice = playVoice(false);
            if (voice != ISoundService::kInvalidVoiceHandle) {
                runtime.oneShotVoices.push_back(voice);
            }
        }
        if (runtime.voice != ISoundService::kInvalidVoiceHandle &&
            !sound->IsPlaying(runtime.voice)) {
            runtime.voice = ISoundService::kInvalidVoiceHandle;
        }
        std::optional<DirectX::XMFLOAT3> sourcePosition;
        if (source->spatial) {
            DirectX::XMFLOAT4X4 matrix{};
            if (world_.TryGetWorldMatrix(runtime.entity, matrix)) {
                sourcePosition = {matrix._41, matrix._42, matrix._43};
            }
        }
        const auto updateVoice = [&](uint32_t voice) {
            sound->SetVoiceVolume(voice, source->volume);
            sound->SetVoiceFrequencyRatio(voice, source->pitch);
            if (sourcePosition) {
                sound->SetVoicePosition(voice, *sourcePosition);
                sound->SetVoice3DRange(voice, source->minDistance, source->maxDistance);
            }
        };
        if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
            updateVoice(runtime.voice);
        }
        for (const uint32_t voice : runtime.oneShotVoices) {
            updateVoice(voice);
        }
        source->runtimePlaying = runtime.voice != ISoundService::kInvalidVoiceHandle ||
                                 !runtime.oneShotVoices.empty();
    }
}

void EditorScene::PauseRuntimeAudio(bool paused) {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        return;
    }
    for (const RuntimeAudioSource& runtime : runtimeAudioSources_) {
        const auto setPaused = [&](uint32_t voice) {
            if (paused) {
                sound->Pause(voice);
            } else {
                sound->Resume(voice);
            }
        };
        if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
            setPaused(runtime.voice);
        }
        for (const uint32_t voice : runtime.oneShotVoices) {
            setPaused(voice);
        }
    }
}

void EditorScene::EndRuntimeAudio() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound != nullptr) {
        for (const RuntimeAudioSource& runtime : runtimeAudioSources_) {
            if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
                sound->Stop(runtime.voice);
            }
            for (const uint32_t voice : runtime.oneShotVoices) {
                sound->Stop(voice);
            }
        }
    }
    for (RuntimeAudioSource& runtime : runtimeAudioSources_) {
        if (WorldEntity* entity = world_.Find(runtime.entity);
            entity != nullptr && entity->audioSource) {
            entity->audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
            entity->audioSource->pendingOneShots = 0u;
            entity->audioSource->runtimePlaying = false;
        }
    }
    runtimeAudioSources_.clear();
}

void EditorScene::BuildEditorOverlayScene() {
    editorOverlayScene_.BeginFrame();
    if (!showSceneGrid_ || !IsValidResourceId(sceneGridPipelineId_) || ctx_ == nullptr ||
        ctx_->rendering.model == nullptr) {
        return;
    }
    ModelManager* models = ctx_->rendering.model;
    const ModelHandle planeHandle =
        primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)];
    const Model* plane = planeHandle.IsValid() ? models->GetModel(planeHandle) : nullptr;
    if (plane == nullptr || !IsValidResourceId(plane->meshId)) {
        return;
    }

    RenderMeshItem grid{};
    grid.mesh = &models->GetMesh(plane->meshId);
    grid.material.color = {1.0f, 1.0f, 1.0f, 0.45f};
    grid.material.enableTexture = 0;
    grid.material.blendMode = static_cast<int32_t>(BlendMode::Transparent);
    grid.material.cullMode = static_cast<int32_t>(MaterialCullMode::None);
    grid.material.depthWrite = 0;
    grid.transform.scale = {100.0f, 100.0f, 1.0f};
    DirectX::XMStoreFloat4(
        &grid.transform.rotation,
        DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
                                         -DirectX::XM_PIDIV2));
    grid.pipelineId = sceneGridPipelineId_;
    grid.flags = RenderObjectFlags::Transparent;
    editorOverlayScene_.SubmitMesh(grid);
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
        if (!entity.camera && !entity.light && !entity.audioSource && !entity.audioListener &&
            !entity.boxCollider && !entity.characterController) {
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
        if (!entity.camera && !entity.light && !entity.audioSource && !entity.audioListener &&
            !entity.boxCollider && !entity.characterController) {
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
        ImU32 color = entity.camera
                          ? IM_COL32(90, 185, 255, 230)
                          : (entity.light
                                 ? IM_COL32(255, 215, 80, 230)
                                 : (entity.audioSource
                                        ? IM_COL32(190, 120, 255, 230)
                                        : (entity.audioListener
                                               ? IM_COL32(80, 215, 230, 230)
                                               : IM_COL32(80, 230, 130, 230))));
        const bool physicsLayerVisible =
            (physicsDebugLayerMask_ & (uint32_t{1} << entity.layer)) != 0u;
        const bool entityActive = world_.IsActiveInHierarchy(entity.id);
        const bool drawPhysicsShapes = showPhysicsDebug_ && entityActive &&
                                       physicsLayerVisible &&
                                       (entity.boxCollider || entity.characterController);
        if (active) {
            color = IM_COL32(255, 184, 56, 255);
        } else if (selected) {
            color = IM_COL32(90, 190, 255, 255);
        }
        const bool enabled = entityActive &&
                             ((entity.camera && entity.camera->enabled) ||
                               (entity.light && entity.light->enabled) ||
                               (entity.audioSource && entity.audioSource->enabled) ||
                               (entity.audioListener && entity.audioListener->enabled) ||
                               (entity.boxCollider && entity.boxCollider->enabled) ||
                               (entity.characterController &&
                                entity.characterController->enabled));
        if (!enabled) {
            color = (color & 0x00FFFFFFu) | (100u << 24u);
        }

        if (entity.camera) {
            drawList->AddRect({center.x - 8.0f, center.y - 6.0f},
                              {center.x + 5.0f, center.y + 6.0f}, color, 2.0f, 0, 1.8f);
            drawList->AddTriangle({center.x + 5.0f, center.y - 5.0f},
                                  {center.x + 12.0f, center.y - 9.0f},
                                  {center.x + 12.0f, center.y + 1.0f}, color, 1.8f);
        } else if (entity.light) {
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
        } else if (entity.audioSource) {
            drawList->AddRect({center.x - 9.0f, center.y - 4.0f},
                              {center.x - 5.0f, center.y + 4.0f}, color, 1.0f, 0, 1.8f);
            drawList->AddTriangle({center.x - 5.0f, center.y - 4.0f},
                                  {center.x + 1.0f, center.y - 8.0f},
                                  {center.x + 1.0f, center.y + 8.0f}, color, 1.8f);
            constexpr int arcSegments = 6;
            for (int arc = 0; arc < 2; ++arc) {
                const float radius = 5.0f + static_cast<float>(arc) * 4.0f;
                ImVec2 previous{};
                for (int index = 0; index <= arcSegments; ++index) {
                    const float angle = -DirectX::XM_PIDIV4 + DirectX::XM_PIDIV2 *
                                                               static_cast<float>(index) /
                                                               static_cast<float>(arcSegments);
                    const ImVec2 point{center.x + 1.0f + std::cos(angle) * radius,
                                       center.y + std::sin(angle) * radius};
                    if (index > 0) {
                        drawList->AddLine(previous, point, color, 1.5f);
                    }
                    previous = point;
                }
            }
        } else if (entity.audioListener) {
            drawList->AddCircle(center, 3.0f, color, 16, 1.8f);
            drawList->AddCircle(center, 7.0f, color, 24, 1.5f);
            drawList->AddCircle(center, 11.0f, color, 32, 1.25f);
        } else {
            drawList->AddRect({center.x - 6.0f, center.y - 6.0f},
                              {center.x + 6.0f, center.y + 6.0f}, color, 1.0f, 0,
                              1.8f);
        }
        if (active || drawPhysicsShapes) {
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

            if (active && entity.camera) {
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

            if (active && entity.light) {
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

            if (active && entity.audioSource && entity.audioSource->spatial) {
                const AudioSourceComponent& source = *entity.audioSource;
                const bool sourceEnabled = entityActive && source.enabled;
                const ImU32 minColor = sourceEnabled ? IM_COL32(220, 155, 255, 220)
                                                     : IM_COL32(220, 155, 255, 80);
                const ImU32 maxColor = sourceEnabled ? IM_COL32(155, 95, 255, 150)
                                                     : IM_COL32(155, 95, 255, 60);
                auto drawRangeCircle = [&](float radius, XMVECTOR axisA, XMVECTOR axisB,
                                           ImU32 guideColor, float thickness) {
                    constexpr int segments = 48;
                    XMFLOAT3 previous{};
                    for (int index = 0; index <= segments; ++index) {
                        const float angle = XM_2PI * static_cast<float>(index) /
                                            static_cast<float>(segments);
                        XMFLOAT3 point{};
                        XMStoreFloat3(&point, origin + axisA * (std::cos(angle) * radius) +
                                                 axisB * (std::sin(angle) * radius));
                        if (index > 0) {
                            drawWorldLine(previous, point, guideColor, thickness);
                        }
                        previous = point;
                    }
                };
                drawRangeCircle(source.minDistance, right, up, minColor, 1.75f);
                drawRangeCircle(source.minDistance, right, forward, minColor, 1.75f);
                drawRangeCircle(source.minDistance, up, forward, minColor, 1.75f);
                drawRangeCircle(source.maxDistance, right, up, maxColor, 1.25f);
                drawRangeCircle(source.maxDistance, right, forward, maxColor, 1.25f);
                drawRangeCircle(source.maxDistance, up, forward, maxColor, 1.25f);
            }

            if (entity.boxCollider && (active || drawPhysicsShapes)) {
                OBB collider{};
                if (TryBuildWorldBoxCollider(world_, entity.id, collider)) {
                    const XMVECTOR colliderCenter = XMLoadFloat3(&collider.center);
                    const XMVECTOR colliderRotation = XMLoadFloat4(&collider.rotation);
                    const XMVECTOR colliderRight = XMVector3Rotate(g_XMIdentityR0,
                                                                    colliderRotation);
                    const XMVECTOR colliderUp = XMVector3Rotate(g_XMIdentityR1,
                                                                 colliderRotation);
                    const XMVECTOR colliderForward = XMVector3Rotate(g_XMIdentityR2,
                                                                      colliderRotation);
                    const float halfX = collider.size.x * 0.5f;
                    const float halfY = collider.size.y * 0.5f;
                    const float halfZ = collider.size.z * 0.5f;
                    std::array<XMFLOAT3, 8> corners{};
                    size_t cornerIndex = 0;
                    for (int z = -1; z <= 1; z += 2) {
                        for (int y = -1; y <= 1; y += 2) {
                            for (int x = -1; x <= 1; x += 2) {
                                XMStoreFloat3(
                                    &corners[cornerIndex++],
                                    colliderCenter + colliderRight * (halfX * x) +
                                        colliderUp * (halfY * y) +
                                        colliderForward * (halfZ * z));
                            }
                        }
                    }
                    constexpr size_t colliderEdges[][2] = {
                        {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                        {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
                    };
                    const ImU32 guideColor = entity.boxCollider->isTrigger
                                                 ? (entity.boxCollider->enabled
                                                        ? IM_COL32(255, 170, 70, 220)
                                                        : IM_COL32(255, 170, 70, 80))
                                             : (!active && showPhysicsDebug_)
                                                 ? PhysicsDebugLayerColor(
                                                       entity.layer,
                                                       entity.boxCollider->enabled)
                                                 : (entity.boxCollider->enabled
                                                        ? IM_COL32(80, 230, 130, 210)
                                                        : IM_COL32(80, 230, 130, 80));
                    for (const auto& edge : colliderEdges) {
                        drawWorldLine(corners[edge[0]], corners[edge[1]], guideColor,
                                      1.5f);
                    }
                }
            }

            if (entity.characterController && (active || drawPhysicsShapes)) {
                CharacterCapsule capsule{};
                if (TryBuildWorldCharacterCapsule(world_, entity.id, capsule)) {
                    const ImU32 guideColor =
                        !active && showPhysicsDebug_
                            ? PhysicsDebugLayerColor(entity.layer,
                                                     entity.characterController->enabled)
                            : (entity.characterController->enabled
                                   ? IM_COL32(70, 220, 210, 220)
                                   : IM_COL32(70, 220, 210, 80));
                    const float segmentHalfHeight =
                        (std::max)(0.0f, capsule.height * 0.5f - capsule.radius);
                    auto capsulePoint = [&](float x, float y, float z) {
                        return XMFLOAT3{capsule.center.x + x, capsule.center.y + y,
                                       capsule.center.z + z};
                    };
                    constexpr int segments = 32;
                    for (float y : {-segmentHalfHeight, segmentHalfHeight}) {
                        XMFLOAT3 previous = capsulePoint(capsule.radius, y, 0.0f);
                        for (int index = 1; index <= segments; ++index) {
                            const float angle = XM_2PI * static_cast<float>(index) /
                                                static_cast<float>(segments);
                            const XMFLOAT3 point =
                                capsulePoint(std::cos(angle) * capsule.radius, y,
                                             std::sin(angle) * capsule.radius);
                            drawWorldLine(previous, point, guideColor, 1.5f);
                            previous = point;
                        }
                    }
                    drawWorldLine(capsulePoint(capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(capsule.radius, segmentHalfHeight, 0.0f),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(-capsule.radius, -segmentHalfHeight, 0.0f),
                                  capsulePoint(-capsule.radius, segmentHalfHeight, 0.0f),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, capsule.radius),
                                  guideColor, 1.5f);
                    drawWorldLine(capsulePoint(0.0f, -segmentHalfHeight, -capsule.radius),
                                  capsulePoint(0.0f, segmentHalfHeight, -capsule.radius),
                                  guideColor, 1.5f);
                    auto drawCapArc = [&](bool xPlane, bool top) {
                        const float baseY = top ? segmentHalfHeight : -segmentHalfHeight;
                        const float angleStart = top ? 0.0f : XM_PI;
                        XMFLOAT3 previous = xPlane
                                                ? capsulePoint(capsule.radius, baseY, 0.0f)
                                                : capsulePoint(0.0f, baseY, capsule.radius);
                        if (!top) {
                            previous = xPlane
                                           ? capsulePoint(-capsule.radius, baseY, 0.0f)
                                           : capsulePoint(0.0f, baseY, -capsule.radius);
                        }
                        for (int index = 1; index <= segments; ++index) {
                            const float angle = angleStart + XM_PI *
                                static_cast<float>(index) / static_cast<float>(segments);
                            const float horizontal = std::cos(angle) * capsule.radius;
                            const float vertical = std::sin(angle) * capsule.radius;
                            const XMFLOAT3 point =
                                xPlane ? capsulePoint(horizontal, baseY + vertical, 0.0f)
                                       : capsulePoint(0.0f, baseY + vertical, horizontal);
                            drawWorldLine(previous, point, guideColor, 1.5f);
                            previous = point;
                        }
                    };
                    drawCapArc(true, true);
                    drawCapArc(true, false);
                    drawCapArc(false, true);
                    drawCapArc(false, false);
                }
            }
        }
        if (drawPhysicsShapes) {
            std::string layerLabel = physicsSettings_.layerNames[entity.layer];
            if (layerLabel.empty()) {
                layerLabel = "Layer " + std::to_string(entity.layer);
            }
            const ImVec2 labelPosition{center.x + 10.0f, center.y + 8.0f};
            drawList->AddText({labelPosition.x + 1.0f, labelPosition.y + 1.0f},
                              IM_COL32(0, 0, 0, 220), layerLabel.c_str());
            drawList->AddText(labelPosition, PhysicsDebugLayerColor(entity.layer),
                              layerLabel.c_str());
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
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.meshRenderer ||
            !entity.meshRenderer->enabled) {
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
    ImGui::BeginDisabled(IsInPlayMode());
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
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &showSceneGrid_);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Physics", &showPhysicsDebug_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show BoxColliders and Character Controllers in Scene View.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!showPhysicsDebug_);
    if (ImGui::SmallButton("Layers##PhysicsDebugLayers")) {
        ImGui::OpenPopup("Physics Debug Layers");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Physics Debug Layers")) {
        ImGui::TextUnformatted("Visible Physics Layers");
        if (ImGui::SmallButton("All")) {
            physicsDebugLayerMask_ = 0xffffffffu;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) {
            physicsDebugLayerMask_ = 0u;
        }
        ImGui::Separator();
        for (size_t layer = 0u; layer < PhysicsSettings::kLayerCount; ++layer) {
            if (physicsSettings_.layerNames[layer].empty()) {
                continue;
            }
            ImGui::PushID(static_cast<int>(layer));
            const auto color = ImGui::ColorConvertU32ToFloat4(
                PhysicsDebugLayerColor(static_cast<uint8_t>(layer)));
            ImGui::TextColored(color, "\u25a0");
            ImGui::SameLine();
            bool visible = (physicsDebugLayerMask_ & (uint32_t{1} << layer)) != 0u;
            const std::string label = std::to_string(layer) + ": " +
                                      physicsSettings_.layerNames[layer];
            if (ImGui::Checkbox(label.c_str(), &visible)) {
                const uint32_t layerBit = uint32_t{1} << layer;
                if (visible) {
                    physicsDebugLayerMask_ |= layerBit;
                } else {
                    physicsDebugLayerMask_ &= ~layerBit;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(IsInPlayMode());
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
    ImGui::EndDisabled();
}

bool EditorScene::DrawBoxColliderGizmo(const ImVec2& imageMin,
                                       const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->boxCollider || boxColliderGizmoEntity_ != selection_) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
        }
        boxColliderGizmoMode_ = BoxColliderGizmoMode::None;
        boxColliderGizmoEntity_ = {};
        return false;
    }

    DirectX::XMFLOAT4X4 storedEntityWorld{};
    OBB worldCollider{};
    if (!world_.TryGetWorldMatrix(selection_, storedEntityWorld) ||
        !TryBuildWorldBoxCollider(world_, selection_, worldCollider)) {
        return false;
    }

    using namespace DirectX;
    const XMMATRIX entityWorld = XMLoadFloat4x4(&storedEntityWorld);
    XMVECTOR entityScale{};
    XMVECTOR entityRotation{};
    XMVECTOR entityTranslation{};
    if (!XMMatrixDecompose(&entityScale, &entityRotation, &entityTranslation, entityWorld)) {
        return false;
    }

    XMFLOAT4X4 gizmoMatrix{};
    const XMVECTOR colliderCenter = XMLoadFloat3(&worldCollider.center);
    const XMVECTOR colliderRotation = XMLoadFloat4(&worldCollider.rotation);
    if (boxColliderGizmoMode_ == BoxColliderGizmoMode::Center) {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixAffineTransformation(XMVectorReplicate(1.0f),
                                                     XMVectorZero(), colliderRotation,
                                                     colliderCenter));
    } else {
        XMStoreFloat4x4(&gizmoMatrix,
                        XMMatrixScaling(worldCollider.size.x, worldCollider.size.y,
                                        worldCollider.size.z) *
                            XMMatrixRotationQuaternion(colliderRotation) *
                            XMMatrixTranslationFromVector(colliderCenter));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    const ImGuizmo::OPERATION operation =
        boxColliderGizmoMode_ == BoxColliderGizmoMode::Center ? ImGuizmo::TRANSLATE
                                                              : ImGuizmo::SCALE;
    float snapValues[3]{};
    std::ranges::fill(snapValues, boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                                       ? translationSnap_
                                       : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, ImGuizmo::LOCAL, &gizmoMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        BeginHistoryEdit(boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                             ? "Modify BoxCollider Center"
                             : "Modify BoxCollider Size");
    }

    if (manipulated) {
        BoxColliderComponent& collider = *entity->boxCollider;
        if (boxColliderGizmoMode_ == BoxColliderGizmoMode::Center) {
            XMVECTOR determinant{};
            const XMMATRIX inverseEntity = XMMatrixInverse(&determinant, entityWorld);
            const float determinantValue = XMVectorGetX(determinant);
            if (std::isfinite(determinantValue) && std::abs(determinantValue) > 1.0e-8f) {
                XMVECTOR scale{};
                XMVECTOR rotation{};
                XMVECTOR translation{};
                if (XMMatrixDecompose(&scale, &rotation, &translation,
                                      XMLoadFloat4x4(&gizmoMatrix))) {
                    XMStoreFloat3(&collider.center,
                                  XMVector3TransformCoord(translation, inverseEntity));
                    RefreshDirty();
                }
            }
        } else {
            XMVECTOR manipulatedScale{};
            XMVECTOR rotation{};
            XMVECTOR translation{};
            if (XMMatrixDecompose(&manipulatedScale, &rotation, &translation,
                                  XMLoadFloat4x4(&gizmoMatrix))) {
                XMFLOAT3 storedManipulatedScale{};
                XMFLOAT3 storedEntityScale{};
                XMStoreFloat3(&storedManipulatedScale, manipulatedScale);
                XMStoreFloat3(&storedEntityScale, entityScale);
                const auto localSize = [](float worldSize, float worldScale) {
                    constexpr float minimumSize = 0.001f;
                    constexpr float minimumScale = 1.0e-6f;
                    return (std::max)(minimumSize,
                                      std::abs(worldSize) /
                                          (std::max)(minimumScale, std::abs(worldScale)));
                };
                collider.size = {localSize(storedManipulatedScale.x, storedEntityScale.x),
                                 localSize(storedManipulatedScale.y, storedEntityScale.y),
                                 localSize(storedManipulatedScale.z, storedEntityScale.z)};
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        status_ = boxColliderGizmoMode_ == BoxColliderGizmoMode::Center
                      ? "Modified BoxCollider center from Scene View."
                      : "Modified BoxCollider size from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
}

bool EditorScene::DrawCharacterControllerGizmo(const ImVec2& imageMin,
                                               const ImVec2& imageMax) {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->characterController ||
        characterControllerGizmoEntity_ != selection_) {
        if (gizmoWasUsing_) {
            CommitHistoryEdit();
            gizmoWasUsing_ = false;
        }
        characterControllerGizmoMode_ = CharacterControllerGizmoMode::None;
        characterControllerGizmoEntity_ = {};
        return false;
    }

    DirectX::XMFLOAT4X4 storedEntityWorld{};
    CharacterCapsule worldCapsule{};
    if (!world_.TryGetWorldMatrix(selection_, storedEntityWorld) ||
        !TryBuildWorldCharacterCapsule(world_, selection_, worldCapsule)) {
        return false;
    }

    using namespace DirectX;
    const XMMATRIX entityWorld = XMLoadFloat4x4(&storedEntityWorld);
    XMVECTOR entityScale{};
    XMVECTOR entityRotation{};
    XMVECTOR entityTranslation{};
    if (!XMMatrixDecompose(&entityScale, &entityRotation, &entityTranslation, entityWorld)) {
        return false;
    }
    XMFLOAT3 storedEntityScale{};
    XMStoreFloat3(&storedEntityScale, entityScale);

    const float worldDiameter = worldCapsule.radius * 2.0f;
    XMFLOAT4X4 gizmoMatrix{};
    if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
        XMStoreFloat4x4(
            &gizmoMatrix,
            XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                worldCapsule.center.z));
    } else {
        XMStoreFloat4x4(
            &gizmoMatrix,
            XMMatrixScaling(worldDiameter, worldCapsule.height, worldDiameter) *
                XMMatrixTranslation(worldCapsule.center.x, worldCapsule.center.y,
                                    worldCapsule.center.z));
    }

    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                      imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
        operation = ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z;
    } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Height) {
        operation = ImGuizmo::SCALE_Y;
    }
    float snapValues[3]{};
    std::ranges::fill(
        snapValues,
        characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center
            ? translationSnap_
            : scaleSnap_);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    const bool manipulated = ImGuizmo::Manipulate(
        &view._11, &projection._11, operation, ImGuizmo::WORLD, &gizmoMatrix._11, nullptr,
        snapActive ? snapValues : nullptr);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        const char* label = "Modify CharacterController Center";
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Radius) {
            label = "Modify CharacterController Radius";
        } else if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Height) {
            label = "Modify CharacterController Height";
        }
        BeginHistoryEdit(label);
    }

    if (manipulated) {
        CharacterControllerComponent& controller = *entity->characterController;
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
            XMVECTOR determinant{};
            const XMMATRIX inverseEntity = XMMatrixInverse(&determinant, entityWorld);
            const float determinantValue = XMVectorGetX(determinant);
            if (std::isfinite(determinantValue) && std::abs(determinantValue) > 1.0e-8f) {
                XMVECTOR scale{};
                XMVECTOR rotation{};
                XMVECTOR translation{};
                if (XMMatrixDecompose(&scale, &rotation, &translation,
                                      XMLoadFloat4x4(&gizmoMatrix))) {
                    XMStoreFloat3(&controller.center,
                                  XMVector3TransformCoord(translation, inverseEntity));
                    RefreshDirty();
                }
            }
        } else {
            XMVECTOR manipulatedScale{};
            XMVECTOR rotation{};
            XMVECTOR translation{};
            if (XMMatrixDecompose(&manipulatedScale, &rotation, &translation,
                                  XMLoadFloat4x4(&gizmoMatrix))) {
                XMFLOAT3 storedManipulatedScale{};
                XMStoreFloat3(&storedManipulatedScale, manipulatedScale);
                constexpr float minimumScale = 1.0e-6f;
                if (characterControllerGizmoMode_ ==
                    CharacterControllerGizmoMode::Radius) {
                    const float changedX =
                        std::abs(std::abs(storedManipulatedScale.x) - worldDiameter);
                    const float changedZ =
                        std::abs(std::abs(storedManipulatedScale.z) - worldDiameter);
                    const float newWorldDiameter =
                        changedX >= changedZ ? std::abs(storedManipulatedScale.x)
                                             : std::abs(storedManipulatedScale.z);
                    const float radialScale =
                        (std::max)(minimumScale,
                                   (std::max)(std::abs(storedEntityScale.x),
                                              std::abs(storedEntityScale.z)));
                    controller.radius =
                        (std::max)(0.001f, newWorldDiameter * 0.5f / radialScale);
                    controller.height =
                        (std::max)(controller.height, controller.radius * 2.0f);
                    controller.skinWidth =
                        (std::min)(controller.skinWidth,
                                   (std::max)(0.0f, controller.radius - 0.001f));
                } else {
                    const float verticalScale =
                        (std::max)(minimumScale, std::abs(storedEntityScale.y));
                    controller.height =
                        (std::max)(controller.radius * 2.0f,
                                   std::abs(storedManipulatedScale.y) / verticalScale);
                    controller.stepOffset =
                        (std::min)(controller.stepOffset, controller.height);
                }
                RefreshDirty();
            }
        }
    }

    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        if (characterControllerGizmoMode_ == CharacterControllerGizmoMode::Center) {
            status_ = "Modified CharacterController center from Scene View.";
        } else if (characterControllerGizmoMode_ ==
                   CharacterControllerGizmoMode::Radius) {
            status_ = "Modified CharacterController radius from Scene View.";
        } else {
            status_ = "Modified CharacterController height from Scene View.";
        }
    }
    gizmoWasUsing_ = usingNow;
    return ImGuizmo::IsOver() || usingNow;
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
                TransformComponent localTransform = transformed->transform;
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
    if (IsInPlayMode()) {
        if (action == PendingSceneAction::Exit) {
            StopPlayMode();
        } else {
            status_ = "Stop Play Mode before changing scenes.";
            return;
        }
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
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before creating a scene.";
        return;
    }
    world_.Clear();
    world_.SetPhysicsSettings(physicsSettings_);
    const EntityId camera = world_.CreateEntity("Main Camera");
    if (WorldEntity* cameraEntity = world_.Find(camera)) {
        cameraEntity->transform.position = {0.0f, 2.0f, -5.0f};
        cameraEntity->camera = CameraComponent{};
        cameraEntity->camera->primary = true;
        cameraEntity->audioListener = AudioListenerComponent{};
    }
    const EntityId light = world_.CreateEntity("Directional Light");
    if (WorldEntity* lightEntity = world_.Find(light)) {
        lightEntity->transform.rotationDegrees = {50.0f, -30.0f, 0.0f};
        lightEntity->light = LightComponent{};
    }
    selection_ = world_.CreateEntity("Cube");
    if (WorldEntity* cube = world_.Find(selection_)) {
        cube->meshRenderer = MeshRendererComponent{};
        cube->materialOverride = MaterialOverrideComponent{};
    }
    if (clearPath) {
        scenePath_.clear();
    }
    ClearHistory(false);
    status_ = "Created a new scene.";
}

bool EditorScene::SaveScene() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before saving the scene.";
        return false;
    }
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
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before saving the scene.";
        return false;
    }
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
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before loading a scene.";
        return false;
    }
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
    world_.SetPhysicsSettings(physicsSettings_);
    scenePath_ = path;
    selection_ = world_.Empty() ? EntityId{} : world_.Entities().front().id;
    dirty_ = false;
    ClearHistory(true);
    AddRecentScene(scenePath_);
    std::string behaviorRequirementError;
    if (ctx_ != nullptr &&
        !ValidateWorldBehaviorRequirements(&behaviorRequirementError)) {
        status_ = "Warning: Loaded scene with an invalid Behavior: " +
                  behaviorRequirementError;
    } else {
        status_ = "Loaded scene: " + scenePath_.string();
    }
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

std::optional<std::filesystem::path> EditorScene::ShowSavePrefabDialog(
    std::string_view entityName) const {
    std::wstring filename = L"Prefab";
    if (!entityName.empty()) {
        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, entityName.data(),
            static_cast<int>(entityName.size()), nullptr, 0);
        if (length > 0) {
            filename.resize(static_cast<size_t>(length));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entityName.data(),
                                static_cast<int>(entityName.size()), filename.data(), length);
        }
    }
    for (wchar_t& character : filename) {
        if (character < L' ' || wcschr(L"\\/:*?\"<>|", character) != nullptr) {
            character = L'_';
        }
    }
    while (!filename.empty() && (filename.back() == L' ' || filename.back() == L'.')) {
        filename.pop_back();
    }
    if (filename.empty()) {
        filename = L"Prefab";
    }
    filename += L".likeprefab";

    std::array<wchar_t, 32768> buffer{};
    wcsncpy_s(buffer.data(), buffer.size(), filename.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"LikeEngine Prefab (*.likeprefab)\0*.likeprefab\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    const std::filesystem::path initialPath = assetRoot_ / currentAssetDirectory_;
    const std::wstring initialDirectory = initialPath.wstring();
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"likeprefab";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT;
    if (!GetSaveFileNameW(&dialog)) {
        return std::nullopt;
    }
    const std::filesystem::path selected(buffer.data());
    return IsPrefabAsset(selected) && IsPathWithinRoot(assetRoot_, selected)
               ? std::optional<std::filesystem::path>(selected)
               : std::nullopt;
}

std::vector<std::filesystem::path> EditorScene::ShowImportAssetDialog() const {
    std::array<wchar_t, 32768> buffer{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter =
        L"Model, Texture, and Audio Assets\0"
        L"*.fbx;*.obj;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.bin;*.mtl;*.png;*.jpg;*.jpeg;"
        L"*.tga;*.bmp;*.dds;*.hdr;*.exr;*.wav;*.mp3;*.aac;*.m4a;*.wma\0";
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
