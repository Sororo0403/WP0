#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneViewportUtils.h"

#include <cmath>

using namespace EditorSceneViewportUtils;

bool EditorScene::HasSceneComponentGizmo(const WorldEntity& entity) const {
    return entity.camera || entity.light || entity.audioSource || entity.audioListener ||
           entity.boxCollider || entity.characterController;
}

bool EditorScene::IsSceneComponentGizmoEnabled(const WorldEntity& entity) const {
    return world_.IsActiveInHierarchy(entity.id) &&
           ((entity.camera && entity.camera->enabled) ||
            (entity.light && entity.light->enabled) ||
            (entity.audioSource && entity.audioSource->enabled) ||
            (entity.audioListener && entity.audioListener->enabled) ||
            (entity.boxCollider && entity.boxCollider->enabled) ||
            (entity.characterController && entity.characterController->enabled));
}

bool EditorScene::ShouldDrawScenePhysicsShapes(const WorldEntity& entity) const {
    const bool physicsLayerVisible =
        (physicsDebugLayerMask_ & (uint32_t{1} << entity.layer)) != 0u;
    return showPhysicsDebug_ && world_.IsActiveInHierarchy(entity.id) &&
           physicsLayerVisible && (entity.boxCollider || entity.characterController);
}

uint32_t EditorScene::ResolveSceneComponentGizmoColor(
    const WorldEntity& entity, bool active, bool selected, bool enabled) const {
    uint32_t color = IM_COL32(80, 230, 130, 230);
    if (entity.camera) {
        color = IM_COL32(90, 185, 255, 230);
    } else if (entity.light) {
        color = IM_COL32(255, 215, 80, 230);
    } else if (entity.audioSource) {
        color = IM_COL32(190, 120, 255, 230);
    } else if (entity.audioListener) {
        color = IM_COL32(80, 215, 230, 230);
    }
    if (active) {
        color = IM_COL32(255, 184, 56, 255);
    } else if (selected) {
        color = IM_COL32(90, 190, 255, 255);
    }
    return enabled ? color : (color & 0x00FFFFFFu) | (100u << 24u);
}

void EditorScene::DrawSceneCameraGizmoIcon(const ImVec2& center,
                                            uint32_t color) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRect({center.x - 8.0f, center.y - 6.0f},
                      {center.x + 5.0f, center.y + 6.0f}, color, 2.0f, 0, 1.8f);
    drawList->AddTriangle({center.x + 5.0f, center.y - 5.0f},
                          {center.x + 12.0f, center.y - 9.0f},
                          {center.x + 12.0f, center.y + 1.0f}, color, 1.8f);
}

void EditorScene::DrawSceneLightGizmoIcon(const ImVec2& center,
                                           uint32_t color) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircle(center, 5.0f, color, 16, 1.8f);
    for (int index = 0; index < 8; ++index) {
        const float angle = DirectX::XM_2PI * static_cast<float>(index) / 8.0f;
        const ImVec2 direction{std::cos(angle), std::sin(angle)};
        drawList->AddLine({center.x + direction.x * 7.0f, center.y + direction.y * 7.0f},
                          {center.x + direction.x * 11.0f, center.y + direction.y * 11.0f},
                          color, 1.5f);
    }
}

void EditorScene::DrawSceneAudioSourceGizmoIcon(const ImVec2& center,
                                                 uint32_t color) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
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
            const float angle =
                -DirectX::XM_PIDIV4 +
                DirectX::XM_PIDIV2 * static_cast<float>(index) /
                    static_cast<float>(arcSegments);
            const ImVec2 point{
                center.x + 1.0f + std::cos(angle) * radius,
                center.y + std::sin(angle) * radius,
            };
            if (index > 0) {
                drawList->AddLine(previous, point, color, 1.5f);
            }
            previous = point;
        }
    }
}

void EditorScene::DrawSceneAudioListenerGizmoIcon(const ImVec2& center,
                                                   uint32_t color) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircle(center, 3.0f, color, 16, 1.8f);
    drawList->AddCircle(center, 7.0f, color, 24, 1.5f);
    drawList->AddCircle(center, 11.0f, color, 32, 1.25f);
}

void EditorScene::DrawSceneComponentGizmoIcon(
    const WorldEntity& entity, const ImVec2& center, uint32_t color) const {
    if (entity.camera) {
        DrawSceneCameraGizmoIcon(center, color);
    } else if (entity.light) {
        DrawSceneLightGizmoIcon(center, color);
    } else if (entity.audioSource) {
        DrawSceneAudioSourceGizmoIcon(center, color);
    } else if (entity.audioListener) {
        DrawSceneAudioListenerGizmoIcon(center, color);
    } else {
        ImGui::GetWindowDrawList()->AddRect(
            {center.x - 6.0f, center.y - 6.0f},
            {center.x + 6.0f, center.y + 6.0f}, color, 1.0f, 0, 1.8f);
    }
}

void EditorScene::DrawScenePhysicsLayerLabel(
    const WorldEntity& entity, const ImVec2& center) const {
    std::string layerLabel = physicsSettings_.layerNames[entity.layer];
    if (layerLabel.empty()) {
        layerLabel = "Layer " + std::to_string(entity.layer);
    }
    const ImVec2 labelPosition{center.x + 10.0f, center.y + 8.0f};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText({labelPosition.x + 1.0f, labelPosition.y + 1.0f},
                      IM_COL32(0, 0, 0, 220), layerLabel.c_str());
    drawList->AddText(
        labelPosition, PhysicsDebugLayerColor(entity.layer), layerLabel.c_str());
}

void EditorScene::DrawSingleSceneComponentGizmo(
    const WorldEntity& entity, const ImVec2& imageMin, const ImVec2& imageMax) const {
    DirectX::XMFLOAT4X4 worldMatrix{};
    ImVec2 center{};
    if (!world_.TryGetWorldMatrix(entity.id, worldMatrix) ||
        !ProjectScenePoint(sceneViewCamera_,
                           {worldMatrix._41, worldMatrix._42, worldMatrix._43},
                           imageMin, imageMax, center)) {
        return;
    }
    const bool active = entity.id == selection_;
    const bool selected = hierarchySelection_.contains(entity.id);
    const bool drawPhysicsShapes = ShouldDrawScenePhysicsShapes(entity);
    const uint32_t color = ResolveSceneComponentGizmoColor(
        entity, active, selected, IsSceneComponentGizmoEnabled(entity));
    DrawSceneComponentGizmoIcon(entity, center, color);
    DrawSceneActiveComponentGuides(entity, worldMatrix, imageMin, imageMax, active);
    DrawScenePhysicsGizmos(entity, imageMin, imageMax, active, drawPhysicsShapes);
    if (drawPhysicsShapes) {
        DrawScenePhysicsLayerLabel(entity, center);
    }
    if (active && (!entity.meshRenderer || !entity.meshRenderer->enabled)) {
        ImGui::GetWindowDrawList()->AddText(
            {center.x + 14.0f, center.y - ImGui::GetTextLineHeight() * 0.5f},
            color, entity.name.c_str());
    }
}

void EditorScene::DrawSceneComponentGizmos(
    const ImVec2& imageMin, const ImVec2& imageMax) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(imageMin, imageMax, true);
    for (const WorldEntity& entity : world_.Entities()) {
        if (HasSceneComponentGizmo(entity)) {
            DrawSingleSceneComponentGizmo(entity, imageMin, imageMax);
        }
    }
    drawList->PopClipRect();
}
