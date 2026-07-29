#include "EditorScene.h"

#include "graphics/DirectXCommon.h"
#include "imgui.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/ModelRenderer.h"

#include <algorithm>
#include <cfloat>
#include <ranges>
#include <unordered_set>

void EditorScene::DrawAssetPreviewPopup() {
    ImGui::SetNextWindowSize({360.0f, 460.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopup("Model Preview")) {
        return;
    }
    const std::string filename = assetPreviewAsset_.filename().string();
    ImGui::TextUnformatted(filename.c_str());
    ImGui::Separator();
    if (!IsAssetPreviewPopupReady()) {
        ImGui::TextDisabled("Model preview is not ready.");
        ImGui::EndPopup();
        return;
    }

    ModelManager& modelManager = *ctx_->rendering.model;
    Model& model = *modelManager.GetModel(assetPreviewModel_);
    DrawAssetPreviewModelSummary(model);
    DrawAssetPreviewAnimationControls(modelManager, model);
    DrawAssetPreviewViewport(modelManager);
    ImGui::EndPopup();
}

bool EditorScene::IsAssetPreviewPopupReady() const {
    return assetPreviewModel_.IsValid() && assetPreviewSurface_.IsReady() &&
           assetPreviewPostProcess_.IsReady() && ctx_ != nullptr &&
           ctx_->rendering.dxCommon != nullptr && ctx_->rendering.model != nullptr &&
           ctx_->rendering.model->GetModel(assetPreviewModel_) != nullptr;
}

void EditorScene::DrawAssetPreviewModelSummary(const Model& model) const {
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    std::unordered_set<uint32_t> materials;
    for (const ModelSubMesh& subMesh : model.subMeshes) {
        vertexCount += subMesh.vertexCount;
        if (IsValidResourceId(subMesh.meshId)) {
            triangleCount += ctx_->rendering.model->GetMesh(subMesh.meshId).indexCount / 3u;
        }
        if (IsValidResourceId(subMesh.materialId)) {
            materials.insert(subMesh.materialId);
        }
    }
    if (model.subMeshes.empty() && IsValidResourceId(model.meshId)) {
        const Mesh& mesh = ctx_->rendering.model->GetMesh(model.meshId);
        vertexCount = mesh.vertexStride == 0u ? 0u : mesh.vertexBytes / mesh.vertexStride;
        triangleCount = mesh.indexCount / 3u;
        if (IsValidResourceId(model.materialId)) {
            materials.insert(model.materialId);
        }
    }
    ImGui::TextDisabled("Meshes: %zu   Vertices: %llu   Triangles: %llu",
                        model.subMeshes.empty() ? size_t{1} : model.subMeshes.size(),
                        static_cast<unsigned long long>(vertexCount),
                        static_cast<unsigned long long>(triangleCount));
    ImGui::TextDisabled("Materials: %zu   Animations: %zu   Bones: %zu", materials.size(),
                        model.animations.size(), model.bones.size());
}

void EditorScene::DrawAssetPreviewAnimationControls(ModelManager& modelManager, Model& model) {
    if (model.animations.empty()) {
        return;
    }
    std::vector<std::string> animationNames;
    animationNames.reserve(model.animations.size());
    for (const auto& [name, clip] : model.animations) {
        (void)clip;
        animationNames.push_back(name);
    }
    std::ranges::sort(animationNames);
    EnsureAssetPreviewAnimation(modelManager, model, animationNames);
    DrawAssetPreviewAnimationSelector(modelManager, animationNames);
    DrawAssetPreviewPlaybackControls(modelManager, model);
    UpdateAssetPreviewAnimation(modelManager, model);
}

void EditorScene::EnsureAssetPreviewAnimation(
    ModelManager& modelManager, Model& model,
    const std::vector<std::string>& animationNames) {
    if (!assetPreviewAnimation_.empty() &&
        model.animations.contains(assetPreviewAnimation_)) {
        return;
    }
    assetPreviewAnimation_ = model.animations.contains(model.currentAnimation)
                                 ? model.currentAnimation
                                 : animationNames.front();
    modelManager.PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                               assetPreviewAnimationLoop_);
}

