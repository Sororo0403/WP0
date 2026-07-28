#include "EditorScene.h"

#include "graphics/DirectXCommon.h"
#include "imgui.h"
#include "internal/EditorSceneViewportUtils.h"

#include <algorithm>

using namespace EditorSceneViewportUtils;

bool EditorScene::DrawSelectedCameraPreview(const ImVec2& imageMin, const ImVec2& imageMax) {
    const WorldEntity* entity = ResolveSelectedCameraPreviewEntity();
    ImVec2 previewMin{};
    ImVec2 previewMax{};
    if (entity == nullptr ||
        !PrepareSelectedCameraPreviewRect(*entity, imageMin, imageMax, previewMin, previewMax)) {
        return false;
    }
    RenderSelectedCameraPreview();
    return DrawSelectedCameraPreviewOverlay(*entity, previewMin, previewMax);
}

const WorldEntity* EditorScene::ResolveSelectedCameraPreviewEntity() const {
    const WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !world_.IsActiveInHierarchy(selection_) || !entity->camera ||
        !cameraPreviewSurface_.IsReady() || !cameraPreviewPostProcess_.IsReady() ||
        ctx_ == nullptr || ctx_->rendering.dxCommon == nullptr ||
        ctx_->rendering.model == nullptr) {
        return nullptr;
    }
    return entity;
}

bool EditorScene::PrepareSelectedCameraPreviewRect(const WorldEntity& entity,
                                                   const ImVec2& imageMin,
                                                   const ImVec2& imageMax, ImVec2& previewMin,
                                                   ImVec2& previewMax) {
    return TryGetCameraPreviewRect(imageMin, imageMax, previewMin, previewMax) &&
           UpdateCameraFromEntity(entity.id, cameraPreviewCamera_, cameraPreviewSurface_.GetWidth(),
                                  cameraPreviewSurface_.GetHeight());
}

void EditorScene::RenderSelectedCameraPreview() {
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
}

bool EditorScene::DrawSelectedCameraPreviewOverlay(const WorldEntity& entity,
                                                   const ImVec2& previewMin,
                                                   const ImVec2& previewMax) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const D3D12_GPU_DESCRIPTOR_HANDLE output = cameraPreviewSurface_.GetOutputGpuHandle();
    drawList->AddImage(static_cast<ImTextureID>(output.ptr), previewMin, previewMax);
    drawList->AddRect(previewMin, previewMax, IM_COL32(255, 184, 56, 255), 3.0f, 0, 2.0f);
    const std::string label = "Camera Preview  |  " + entity.name;
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
