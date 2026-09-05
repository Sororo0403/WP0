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


namespace {
constexpr float kRuntimeStepDeltaTime = 1.0f / 60.0f;
} // namespace

bool EditorScene::IsInPlayMode() const {
    return playModeState_ != PlayModeState::Edit;
}

void EditorScene::EnterPlayMode() {
    if (IsInPlayMode()) {
        return;
    }
    CommitHistoryEdit();
    StopAudioAssetPreview();
    EndEditAnimatorPreview();
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
    runtimeScenePath_ = scenePath_;
    world_.SetPhysicsSettings(physicsSettings_);
    std::string runtimeError;
    if (ctx_ != nullptr && ctx_->systems.input != nullptr) ctx_->systems.input->ResetGameInput();
    gameInputSuspended_ = false;
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
    ReleaseGameInputFocus();
    EndRuntimeWorld();
    if (ctx_ != nullptr && ctx_->systems.input != nullptr) ctx_->systems.input->ResetGameInput();
    world_ = std::move(*editModeWorld_);
    editModeWorld_.reset();
    runtimeScenePath_ = scenePath_;
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
        ReleaseGameInputFocus();
        PauseRuntimeAudio(true);
        playModeState_ = PlayModeState::Paused;
        status_ = "Paused Play Mode.";
    } else if (playModeState_ == PlayModeState::Paused) {
        PauseRuntimeAudio(false);
        gameInputSuspended_ = false;
        focusGamePanelRequested_ = true;
        playModeState_ = PlayModeState::Playing;
        status_ = "Resumed Play Mode.";
    }
}