void EditorScene::DrawAssetPreviewAnimationSelector(
    ModelManager& modelManager, const std::vector<std::string>& animationNames) {
    if (!ImGui::BeginCombo("Animation##ModelPreview", assetPreviewAnimation_.c_str())) {
        return;
    }
    for (const std::string& name : animationNames) {
        const bool selected = name == assetPreviewAnimation_;
        if (ImGui::Selectable(name.c_str(), selected)) {
            assetPreviewAnimation_ = name;
            modelManager.PlayAnimation(assetPreviewModel_, name, assetPreviewAnimationLoop_);
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void EditorScene::DrawAssetPreviewPlaybackControls(ModelManager& modelManager, Model& model) {
    if (model.isPlaying) {
        if (ImGui::Button("Pause##ModelPreviewAnimation")) {
            model.isPlaying = false;
        }
    } else if (ImGui::Button("Play##ModelPreviewAnimation")) {
        if (model.animationFinished) {
            modelManager.PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                       assetPreviewAnimationLoop_);
        } else {
            model.isPlaying = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart##ModelPreviewAnimation")) {
        modelManager.PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                   assetPreviewAnimationLoop_);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop##ModelPreviewAnimation", &assetPreviewAnimationLoop_)) {
        model.isLoop = assetPreviewAnimationLoop_;
    }
    ImGui::SetNextItemWidth(140.0f);
    ImGui::DragFloat("Speed##ModelPreviewAnimation", &assetPreviewAnimationSpeed_, 0.01f,
                     0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
}

void EditorScene::SeekAssetPreviewAnimation(ModelManager& modelManager, Model& model,
                                            const float duration, const float time) {
    model.animationTime = std::clamp(time, 0.0f, duration);
    model.isPlaying = false;
    model.animationFinished = duration > 0.0f && model.animationTime >= duration;
    modelManager.UpdateAnimation(assetPreviewModel_, 0.0f);
}

void EditorScene::UpdateAssetPreviewAnimation(ModelManager& modelManager, Model& model) {
    const float duration =
        (std::max)(model.animations.at(assetPreviewAnimation_).duration, 0.0f);
    DrawAssetPreviewSeekButtons(modelManager, model, duration);
    DrawAssetPreviewTimeline(modelManager, model, duration);
    AdvanceAssetPreviewAnimation(modelManager, model);
}

void EditorScene::DrawAssetPreviewSeekButtons(ModelManager& modelManager, Model& model,
                                              const float duration) {
    constexpr float kStepSeconds = 1.0f / 30.0f;
    if (ImGui::SmallButton("|<##ModelPreviewAnimation")) {
        SeekAssetPreviewAnimation(modelManager, model, duration, 0.0f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("<##ModelPreviewAnimation")) {
        SeekAssetPreviewAnimation(modelManager, model, duration,
                                  model.animationTime - kStepSeconds);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Step backward 1/30 second");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">##ModelPreviewAnimation")) {
        SeekAssetPreviewAnimation(modelManager, model, duration,
                                  model.animationTime + kStepSeconds);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Step forward 1/30 second");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|##ModelPreviewAnimation")) {
        SeekAssetPreviewAnimation(modelManager, model, duration, duration);
    }
}

void EditorScene::DrawAssetPreviewTimeline(ModelManager& modelManager, Model& model,
                                           const float duration) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    float animationTime = model.animationTime;
    if (ImGui::SliderFloat("##ModelPreviewAnimationTimeline", &animationTime, 0.0f,
                           duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
        SeekAssetPreviewAnimation(modelManager, model, duration, animationTime);
    }
    ImGui::TextDisabled("%.2f / %.2f s", model.animationTime, duration);
}

void EditorScene::AdvanceAssetPreviewAnimation(ModelManager& modelManager,
                                               const Model& model) {
    if (model.isPlaying) {
        const float deltaTime =
            std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f) *
            assetPreviewAnimationSpeed_;
        modelManager.UpdateAnimation(assetPreviewModel_, deltaTime);
    }
}

void EditorScene::DrawAssetPreviewViewport(ModelManager& modelManager) {
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
    RenderAssetPreview(modelManager);
    const D3D12_GPU_DESCRIPTOR_HANDLE output = assetPreviewSurface_.GetOutputGpuHandle();
    ImGui::Image(static_cast<ImTextureID>(output.ptr), {320.0f, 320.0f});
    HandleAssetPreviewRotation();
}

void EditorScene::RenderAssetPreview(ModelManager& modelManager) {
    assetPreviewSurface_.BeginScenePass({0.035f, 0.045f, 0.065f, 1.0f});
    modelManager.GetRenderer()->PreDraw();
    modelManager.Draw(assetPreviewModel_, assetPreviewTransform_, assetPreviewCamera_);
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
}

void EditorScene::HandleAssetPreviewRotation() {
    if (!ImGui::IsItemHovered() || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        return;
    }
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    assetPreviewRotationDegrees_.x =
        std::clamp(assetPreviewRotationDegrees_.x + delta.y * 0.4f, -89.0f, 89.0f);
    assetPreviewRotationDegrees_.y += delta.x * 0.4f;
}
