#include "EditorScene.h"

#include "imgui.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <ranges>
#include <utility>

bool EditorScene::DrawAnimatorHeader(WorldEntity* entity) {
    ImGui::SeparatorText("Animator");
    if (!ImGui::Button("Remove Animator")) {
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (editAnimatorPreviewEntity_ == entity->id) {
        EndEditAnimatorPreview();
    }
    entity->animator.reset();
    RecordImmediateEdit("Remove Animator", before, selectionBefore);
    status_ = "Removed Animator.";
    return true;
}

void EditorScene::DrawAnimatorRuntimeStatus(const AnimatorComponent& animator) const {
    if (!IsInPlayMode()) {
        ImGui::TextDisabled("Script API: PlayAnimation / CrossFadeAnimation / StopAnimation");
        return;
    }
    ImGui::TextDisabled("Runtime: %s%s", animator.runtimePlaying ? "Playing" : "Stopped",
                        animator.runtimeFinished ? " (Finished)" : "");
    if (!animator.runtimeClip.empty()) {
        ImGui::TextDisabled("Runtime Clip: %s", animator.runtimeClip.c_str());
    }
    char animationProgress[64]{};
    std::snprintf(animationProgress, std::size(animationProgress), "%.2f / %.2f s",
                  animator.runtimeTime, animator.runtimeDuration);
    ImGui::ProgressBar(animator.runtimeNormalizedTime, {-FLT_MIN, 0.0f}, animationProgress);
    if (animator.runtimeTransitioning) {
        ImGui::ProgressBar(animator.runtimeTransitionProgress, {-FLT_MIN, 0.0f}, "Cross Fade");
    }
}

const Model* EditorScene::ResolveAnimatorInspectorModel(const WorldEntity& entity) const {
    const ModelHandle handle =
        entity.meshRenderer ? ResolveModel(*entity.meshRenderer) : ModelHandle{};
    return handle.IsValid() && ctx_ != nullptr && ctx_->rendering.model != nullptr
               ? ctx_->rendering.model->GetModel(handle)
               : nullptr;
}

void EditorScene::DrawAnimatorClipSelection(const WorldEntity& entity,
                                            AnimatorComponent& animator, const Model* model,
                                            EntityId selectionBefore) {
    const char* clipLabel = animator.clip.empty() ? "First Clip" : animator.clip.c_str();
    if (ImGui::BeginCombo("Clip##Animator", clipLabel)) {
        if (ImGui::Selectable("First Clip", animator.clip.empty())) {
            std::string before = WorldSerializer::Serialize(world_);
            animator.clip.clear();
            RecordImmediateEdit("Change Animator Clip", std::move(before), selectionBefore);
            if (editAnimatorPreviewEntity_ == entity.id) {
                BeginEditAnimatorPreview(entity.id);
            }
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
                    std::string before = WorldSerializer::Serialize(world_);
                    animator.clip = clip;
                    RecordImmediateEdit("Change Animator Clip", std::move(before),
                                        selectionBefore);
                    if (editAnimatorPreviewEntity_ == entity.id) {
                        BeginEditAnimatorPreview(entity.id);
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (model == nullptr || model->animations.empty()) {
        ImGui::TextDisabled("Assign an animated Model to Mesh Renderer.");
    }
}

void EditorScene::DrawAnimatorPreviewControls(EntityId entity, const Model* model) {
    if (IsInPlayMode() || model == nullptr || model->animations.empty()) {
        return;
    }
    const bool previewing =
        editAnimatorPreviewEntity_ == entity && editAnimatorPreviewModel_.IsValid();
    Model* previewModel = previewing && ctx_ != nullptr && ctx_->rendering.model != nullptr
                              ? ctx_->rendering.model->GetModel(editAnimatorPreviewModel_)
                              : nullptr;
    const bool previewPlaying = previewModel != nullptr && previewModel->isPlaying;
    if (ImGui::Button(previewPlaying ? "Pause##AnimatorPreview" : "Play##AnimatorPreview")) {
        if (previewModel != nullptr && !previewPlaying && previewModel->animationFinished) {
            status_ = BeginEditAnimatorPreview(entity) ? "Restarted Animator preview."
                                                       : "Animator preview could not be started.";
        } else if (previewModel != nullptr && !previewPlaying) {
            previewModel->isPlaying = true;
            previewModel->animationFinished = false;
            status_ = "Resumed Animator preview.";
        } else if (previewPlaying) {
            previewModel->isPlaying = false;
            status_ = "Paused Animator preview.";
        } else if (BeginEditAnimatorPreview(entity)) {
            status_ = "Started Animator preview.";
        } else {
            status_ = "Animator preview could not be started.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart##AnimatorPreview")) {
        status_ = BeginEditAnimatorPreview(entity) ? "Restarted Animator preview."
                                                   : "Animator preview could not be started.";
    }
    if (previewModel == nullptr) {
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop##AnimatorPreview")) {
        EndEditAnimatorPreview();
        status_ = "Stopped Animator preview.";
    }
    const auto clip = previewModel->animations.find(previewModel->currentAnimation);
    const float duration =
        clip != previewModel->animations.end() ? (std::max)(0.0f, clip->second.duration) : 0.0f;
    float time = std::clamp(previewModel->animationTime, 0.0f, duration);
    if (ImGui::SliderFloat("Time##AnimatorPreview", &time, 0.0f, duration, "%.2f s")) {
        previewModel->animationTime = time;
        previewModel->isPlaying = false;
        previewModel->animationFinished = duration > 0.0f && time >= duration;
        ctx_->rendering.model->UpdateAnimation(editAnimatorPreviewModel_, 0.0f);
        status_ = "Scrubbed Animator preview.";
    }
}

void EditorScene::DrawAnimatorCheckbox(const char* label, bool& value, const char* historyLabel,
                                       EntityId selectionBefore) {
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox(label, &value)) {
        RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
    }
}

void EditorScene::SynchronizeAnimatorPreviewSettings(EntityId entity,
                                                     const AnimatorComponent& animator) {
    if (editAnimatorPreviewEntity_ != entity || !editAnimatorPreviewModel_.IsValid() ||
        ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        return;
    }
    if (Model* previewModel = ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
        previewModel != nullptr) {
        previewModel->isLoop = animator.loop;
    }
}

void EditorScene::DrawAnimatorPlaybackSettings(const WorldEntity& entity,
                                               AnimatorComponent& animator,
                                               EntityId selectionBefore) {
    DrawAnimatorCheckbox("Play On Awake##Animator", animator.playOnAwake,
                         "Toggle Animator Play On Awake", selectionBefore);
    DrawAnimatorCheckbox("Loop##Animator", animator.loop, "Toggle Animator Loop", selectionBefore);
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Lock Root Position##Animator", &animator.lockRootPosition)) {
        RecordImmediateEdit("Toggle Animator Root Position Lock", std::move(before),
                            selectionBefore);
        status_ = animator.lockRootPosition ? "Locked Animator root position."
                                            : "Unlocked Animator root position.";
        if (editAnimatorPreviewEntity_ == entity.id && editAnimatorPreviewModel_.IsValid() &&
            ctx_ != nullptr && ctx_->rendering.model != nullptr) {
            if (Model* previewModel =
                    ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
                previewModel != nullptr) {
                previewModel->lockRootAnimationPosition = animator.lockRootPosition;
                ctx_->rendering.model->UpdateAnimation(editAnimatorPreviewModel_, 0.0f);
            }
        }
    }
    ImGui::TextDisabled("Keeps animation root translation aligned with the Entity.");
    SynchronizeAnimatorPreviewSettings(entity.id, animator);
    if (ImGui::DragFloat("Speed##Animator", &animator.speed, 0.01f, 0.0f, 100.0f, "%.2fx",
                         ImGuiSliderFlags_AlwaysClamp)) {
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
