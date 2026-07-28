#include "AssetImportPlanner.h"
#include "EditorScene.h"
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

#include "internal/EditorSceneHierarchyUtils.h"

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

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawAudioSourceInspector(WorldEntity* entity) {
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
                ImGui::TextDisabled("Runtime: %s", source.runtimePlaying ? "Playing" : "Stopped");
            } else {
                ImGui::TextDisabled(
                    "Script API: PlayAudioSource / PlayAudioSourceOneShot / StopAudioSource");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##AudioSource", &source.enabled)) {
                RecordImmediateEdit("Toggle AudioSource", std::move(before), selectionBefore);
            }

            const std::string clipLabel =
                source.clipPath.empty() ? "None (drop an Audio asset)" : source.clipPath;
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
                    RecordImmediateEdit("Clear Audio Clip", std::move(before), selectionBefore);
                }
                for (const std::filesystem::path& audioAsset : audioAssets_) {
                    const std::string label = audioAsset.generic_string();
                    if (ImGui::Selectable(label.c_str(), source.clipPath == label)) {
                        AssignAudioAsset(selection_, audioAsset);
                    }
                }
                ImGui::EndPopup();
            }

            auto drawAudioCheckbox = [&](const char* label, bool& value, const char* historyLabel) {
                before = WorldSerializer::Serialize(world_);
                if (ImGui::Checkbox(label, &value)) {
                    RecordImmediateEdit(historyLabel, std::move(before), selectionBefore);
                }
            };
            drawAudioCheckbox("Play On Awake##AudioSource", source.playOnAwake,
                              "Toggle AudioSource Play On Awake");
            drawAudioCheckbox("Loop##AudioSource", source.loop, "Toggle AudioSource Loop");
            drawAudioCheckbox("Spatial##AudioSource", source.spatial, "Toggle AudioSource Spatial");

            auto drawAudioFloat = [&](const char* label, float& value, float speed, float minimum,
                                      float maximum) {
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
                drawAudioFloat("Min Distance##AudioSource", source.minDistance, 0.05f, 0.0f,
                               (std::max)(0.0f, source.maxDistance - 0.01f));
                drawAudioFloat("Max Distance##AudioSource", source.maxDistance, 0.1f,
                               source.minDistance + 0.01f, 1000000.0f);
            }
        }
    }
}

void EditorScene::DrawAudioListenerInspector(WorldEntity* entity) {
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
}

