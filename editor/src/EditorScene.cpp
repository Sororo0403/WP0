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
#include "imgui.h"
#include "imgui/ImguiManager.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/MeshRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"

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

using namespace EditorSceneViewportUtils;

namespace {

constexpr const char* kPrimitiveNames[] = {"Box", "Sphere", "Plane", "Cylinder"};
constexpr const char* kEntityDragPayload = "EDITOR_ENTITY";
constexpr const char* kModelAssetDragPayload = "EDITOR_MODEL_ASSET";
constexpr const char* kTextureAssetDragPayload = "EDITOR_TEXTURE_ASSET";
constexpr const char* kAudioAssetDragPayload = "EDITOR_AUDIO_ASSET";
constexpr const char* kFontAssetDragPayload = "EDITOR_FONT_ASSET";
constexpr const char* kScriptAssetDragPayload = "EDITOR_SCRIPT_ASSET";
constexpr const char* kPrefabAssetDragPayload = "EDITOR_PREFAB_ASSET";
constexpr size_t kMaxHistoryEntries = 128;
constexpr size_t kMaxRecentScenes = 10;
constexpr float kRuntimeStepDeltaTime = 1.0f / 60.0f;

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
    return std::ranges::any_of(path,
                               [](const std::filesystem::path& part) { return part == L".."; });
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
    return !error && !relative.empty() && !relative.is_absolute() && !HasParentTraversal(relative);
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

bool IsPathAtOrWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
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
    if (filename.empty() || filename == "." || filename == ".." || filename.ends_with('.') ||
        filename.ends_with(' ')) {
        return false;
    }
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    return std::ranges::none_of(filename, [invalidCharacters](unsigned char character) {
        return character < 32u ||
               invalidCharacters.find(static_cast<char>(character)) != std::string_view::npos;
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

bool AssetPathMatches(const std::filesystem::path& candidate, const std::filesystem::path& target,
                      bool directory) {
    const std::string candidateText = candidate.lexically_normal().generic_string();
    const std::string targetText = target.lexically_normal().generic_string();
    return candidateText == targetText ||
           (directory && candidateText.starts_with(targetText + '/'));
}

bool IsPrefabAsset(const std::filesystem::path& path) {
    return LowercaseAscii(path.extension().string()) == ".likeprefab";
}

} // namespace

EditorScene::EditorScene(std::filesystem::path projectRoot, std::filesystem::path assetRoot,
                         std::filesystem::path sceneRoot, std::filesystem::path startupScene,
                         std::filesystem::path recentScenesPath,
                         std::filesystem::path imguiSettingsPath,
                         std::function<void()> requestClose, bool playerMode)
    : requestClose_(std::move(requestClose)), playerMode_(playerMode),
      projectRoot_(std::move(projectRoot)), assetRoot_(std::move(assetRoot)),
      sceneRoot_(std::move(sceneRoot)), startupScenePath_(startupScene),
      imguiSettingsPath_(std::move(imguiSettingsPath)),
      playerSettingsStore_(projectRoot_ / L"settings" / L"player.json"),
      physicsSettingsStore_(projectRoot_ / L"settings" / L"physics.json"),
      inputSettingsStore_(projectRoot_ / L"settings" / L"input.json"),
      recentScenesStore_(std::move(recentScenesPath), sceneRoot_), scenePath_(startupScene),
      runtimeScenePath_(std::move(startupScene)) {
    if (playerMode_) {
        showHierarchyPanel_ = false;
        showProjectPanel_ = false;
        showScenePanel_ = false;
        showConsolePanel_ = false;
        showInspectorPanel_ = false;
    }
    std::string playerSettingsError;
    const bool playerSettingsLoaded =
        playerSettingsStore_.Load(playerSettings_, playerSettingsError);
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
    } else if (!playerSettingsLoaded) {
        status_ = "Warning: Could not load Player Settings: " + playerSettingsError;
    }
}

void EditorScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
    std::string inputSettingsError;
    if (ctx.systems.input == nullptr ||
        !inputSettingsStore_.Load(*ctx.systems.input, inputSettingsError)) {
        status_ = "Warning: Could not load Input Settings: " +
                  (inputSettingsError.empty() ? std::string("Input service is unavailable.")
                                              : inputSettingsError);
    }
    std::string behaviorRequirementError;
    std::string scriptModuleError;
    if ((!playerMode_ && !ScriptBuildService::BuildIfNeeded(projectRoot_, scriptModuleError)) ||
        !projectScripts_.Load(projectRoot_, ctx.systems.input, behaviorRegistry_,
                              scriptModuleError)) {
        status_ = "Error: " + scriptModuleError;
    } else if (!ValidateWorldBehaviorRequirements(&behaviorRequirementError)) {
        status_ = "Error: Scene contains an invalid Behavior: " + behaviorRequirementError;
    } else {
        const size_t upgradedReferences = UpgradeInputActionReferences();
        if (upgradedReferences != 0u) {
            status_ = "Upgraded " + std::to_string(upgradedReferences) +
                      " Input Action reference(s) to stable IDs.";
        }
    }
    if (ctx.systems.imgui == nullptr || !ctx.systems.imgui->ConfigureDocking(imguiSettingsPath_)) {
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
    sceneGridPipelineId_ =
        ctx.rendering.meshRenderer->CreatePipeline(ShaderPaths::MeshVS, ShaderPaths::EditorGridPS);
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
    if (playerMode_) {
        EnterPlayMode();
    }
}

void EditorScene::Update() {
    if (!playerMode_) {
        UpdateScriptCompilation();
    }
    if (playModeState_ == PlayModeState::Playing && ctx_ != nullptr) {
        UpdateRuntimeWorld(ctx_->frame.deltaTime);
    } else if (playModeState_ == PlayModeState::Edit && ctx_ != nullptr) {
        UpdateEditAnimatorPreview(ctx_->frame.deltaTime);
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
    if (!gamePostProcessInitializationAttempted_ && gameViewSurface_.IsReady() && ctx_ != nullptr &&
        ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
        !ctx_->rendering.dxCommon->IsCommandListRecording()) {
        gamePostProcessInitializationAttempted_ = true;
        gameViewPostProcess_.Initialize(ctx_->rendering.dxCommon, ctx_->rendering.srv,
                                        gameViewSurface_.GetWidth(), gameViewSurface_.GetHeight());
        if (!gameViewPostProcess_.IsReady()) {
            status_ = "Game View PostProcess initialization failed.";
        }
    }
    if (!cameraPreviewPostProcessInitializationAttempted_ && cameraPreviewSurface_.IsReady() &&
        ctx_ != nullptr && ctx_->rendering.dxCommon != nullptr && ctx_->rendering.srv != nullptr &&
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
        if (!world_.IsActiveInHierarchy(entity.id) || !entity.light || !entity.light->enabled ||
            entity.light->intensity <= 0.0f) {
            continue;
        }
        DirectX::XMFLOAT4X4 storedWorld{};
        if (!world_.TryGetWorldMatrix(entity.id, storedWorld)) {
            continue;
        }
        const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&storedWorld);
        DirectX::XMVECTOR direction =
            DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), world);
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
            spot.angleParams = {std::cos(DirectX::XMConvertToRadians(component.innerAngleDegrees)),
                                std::cos(DirectX::XMConvertToRadians(component.outerAngleDegrees)),
                                2.4f, 1.0f};
            spotAssigned = true;
        }
    }
    lightingScene.SetSceneLighting(lighting);
}

