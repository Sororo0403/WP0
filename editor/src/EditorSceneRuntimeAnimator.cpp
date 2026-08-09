#include "EditorScene.h"

#include "model/Model.h"
#include "model/ModelManager.h"

#include <algorithm>
#include <ranges>

namespace {
void SetFirstAnimatorError(std::string* error, const std::string& message) {
    if (error != nullptr && error->empty()) {
        *error = message;
    }
}
} // namespace

bool EditorScene::BeginRuntimeAnimators(std::string* error) {
    EndRuntimeAnimators();
    const bool hasEnabledAnimator =
        std::ranges::any_of(world_.Entities(), [](const WorldEntity& entity) {
            return entity.animator && entity.animator->enabled;
        });
    if (!hasEnabledAnimator) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        SetFirstAnimatorError(error, "Model service is unavailable.");
        return false;
    }
    bool valid = true;
    for (const WorldEntity& entity : world_.Entities()) {
        if (entity.animator && entity.animator->enabled) {
            valid = StartRuntimeAnimator(entity, *models, error) && valid;
        }
    }
    if (valid && error != nullptr) {
        error->clear();
    }
    return valid;
}

bool EditorScene::StartRuntimeAnimator(const WorldEntity& entity, ModelManager& models,
                                       std::string* error) {
    if (!entity.meshRenderer || entity.meshRenderer->sourceType != MeshSourceType::Model ||
        entity.meshRenderer->modelPath.empty()) {
        SetFirstAnimatorError(error, entity.name + ": Animator requires a Model MeshRenderer.");
        return false;
    }
    const ModelHandle handle = ResolveRuntimeAnimatorModel(entity, models);
    Model* model = handle.IsValid() ? models.GetModel(handle) : nullptr;
    if (model == nullptr || model->animations.empty()) {
        SetFirstAnimatorError(error, entity.name + ": Animator model has no animation clips.");
        return false;
    }
    const std::string cacheKey =
        entity.id.ToString() + "|" + entity.meshRenderer->modelPath;
    animatorModels_.insert_or_assign(cacheKey, handle);
    const AnimatorComponent& animator = *entity.animator;
    const std::string clip = animator.clip.empty() ? model->animations.begin()->first
                                                   : animator.clip;
    if (!model->animations.contains(clip)) {
        SetFirstAnimatorError(error,
                              entity.name + ": Animator clip was not found: " + clip);
        return false;
    }
    model->lockRootAnimationPosition = animator.lockRootPosition;
    models.PlayAnimation(handle, clip, animator.loop);
    if (!animator.playOnAwake) {
        model->isPlaying = false;
        models.UpdateAnimation(handle, 0.0f);
    }
    InitializeRuntimeAnimatorState(entity.id, animator, clip, *model);
    runtimeAnimators_.push_back({entity.id, handle});
    return true;
}

ModelHandle EditorScene::ResolveRuntimeAnimatorModel(const WorldEntity& entity,
                                                     ModelManager& models) {
    const std::string& modelPath = entity.meshRenderer->modelPath;
    const std::string cacheKey = entity.id.ToString() + "|" + modelPath;
    if (const auto cached = animatorModels_.find(cacheKey); cached != animatorModels_.end()) {
        return cached->second;
    }
    const std::optional<std::filesystem::path> path = ResolveProjectAssetPath(modelPath);
    return path ? models.LoadUniqueHandle(path->wstring()) : ModelHandle{};
}

void EditorScene::InitializeRuntimeAnimatorState(EntityId entityId,
                                                 const AnimatorComponent& animator,
                                                 const std::string& clip,
                                                 const Model& model) {
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !entity->animator) {
        return;
    }
    AnimatorComponent& runtime = *entity->animator;
    runtime.runtimeCommand = AnimatorComponent::RuntimeCommand::None;
    runtime.runtimeRequestedClip.clear();
    runtime.runtimeClip = clip;
    runtime.runtimeLoop = animator.loop;
    runtime.runtimeFadeDuration = 0.0f;
    runtime.runtimePlaying = model.isPlaying;
    runtime.runtimeFinished = model.animationFinished;
    runtime.runtimeTime = model.animationTime;
    runtime.runtimeDuration = model.animations.at(clip).duration;
    runtime.runtimeNormalizedTime = 0.0f;
    runtime.runtimeTransitioning = false;
    runtime.runtimeTransitionProgress = 0.0f;
}

void EditorScene::UpdateRuntimeAnimators(float deltaTime) {
    ModelManager* models = ctx_ != nullptr ? ctx_->rendering.model : nullptr;
    if (models == nullptr) {
        return;
    }
    for (const RuntimeAnimator& runtime : runtimeAnimators_) {
        UpdateRuntimeAnimator(runtime, *models, deltaTime);
    }
}

