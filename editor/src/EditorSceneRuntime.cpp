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

struct RuntimeUiGroupState {
    float alpha = 1.0f;
    bool interactable = true;
    bool blocksRaycasts = true;
};

RuntimeUiGroupState GetUiGroupState(const World& world,
                                    const WorldEntity& entity) {
    RuntimeUiGroupState state{};
    const WorldEntity* current = &entity;
    while (current != nullptr) {
        if (current->canvasGroup && current->canvasGroup->enabled) {
            const CanvasGroupComponent& group = *current->canvasGroup;
            state.alpha *= group.alpha;
            state.interactable = state.interactable && group.interactable;
            state.blocksRaycasts =
                state.blocksRaycasts && group.blocksRaycasts;
        }
        current = current->parent.IsValid() ? world.Find(current->parent) : nullptr;
    }
    return state;
}
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
        model->lockRootAnimationPosition = animator.lockRootPosition;
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
    std::vector<InputFieldEvent> inputFieldEvents =
        std::exchange(pendingInputFieldEvents_, {});
    for (const InputFieldEvent& event :
         inputFieldEvents) {
        WorldEntity* entity = world_.Find(event.entity);
        if (entity == nullptr || !entity->inputField ||
            !entity->inputField->enabled ||
            !entity->inputField->interactable ||
            !entity->button || !entity->button->enabled ||
            !entity->button->interactable ||
            !GetUiGroupState(world_, *entity).interactable ||
            !world_.IsActiveInHierarchy(event.entity)) {
            continue;
        }
        if (event.submitted) {
            runtimeBehaviors_.DispatchInputFieldSubmit(
                event.entity, event.text);
        } else {
            runtimeBehaviors_.DispatchInputFieldValueChanged(
                event.entity, event.text);
        }
        if (ApplyPendingRuntimeSceneLoad()) {
            return;
        }
    }
    std::vector<DropdownValueChange> dropdownChanges =
        std::exchange(pendingDropdownValueChanges_, {});
    for (const DropdownValueChange& change :
         dropdownChanges) {
        WorldEntity* entity = world_.Find(change.entity);
        if (entity == nullptr || !entity->dropdown ||
            !entity->dropdown->enabled ||
            !entity->dropdown->interactable ||
            !entity->button || !entity->button->enabled ||
            !entity->button->interactable ||
            !GetUiGroupState(world_, *entity).interactable ||
            !world_.IsActiveInHierarchy(change.entity)) {
            continue;
        }
        runtimeBehaviors_.DispatchDropdownValueChanged(
            change.entity, change.value);
        if (ApplyPendingRuntimeSceneLoad()) {
            return;
        }
    }
    std::vector<SliderValueChange> sliderChanges =
        std::exchange(pendingSliderValueChanges_, {});
    for (const SliderValueChange& change : sliderChanges) {
        WorldEntity* entity = world_.Find(change.entity);
        if (entity == nullptr || !entity->slider ||
            !entity->slider->enabled ||
            !entity->slider->interactable ||
            !GetUiGroupState(world_, *entity).interactable ||
            !world_.IsActiveInHierarchy(change.entity)) {
            continue;
        }
        runtimeBehaviors_.DispatchSliderValueChanged(
            change.entity, change.value);
        if (ApplyPendingRuntimeSceneLoad()) {
            return;
        }
    }
    std::vector<EntityId> buttonClicks = std::exchange(pendingButtonClicks_, {});
    for (const EntityId entityId : buttonClicks) {
        WorldEntity* entity = world_.Find(entityId);
        if (entity == nullptr || !entity->button || !entity->button->enabled ||
            !entity->button->interactable ||
            !GetUiGroupState(world_, *entity).interactable ||
            (entity->toggle && !entity->toggle->enabled) ||
            !world_.IsActiveInHierarchy(entityId)) {
            continue;
        }
        if (entity->toggle) {
            entity->toggle->isOn = !entity->toggle->isOn;
            runtimeBehaviors_.DispatchToggleValueChanged(
                entityId, entity->toggle->isOn);
            if (ApplyPendingRuntimeSceneLoad()) {
                return;
            }
        }
        runtimeBehaviors_.DispatchButtonClick(entityId);
        if (ApplyPendingRuntimeSceneLoad()) {
            return;
        }
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