void EditorScene::ReleaseGameInputFocus() {
    gameInputFocused_ = false;
    gamePointerInside_ = false;
    pressedButton_ = {};
    activeSlider_ = {};
    activeInputField_ = {};
    openDropdown_ = {};
    if (ctx_ != nullptr && ctx_->systems.input != nullptr) {
        ctx_->systems.input->RouteGameInput(false, false, {});
    }
    if (ctx_ != nullptr && ctx_->systems.winApp != nullptr) {
        ctx_->systems.winApp->SetCursorVisible(true);
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
}

void EditorScene::StepRuntimeWorld() {
    if (playModeState_ != PlayModeState::Paused) {
        return;
    }
    ReleaseGameInputFocus();
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

bool EditorScene::BeginEditAnimatorPreview(EntityId entityId) {
    if (IsInPlayMode()) {
        return false;
    }
    WorldEntity* entity = world_.Find(entityId);
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (entity == nullptr || !entity->animator || !entity->animator->enabled ||
        !entity->meshRenderer || entity->meshRenderer->sourceType != MeshSourceType::Model ||
        entity->meshRenderer->modelPath.empty() || models == nullptr) {
        return false;
    }
    const std::optional<std::filesystem::path> path =
        ResolveProjectAssetPath(entity->meshRenderer->modelPath);
    if (!path) {
        return false;
    }
    const std::string cacheKey =
        "edit-preview|" + entity->id.ToString() + "|" + entity->meshRenderer->modelPath;
    const auto cached = animatorModels_.find(cacheKey);
    const ModelHandle handle =
        cached != animatorModels_.end() ? cached->second
                                       : models->LoadUniqueHandle(path->wstring());
    Model* model = handle.IsValid() ? models->GetModel(handle) : nullptr;
    if (model == nullptr || model->animations.empty()) {
        return false;
    }
    animatorModels_.insert_or_assign(cacheKey, handle);
    std::string clip = entity->animator->clip;
    if (clip.empty()) {
        const auto first = std::ranges::min_element(
            model->animations, {}, [](const auto& entry) { return entry.first; });
        clip = first != model->animations.end() ? first->first : std::string{};
    }
    if (clip.empty() || !model->animations.contains(clip)) {
        return false;
    }
    EndEditAnimatorPreview();
    model->lockRootAnimationPosition = entity->animator->lockRootPosition;
    models->PlayAnimation(handle, clip, entity->animator->loop);
    models->UpdateAnimation(handle, 0.0f);
    editAnimatorPreviewEntity_ = entityId;
    editAnimatorPreviewModel_ = handle;
    editAnimatorPreviewModelPath_ = entity->meshRenderer->modelPath;
    return true;
}

void EditorScene::UpdateEditAnimatorPreview(float deltaTime) {
    if (!editAnimatorPreviewEntity_.IsValid()) {
        return;
    }
    WorldEntity* entity = world_.Find(editAnimatorPreviewEntity_);
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (entity == nullptr || selection_ != editAnimatorPreviewEntity_ || !entity->animator ||
        !entity->animator->enabled || !entity->meshRenderer ||
        entity->meshRenderer->sourceType != MeshSourceType::Model ||
        entity->meshRenderer->modelPath != editAnimatorPreviewModelPath_ || models == nullptr) {
        EndEditAnimatorPreview();
        return;
    }
    Model* model = editAnimatorPreviewModel_.IsValid()
                       ? models->GetModel(editAnimatorPreviewModel_)
                       : nullptr;
    if (model == nullptr) {
        EndEditAnimatorPreview();
        return;
    }
    model->lockRootAnimationPosition = entity->animator->lockRootPosition;
    if (model->isPlaying) {
        const float safeDeltaTime =
            std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 0.1f) : 0.0f;
        models->UpdateAnimation(editAnimatorPreviewModel_,
                                safeDeltaTime * entity->animator->speed);
    }
}

void EditorScene::EndEditAnimatorPreview() {
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (models != nullptr && editAnimatorPreviewModel_.IsValid()) {
        if (Model* model = models->GetModel(editAnimatorPreviewModel_); model != nullptr) {
            model->isPlaying = false;
        }
    }
    editAnimatorPreviewEntity_ = {};
    editAnimatorPreviewModel_ = {};
    editAnimatorPreviewModelPath_.clear();
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
    if (DispatchPendingRuntimeUiEvents()) {
        return;
    }
    runtimeBehaviors_.Update(safeDeltaTime);
    if (ApplyPendingRuntimeSceneLoad()) {
        return;
    }
    runtimeTriggers_.Update(world_, runtimeBehaviors_);
    if (ApplyPendingRuntimeSceneLoad()) {
        return;
    }
    UpdateRuntimeAnimators(safeDeltaTime);
    UpdateRuntimeAudio();
    ++runtimeFrameCount_;
    runtimeElapsedSeconds_ += static_cast<double>(safeDeltaTime);
}

bool EditorScene::ApplyPendingRuntimeSceneLoad() {
    const std::optional<std::string> request =
        world_.ConsumeSceneLoadRequest();
    if (!request) {
        return false;
    }
    World loaded;
    std::filesystem::path loadedPath;
    std::string error;
    if (!RuntimeSceneLoader::Load(sceneRoot_, *request, physicsSettings_,
                                  loaded, loadedPath, error)) {
        status_ = "Error: Could not load Runtime Scene: " + error;
        return false;
    }

    EndRuntimeWorld();
    world_ = std::move(loaded);
    runtimeScenePath_ = std::move(loadedPath);
    selection_ = {};
    hierarchySelection_.clear();
    hierarchySelectionAnchor_ = {};
    std::string runtimeError;
    const bool started = BeginRuntimeWorld(&runtimeError);
    status_ = started
                  ? "Loaded Runtime Scene: " +
                        runtimeScenePath_.generic_string()
                  : "Error: Loaded Runtime Scene with setup issue(s): " +
                        runtimeError;
    return true;
}

void EditorScene::EndRuntimeWorld() {
    if (ctx_ != nullptr && ctx_->systems.input != nullptr) {
        ctx_->systems.input->SetCursorMode(CursorMode::Free);
        ctx_->systems.input->SetCursorVisible(true);
    }
    EndRuntimeAnimators();
    EndRuntimeAudio();
    runtimeTriggers_.Clear();
    runtimeBehaviors_.Clear();
    focusedButton_ = {};
    pressedButton_ = {};
    activeSlider_ = {};
    openDropdown_ = {};
    dropdownHighlightedIndex_ = 0;
    activeInputField_ = {};
    runtimeInitialUiSelectionApplied_ = false;
    pendingButtonClicks_.clear();
    pendingSliderValueChanges_.clear();
    pendingDropdownValueChanges_.clear();
    pendingInputFieldEvents_.clear();
    buttonColorTransitions_.clear();
    runtimeFrameCount_ = 0;
    runtimeElapsedSeconds_ = 0.0;
}