void EditorScene::UpdateRuntimeAnimator(const RuntimeAnimator& runtime, ModelManager& models,
                                        float deltaTime) {
    WorldEntity* entity = world_.Find(runtime.entity);
    if (entity == nullptr || !entity->animator) {
        return;
    }
    AnimatorComponent& animator = *entity->animator;
    Model* model = models.GetModel(runtime.model);
    if (model == nullptr) {
        ClearRuntimeAnimatorPlaybackState(animator);
        return;
    }
    ApplyRuntimeAnimatorCommand(runtime, animator, *model, models);
    const bool active = animator.enabled && world_.IsActiveInHierarchy(runtime.entity);
    if (active && model->isPlaying) {
        models.UpdateAnimation(runtime.model, deltaTime * animator.speed);
    }
    SyncRuntimeAnimatorState(runtime.entity, animator, *model);
}

void EditorScene::ApplyRuntimeAnimatorCommand(const RuntimeAnimator& runtime,
                                              AnimatorComponent& animator, Model& model,
                                              ModelManager& models) {
    const AnimatorComponent::RuntimeCommand command = animator.runtimeCommand;
    animator.runtimeCommand = AnimatorComponent::RuntimeCommand::None;
    if (command == AnimatorComponent::RuntimeCommand::Stop) {
        const std::string clip = model.currentAnimation;
        if (!clip.empty() && model.animations.contains(clip)) {
            models.PlayAnimation(runtime.model, clip, model.isLoop);
            model.isPlaying = false;
            model.animationFinished = false;
            models.UpdateAnimation(runtime.model, 0.0f);
        }
        return;
    }
    if (!model.animations.contains(animator.runtimeRequestedClip)) {
        return;
    }
    if (command == AnimatorComponent::RuntimeCommand::CrossFade) {
        models.CrossFadeAnimation(runtime.model, animator.runtimeRequestedClip,
                                  animator.runtimeFadeDuration, animator.runtimeLoop);
    } else if (command == AnimatorComponent::RuntimeCommand::Play) {
        if (model.currentAnimation != animator.runtimeRequestedClip ||
            (!model.isPlaying && !model.animationFinished)) {
            models.PlayAnimation(runtime.model, animator.runtimeRequestedClip,
                                 animator.runtimeLoop);
        } else {
            model.isLoop = animator.runtimeLoop;
        }
    }
}

void EditorScene::SyncRuntimeAnimatorState(EntityId entityId, AnimatorComponent& animator,
                                           const Model& model) {
    animator.runtimePlaying = animator.enabled && world_.IsActiveInHierarchy(entityId) &&
                              model.isPlaying;
    animator.runtimeFinished = model.animationFinished;
    animator.runtimeClip = model.currentAnimation;
    animator.runtimeTime = model.animationTime;
    const auto currentClip = model.animations.find(model.currentAnimation);
    animator.runtimeDuration = currentClip != model.animations.end()
                                   ? (std::max)(currentClip->second.duration, 0.0f)
                                   : 0.0f;
    animator.runtimeNormalizedTime = animator.runtimeDuration > 0.0f
                                         ? std::clamp(animator.runtimeTime /
                                                          animator.runtimeDuration,
                                                      0.0f, 1.0f)
                                         : 0.0f;
    animator.runtimeTransitioning = !model.blendSourceAnimation.empty();
    animator.runtimeTransitionProgress =
        animator.runtimeTransitioning && model.blendDuration > 0.0f
            ? std::clamp(model.blendTime / model.blendDuration, 0.0f, 1.0f)
            : 0.0f;
}

void EditorScene::ClearRuntimeAnimatorPlaybackState(AnimatorComponent& animator) {
    animator.runtimeClip.clear();
    animator.runtimePlaying = false;
    animator.runtimeFinished = false;
    animator.runtimeTime = 0.0f;
    animator.runtimeDuration = 0.0f;
    animator.runtimeNormalizedTime = 0.0f;
    animator.runtimeTransitioning = false;
    animator.runtimeTransitionProgress = 0.0f;
}

void EditorScene::ResetRuntimeAnimatorState(AnimatorComponent& animator) {
    animator.runtimeCommand = AnimatorComponent::RuntimeCommand::None;
    animator.runtimeRequestedClip.clear();
    animator.runtimeLoop = true;
    animator.runtimeFadeDuration = 0.0f;
    ClearRuntimeAnimatorPlaybackState(animator);
}

void EditorScene::EndRuntimeAnimators() {
    for (const RuntimeAnimator& runtime : runtimeAnimators_) {
        if (WorldEntity* entity = world_.Find(runtime.entity); entity != nullptr &&
            entity->animator) {
            ResetRuntimeAnimatorState(*entity->animator);
        }
    }
    runtimeAnimators_.clear();
}
