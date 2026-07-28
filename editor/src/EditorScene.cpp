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