void EditorScene::Draw() {}

void EditorScene::DrawPostProcessOverlay() {
    ImGuizmo::BeginFrame();
    CaptureConsoleStatus();
    if (playerMode_) {
        if (gameInputCaptured_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ReleaseGameInputCapture();
        }
        DrawPanels();
        return;
    }
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
    if (playerSettingsDirty_ && !SavePlayerSettings()) {
        return false;
    }
    if (inputSettingsDirty_ && !SaveInputSettings()) {
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
    if (playerMode_) {
        return;
    }
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

bool EditorScene::LaunchPlayerPreview() {
    if (IsInPlayMode()) {
        status_ = "Stop Play Mode before running the Player Preview.";
        return false;
    }
    if (dirty_) {
        status_ = "Save the scene before running the Player Preview.";
        return false;
    }
    if (playerSettingsDirty_ || physicsSettingsDirty_ || inputSettingsDirty_) {
        status_ = "Save Project Settings before running the Player Preview.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before running the Player Preview.";
        return false;
    }
    ProjectDescriptor project;
    std::string validationError;
    if (!ProjectDescriptor::Load(projectRoot_, project, validationError) ||
        !PlayerProjectValidator::Validate(project, validationError)) {
        status_ = "Could not run Player Preview: " + validationError;
        return false;
    }
    std::array<wchar_t, 32768> executableBuffer{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executableBuffer.data(),
                                                      static_cast<DWORD>(executableBuffer.size()));
    if (executableLength == 0u || executableLength >= executableBuffer.size()) {
        status_ = "Could not locate the Editor executable.";
        return false;
    }
    const std::filesystem::path executable(std::wstring(executableBuffer.data(), executableLength));
    std::wstring command =
        L"\"" + executable.wstring() + L"\" --player --project \"" + projectRoot_.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0u,
                        nullptr, projectRoot_.c_str(), &startup, &process)) {
        status_ = "Could not launch the Player Preview.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    status_ = "Launched Player Preview.";
    return true;
}

bool EditorScene::BuildPlayerPackage(std::filesystem::path* destination) {
    if (IsInPlayMode() || dirty_ || playerSettingsDirty_ || physicsSettingsDirty_ ||
        inputSettingsDirty_) {
        status_ = "Save the scene and Project Settings before building.";
        return false;
    }
    if (scriptBuildInProgress_ || scriptBuildPending_) {
        status_ = "Wait for Project Script compilation before building.";
        return false;
    }
    ProjectDescriptor project;
    std::string error;
    if (!ProjectDescriptor::Load(projectRoot_, project, error) ||
        !PlayerProjectValidator::Validate(project, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    std::array<wchar_t, 32768> executableBuffer{};
    const DWORD executableLength = GetModuleFileNameW(nullptr, executableBuffer.data(),
                                                      static_cast<DWORD>(executableBuffer.size()));
    if (executableLength == 0u || executableLength >= executableBuffer.size()) {
        status_ = "Could not locate the Player executable.";
        return false;
    }
#ifdef _DEBUG
    constexpr char configuration[] = "Debug";
    constexpr wchar_t outputName[] = L"windows-x64-debug";
#else
    constexpr char configuration[] = "Release";
    constexpr wchar_t outputName[] = L"windows-x64";
#endif
    const PlayerPackageRequest request{
        .executable =
            std::filesystem::path(std::wstring(executableBuffer.data(), executableLength)),
        .projectRoot = project.root,
        .manifest = project.manifestPath,
        .assetRoot = project.assetRoot,
        .sceneRoot = project.sceneRoot,
        .destination = project.root / L"build" / outputName,
        .configuration = configuration,
    };
    if (!PlayerPackageBuilder::Build(request, error)) {
        status_ = "Could not build Player: " + error;
        return false;
    }
    if (destination != nullptr) {
        *destination = request.destination;
    }
    status_ = "Built Player package: " + request.destination.generic_string();
    if (!error.empty()) {
        status_ += " Warning: " + error;
    }
    return true;
}

bool EditorScene::LaunchPackagedPlayer(const std::filesystem::path& package) {
    const std::filesystem::path executable = package / L"Game.exe";
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(executable, filesystemError) || filesystemError) {
        status_ = "Could not run Player: Game.exe was not found in the package.";
        return false;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0u,
                        nullptr, package.c_str(), &startup, &process)) {
        status_ = "Could not run the packaged Player.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    status_ = "Built and launched Player package: " + package.generic_string();
    return true;
}

bool EditorScene::BuildAndRunPlayerPackage() {
    std::filesystem::path package;
    return BuildPlayerPackage(&package) && LaunchPackagedPlayer(package);
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
                std::filesystem::path label = std::filesystem::relative(path, sceneRoot_, error);
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
    if (ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build Project", nullptr, false, !IsInPlayMode())) {
            BuildPlayerPackage();
        }
        if (ImGui::MenuItem("Build And Run", "F9", false, !IsInPlayMode())) {
            BuildAndRunPlayerPackage();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Run Project", "F8", false, !IsInPlayMode())) {
            LaunchPlayerPreview();
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
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, editing && !entityClipboard_.empty())) {
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
                  static_cast<unsigned long long>(runtimeFrameCount_), runtimeElapsedSeconds_);
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
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
    if (!ImGui::BeginPopupModal("Rename Entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
    const bool submitted =
        ImGui::InputText("##EntityName", renameBuffer_.data(), renameBuffer_.size(),
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
    const std::string_view scriptType = behaviorRegistry_.TypeFromSourceAsset(assetPath);
    if (scriptType.empty()) {
        status_ = "C++ Script source is not registered by the Project Script module: " + assetPath;
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
    RecordImmediateEdit(scriptIndex ? "Replace Script" : "Add Script", before, selectionBefore);
    status_ = std::string(scriptIndex ? "Replaced" : "Added") + " Script component: " + assetPath;
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

void EditorScene::AssignBaseColorTexture(EntityId entityId, const std::filesystem::path& path) {
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
    const std::string previousPath =
        entity->materialOverride ? entity->materialOverride->baseColorTexturePath : std::string{};
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

void EditorScene::AssignImageTexture(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !entity->image) {
        status_ = "The target Image component no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeTextureAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->image->texturePath;
    entity->image->texturePath = assetPath;
    loadedTextures_.erase(previousPath);
    loadedTextures_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Image Texture", before, selectionBefore);
    status_ = "Assigned Image texture: " + assetPath;
}

void EditorScene::AssignTextFont(EntityId entityId, const std::filesystem::path& path) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !entity->text) {
        status_ = "The target Text component no longer exists.";
        return;
    }
    std::string assetPath;
    if (!TryNormalizeFontAssetReference(path, assetPath)) {
        return;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->text->fontPath = assetPath;
    loadedFonts_.erase(assetPath);
    selection_ = entityId;
    RecordImmediateEdit("Assign Text Font", before, selectionBefore);
    status_ = "Assigned Text font: " + assetPath;
}

void EditorScene::AssignNormalTexture(EntityId entityId, const std::filesystem::path& path) {
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
    const std::string previousPath =
        entity->materialOverride ? entity->materialOverride->normalTexturePath : std::string{};
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

void EditorScene::AssignRoughnessTexture(EntityId entityId, const std::filesystem::path& path) {
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

void EditorScene::AssignMetallicTexture(EntityId entityId, const std::filesystem::path& path) {
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

void EditorScene::HandleSceneCameraControls(const ImVec2& imageMin, const ImVec2& imageMax,
                                            bool imageHovered) {
    ImGuiIO& io = ImGui::GetIO();
    if (imageHovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FocusSceneCameraOnSelection();
    }
    const bool beginLook = imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool beginPan = imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
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

    const int cursorCenterX = static_cast<int>(std::lround((imageMin.x + imageMax.x) * 0.5f));
    const int cursorCenterY = static_cast<int>(std::lround((imageMin.y + imageMax.y) * 0.5f));
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
    if (sceneCameraNavigating_) {
        constexpr float mouseSensitivity = 0.004f;
        rotation.x = std::clamp(rotation.x + pointerDeltaY * mouseSensitivity,
                                -DirectX::XM_PIDIV2 + 0.01f, DirectX::XM_PIDIV2 - 0.01f);
        rotation.y += pointerDeltaX * mouseSensitivity;
        if (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f) {
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

    const float movementLengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(movement));
    DirectX::XMVECTOR position = DirectX::XMLoadFloat3(&sceneViewCamera_.GetPosition());
    bool positionChanged = false;
    if (movementLengthSquared > 0.0f) {
        const float deltaTime = std::clamp(io.DeltaTime, 0.0f, 0.1f);
        const float speed = io.KeyShift ? 12.0f : 4.0f;
        movement = DirectX::XMVectorScale(DirectX::XMVector3Normalize(movement), speed * deltaTime);
        position = DirectX::XMVectorAdd(position, movement);
        positionChanged = true;
    }
    if (sceneCameraPanning_ && (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f)) {
        constexpr float panSensitivity = 0.01f;
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(right, -pointerDeltaX * panSensitivity));
        position = DirectX::XMVectorAdd(position,
                                        DirectX::XMVectorScale(up, pointerDeltaY * panSensitivity));
        positionChanged = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (imageHovered && io.MouseWheel != 0.0f) {
        position =
            DirectX::XMVectorAdd(position, DirectX::XMVectorScale(forward, io.MouseWheel * 0.75f));
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
            localCenter = {(boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f,
                           (boundsMin.z + boundsMax.z) * 0.5f};
            const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
            const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
            const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
            radius = (std::max)(0.1f, std::sqrt(extentX * extentX + extentY * extentY +
                                                extentZ * extentZ));
        }
    }

    const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&worldMatrix);
    const DirectX::XMVECTOR center =
        DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&localCenter), world);
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
    sceneViewCamera_.SetClipRange(0.01f, (std::max)(1000.0f, distance + radius * 4.0f));
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
        !TryDecomposeTransformComponent(XMLoadFloat4x4(&currentWorld), currentWorldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }

    const XMFLOAT3 position = sceneViewCamera_.GetPosition();
    const XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    XMMATRIX local = XMMatrixScaling(currentWorldTransform.scale.x, currentWorldTransform.scale.y,
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
        const XMMATRIX inverseParent = XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
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
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix), worldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }
    sceneViewCamera_.SetPosition(worldTransform.position);
    sceneViewCamera_.SetRotation({DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
                                  DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
                                  DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    status_ = "Moved the Scene View to " + entity->name + ".";
    return true;
}

void EditorScene::HandleSceneContextMenu(const ImVec2& imageMin, const ImVec2& imageMax,
                                         bool imageHovered) {
    const bool rightClick = sceneCameraPointerTravel_ <= 3.0f;
    if (imageHovered && rightClick && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        sceneContextCreatePosition_ =
            CalculateScenePlacementPosition(sceneViewCamera_, imageMin, imageMax,
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
    if (!physicalPath || !AssetImport::BuildPlan({*physicalPath}, importPlan, importError)) {
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
    const std::optional<std::filesystem::path> destination = ShowSavePrefabDialog(rootEntity->name);
    if (!destination) {
        status_ = "Prefab save cancelled.";
        return false;
    }

    std::unordered_set<EntityId, EntityIdHash> includedIds;
    includedIds.insert(roots.front());
    for (const WorldEntity& candidate : world_.Entities()) {
        EntityId current = candidate.parent;
        for (size_t depth = 0u; current.IsValid() && depth <= world_.Entities().size(); ++depth) {
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
                if (property.type == ScriptPropertyType::Entity && property.entityValue.IsValid() &&
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

bool EditorScene::TryNormalizeFontAssetReference(const std::filesystem::path& path,
                                                 std::string& assetPath) {
    if (!AssetImport::IsFontFile(path)) {
        status_ = "The dropped font asset is invalid.";
        return false;
    }
    const std::optional<std::filesystem::path> resolvedPath = ResolveProjectAssetPath(path);
    std::error_code error;
    if (!resolvedPath || !std::filesystem::is_regular_file(*resolvedPath, error) || error) {
        status_ = "The dropped font asset no longer exists.";
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    assetPath = normalized.generic_string();
    if (normalized.begin() != normalized.end() && *normalized.begin() == "assets") {
        assetPath = "asset://" + normalized.lexically_relative("assets").generic_string();
    }
    if (assetPath.size() > 1024u) {
        status_ = "The dropped font asset path is too long.";
        return false;
    }
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

bool EditorScene::InstantiatePrefabAsset(const std::filesystem::path& path, EntityId parent,
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
    if (!world_.InstantiateEntityHierarchies(prefab, parent, roots, &error) || roots.empty()) {
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
    fontAssets_.clear();
    scriptAssets_.clear();
    prefabAssets_.clear();
    sceneAssets_.clear();
    assetBrowserEntries_.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator sceneIterator(
        sceneRoot_, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator sceneEnd;
    while (!error && sceneIterator != sceneEnd) {
        if (sceneIterator->is_regular_file(error) && !error &&
            LowercaseAscii(sceneIterator->path().extension().string()) == ".likescene") {
            std::filesystem::path relative =
                std::filesystem::relative(sceneIterator->path(), sceneRoot_, error);
            if (!error) {
                sceneAssets_.push_back(relative.lexically_normal());
            }
        }
        sceneIterator.increment(error);
    }
    std::ranges::sort(sceneAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    error.clear();
    if (!std::filesystem::is_directory(assetRoot_, error) || error) {
        return;
    }

    std::filesystem::path currentDirectory =
        (assetRoot_ / currentAssetDirectory_).lexically_normal();
    if ((!currentAssetDirectory_.empty() && !IsPathWithinRoot(assetRoot_, currentDirectory)) ||
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
                    AssetImport::IsFontFile(entry.path()) || IsPrefabAsset(entry.path()) ||
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
    std::ranges::sort(
        assetBrowserEntries_, [](const AssetBrowserEntry& left, const AssetBrowserEntry& right) {
            if (left.directory != right.directory) {
                return left.directory;
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
             AssetImport::IsFontFile(iterator->path()) || IsPrefabAsset(iterator->path()) ||
             ScriptAssets::IsScriptFile(iterator->path()))) {
            std::filesystem::path relative =
                std::filesystem::relative(iterator->path(), assetRoot_, error);
            if (!error) {
                auto& assets = IsPrefabAsset(iterator->path())                ? prefabAssets_
                               : AssetImport::IsTextureFile(iterator->path()) ? textureAssets_
                               : AssetImport::IsAudioFile(iterator->path())   ? audioAssets_
                               : AssetImport::IsFontFile(iterator->path())    ? fontAssets_
                               : ScriptAssets::IsScriptFile(iterator->path()) ? scriptAssets_
                                                                              : modelAssets_;
                assets.push_back((std::filesystem::path("assets") / relative).lexically_normal());
            }
        }
        iterator.increment(error);
    }
    std::ranges::sort(modelAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    std::ranges::sort(textureAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    std::ranges::sort(audioAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    std::ranges::sort(fontAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    std::ranges::sort(scriptAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
    std::ranges::sort(prefabAssets_, {},
                      [](const std::filesystem::path& path) { return path.generic_string(); });
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

std::optional<std::filesystem::path> EditorScene::ResolveProjectAssetPath(
    const std::filesystem::path& path) const {
    const std::filesystem::path resolved = AssetManager::ResolvePathStrict(path);
    return resolved.empty() ? std::nullopt : std::optional<std::filesystem::path>(resolved);
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
    undoHistory_.push_back({std::move(pending.label), std::move(pending.before), std::move(after)});
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
    undoHistory_.push_back(
        {std::move(label), {std::move(before), selectionBefore}, std::move(after)});
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
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr || ctx_->rendering.texture == nullptr) {
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
        if (!entity.image || entity.image->texturePath.empty() ||
            loadedTextures_.contains(entity.image->texturePath)) {
            continue;
        }
        const std::optional<std::filesystem::path> resolved =
            ResolveProjectAssetPath(entity.image->texturePath);
        if (!resolved || !AssetImport::IsTextureFile(*resolved)) {
            continue;
        }
        loadedTextures_.emplace(
            entity.image->texturePath,
            TextureHandle(ctx_->rendering.texture->LoadSrgb(resolved->wstring())));
    }
    if (ctx_->rendering.font != nullptr) {
        for (const WorldEntity& entity : world_.Entities()) {
            if (!entity.text || entity.text->fontPath.empty() ||
                loadedFonts_.contains(entity.text->fontPath)) {
                continue;
            }
            const std::optional<std::filesystem::path> resolved =
                ResolveProjectAssetPath(entity.text->fontPath);
            if (!resolved || !AssetImport::IsFontFile(*resolved)) {
                continue;
            }
            loadedFonts_.emplace(entity.text->fontPath,
                                 ctx_->rendering.font->LoadFont(resolved->wstring()));
        }
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
            const std::optional<std::filesystem::path> resolved = ResolveProjectAssetPath(*path);
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

TextureHandle EditorScene::ResolveNormalTexture(const MaterialOverrideComponent& component) const {
    return ResolveLinearTexture(component.normalTexturePath);
}

TextureHandle EditorScene::ResolveLinearTexture(const std::string& path) const {
    const auto found = loadedLinearTextures_.find(path);
    return found != loadedLinearTextures_.end() ? found->second : TextureHandle{};
}

bool EditorScene::UpdateGameViewCamera() {
    const WorldEntity* primaryCamera = nullptr;
    for (const WorldEntity& entity : world_.Entities()) {
        if (world_.IsActiveInHierarchy(entity.id) && entity.camera && entity.camera->enabled &&
            entity.camera->primary) {
            primaryCamera = &entity;
            break;
        }
    }
    if (primaryCamera == nullptr) {
        return false;
    }

    return UpdateCameraFromEntity(primaryCamera->id, gameViewCamera_, gameViewSurface_.GetWidth(),
                                  gameViewSurface_.GetHeight());
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
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix), worldTransform)) {
        return false;
    }
    targetCamera.SetPosition(worldTransform.position);
    targetCamera.SetRotation({DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
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

bool EditorScene::DrawSelectedCameraPreview(const ImVec2& imageMin, const ImVec2& imageMax) {
    const WorldEntity* entity = world_.Find(selection_);
    ImVec2 previewMin{};
    ImVec2 previewMax{};
    if (entity == nullptr || !world_.IsActiveInHierarchy(selection_) || !entity->camera ||
        !cameraPreviewSurface_.IsReady() || !cameraPreviewPostProcess_.IsReady() ||
        ctx_ == nullptr || ctx_->rendering.dxCommon == nullptr ||
        ctx_->rendering.model == nullptr ||
        !TryGetCameraPreviewRect(imageMin, imageMax, previewMin, previewMax) ||
        !UpdateCameraFromEntity(entity->id, cameraPreviewCamera_, cameraPreviewSurface_.GetWidth(),
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
    drawList->AddRectFilled(
        previewMin,
        {previewMin.x + (std::min)(previewMax.x - previewMin.x, textSize.x + 12.0f),
         previewMin.y + textSize.y + 8.0f},
        IM_COL32(18, 22, 30, 220), 3.0f, ImDrawFlags_RoundCornersTopLeft);
    drawList->AddText({previewMin.x + 6.0f, previewMin.y + 4.0f}, IM_COL32(240, 242, 248, 255),
                      label.c_str());
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
    const float radius =
        (std::max)(0.05f, std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ));
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
            !entity.meshRenderer->enabled || !entity.materialOverride ||
            !entity.materialOverride->enabled) {
            continue;
        }
        const auto runtimeAnimator =
            std::ranges::find_if(runtimeAnimators_, [&entity](const RuntimeAnimator& runtime) {
                return runtime.entity == entity.id;
            });
        const bool runtimeAnimated = runtimeAnimator != runtimeAnimators_.end();
        const bool editPreviewAnimated = !runtimeAnimated &&
                                         editAnimatorPreviewEntity_ == entity.id &&
                                         editAnimatorPreviewModel_.IsValid();
        const bool animated = runtimeAnimated || editPreviewAnimated;
        const ModelHandle handle = runtimeAnimated       ? runtimeAnimator->model
                                   : editPreviewAnimated ? editAnimatorPreviewModel_
                                                         : ResolveModel(*entity.meshRenderer);
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
                const TextureHandle normalTexture = ResolveNormalTexture(*entity.materialOverride);
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

void EditorScene::BuildEditorOverlayScene() {
    editorOverlayScene_.BeginFrame();
    if (!showSceneGrid_ || !IsValidResourceId(sceneGridPipelineId_) || ctx_ == nullptr ||
        ctx_->rendering.model == nullptr) {
        return;
    }
    ModelManager* models = ctx_->rendering.model;
    const ModelHandle planeHandle = primitiveModels_[static_cast<size_t>(MeshPrimitive::Plane)];
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
    DirectX::XMStoreFloat4(&grid.transform.rotation,
                           DirectX::XMQuaternionRotationAxis(
                               DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), -DirectX::XM_PIDIV2));
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
        if (selection_ == closestComponent && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