void EditorScene::DrawAnimatorInspector(WorldEntity* entity) {
    if (entity->animator) {
        ImGui::SeparatorText("Animator");
        if (ImGui::Button("Remove Animator")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            if (editAnimatorPreviewEntity_ == entity->id) {
                EndEditAnimatorPreview();
            }
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

            const ModelHandle handle =
                entity->meshRenderer ? ResolveModel(*entity->meshRenderer) : ModelHandle{};
            const Model* model = handle.IsValid() && ctx_ != nullptr && ctx_->rendering.model
                                     ? ctx_->rendering.model->GetModel(handle)
                                     : nullptr;
            const char* clipLabel = animator.clip.empty() ? "First Clip" : animator.clip.c_str();
            if (ImGui::BeginCombo("Clip##Animator", clipLabel)) {
                if (ImGui::Selectable("First Clip", animator.clip.empty())) {
                    before = WorldSerializer::Serialize(world_);
                    animator.clip.clear();
                    RecordImmediateEdit("Change Animator Clip", std::move(before), selectionBefore);
                    if (editAnimatorPreviewEntity_ == entity->id) {
                        BeginEditAnimatorPreview(entity->id);
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
                            before = WorldSerializer::Serialize(world_);
                            animator.clip = clip;
                            RecordImmediateEdit("Change Animator Clip", std::move(before),
                                                selectionBefore);
                            if (editAnimatorPreviewEntity_ == entity->id) {
                                BeginEditAnimatorPreview(entity->id);
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (model == nullptr || model->animations.empty()) {
                ImGui::TextDisabled("Assign an animated Model to Mesh Renderer.");
            }
            if (!IsInPlayMode() && model != nullptr && !model->animations.empty()) {
                const bool previewing =
                    editAnimatorPreviewEntity_ == entity->id && editAnimatorPreviewModel_.IsValid();
                Model* previewModel =
                    previewing && ctx_ != nullptr && ctx_->rendering.model != nullptr
                        ? ctx_->rendering.model->GetModel(editAnimatorPreviewModel_)
                        : nullptr;
                const bool previewPlaying = previewModel != nullptr && previewModel->isPlaying;
                if (ImGui::Button(previewPlaying ? "Pause##AnimatorPreview"
                                                 : "Play##AnimatorPreview")) {
                    if (previewModel != nullptr && !previewPlaying &&
                        previewModel->animationFinished) {
                        status_ = BeginEditAnimatorPreview(entity->id)
                                      ? "Restarted Animator preview."
                                      : "Animator preview could not be started.";
                    } else if (previewModel != nullptr && !previewPlaying) {
                        previewModel->isPlaying = true;
                        previewModel->animationFinished = false;
                        status_ = "Resumed Animator preview.";
                    } else if (previewPlaying) {
                        previewModel->isPlaying = false;
                        status_ = "Paused Animator preview.";
                    } else if (BeginEditAnimatorPreview(entity->id)) {
                        status_ = "Started Animator preview.";
                    } else {
                        status_ = "Animator preview could not be started.";
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Restart##AnimatorPreview")) {
                    status_ = BeginEditAnimatorPreview(entity->id)
                                  ? "Restarted Animator preview."
                                  : "Animator preview could not be started.";
                }
                if (previewModel != nullptr) {
                    ImGui::SameLine();
                    if (ImGui::Button("Stop##AnimatorPreview")) {
                        EndEditAnimatorPreview();
                        status_ = "Stopped Animator preview.";
                    }
                    const auto clip = previewModel->animations.find(previewModel->currentAnimation);
                    const float duration = clip != previewModel->animations.end()
                                               ? (std::max)(0.0f, clip->second.duration)
                                               : 0.0f;
                    float time = std::clamp(previewModel->animationTime, 0.0f, duration);
                    if (ImGui::SliderFloat("Time##AnimatorPreview", &time, 0.0f, duration,
                                           "%.2f s")) {
                        previewModel->animationTime = time;
                        previewModel->isPlaying = false;
                        previewModel->animationFinished = duration > 0.0f && time >= duration;
                        ctx_->rendering.model->UpdateAnimation(editAnimatorPreviewModel_, 0.0f);
                        status_ = "Scrubbed Animator preview.";
                    }
                }
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
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Lock Root Position##Animator", &animator.lockRootPosition)) {
                RecordImmediateEdit("Toggle Animator Root Position Lock", std::move(before),
                                    selectionBefore);
                status_ = animator.lockRootPosition ? "Locked Animator root position."
                                                    : "Unlocked Animator root position.";
                if (editAnimatorPreviewEntity_ == entity->id &&
                    editAnimatorPreviewModel_.IsValid() && ctx_ != nullptr &&
                    ctx_->rendering.model != nullptr) {
                    if (Model* previewModel =
                            ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
                        previewModel != nullptr) {
                        previewModel->lockRootAnimationPosition = animator.lockRootPosition;
                        ctx_->rendering.model->UpdateAnimation(editAnimatorPreviewModel_, 0.0f);
                    }
                }
            }
            ImGui::TextDisabled("Keeps animation root translation aligned with the Entity.");
            if (editAnimatorPreviewEntity_ == entity->id && editAnimatorPreviewModel_.IsValid() &&
                ctx_ != nullptr && ctx_->rendering.model != nullptr) {
                if (Model* previewModel =
                        ctx_->rendering.model->GetModel(editAnimatorPreviewModel_);
                    previewModel != nullptr) {
                    previewModel->isLoop = animator.loop;
                }
            }
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
    }
}
