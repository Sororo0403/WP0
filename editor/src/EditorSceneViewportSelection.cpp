#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/Model.h"
#include "model/ModelManager.h"

#include <cmath>
#include <limits>

using namespace EditorSceneViewportUtils;

void EditorScene::PickSceneEntity(const ImVec2& imageMin, const ImVec2& imageMax,
                                  bool imageHovered) {
    if (!imageHovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }
    const EntityId component =
        FindClosestSceneComponent(imageMin, imageMax, ImGui::GetMousePos());
    if (component.IsValid()) {
        ApplyScenePick(component);
        return;
    }
    if (ctx_ == nullptr || ctx_->rendering.model == nullptr) {
        ApplyScenePick({});
        return;
    }
    EntityId mesh{};
    if (TryRaycastSceneMesh(imageMin, imageMax, mesh)) {
        ApplyScenePick(mesh);
    }
}

EntityId EditorScene::FindClosestSceneComponent(const ImVec2& imageMin, const ImVec2& imageMax,
                                                const ImVec2& mouse) const {
    EntityId closest{};
    float closestDistanceSquared = 14.0f * 14.0f;
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
        if (distanceSquared <= closestDistanceSquared) {
            closest = entity.id;
            closestDistanceSquared = distanceSquared;
        }
    }
    return closest;
}

bool EditorScene::TryRaycastSceneMesh(const ImVec2& imageMin, const ImVec2& imageMax,
                                      EntityId& picked) const {
    using namespace DirectX;
    XMVECTOR nearPoint{};
    XMVECTOR rayDirection{};
    if (!BuildSceneRay(sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos(), nearPoint,
                       rayDirection)) {
        return false;
    }

    float closestDistance = (std::numeric_limits<float>::max)();
    const ModelManager* models = ctx_->rendering.model;
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
            picked = entity.id;
            closestDistance = hitDistance;
        }
    }
    return true;
}

void EditorScene::ApplyScenePick(EntityId picked) {
    const ImGuiIO& io = ImGui::GetIO();
    if (picked.IsValid()) {
        SelectHierarchyEntity(picked, io.KeyCtrl, false);
    } else if (!io.KeyCtrl) {
        ClearHierarchySelection();
    }
    if (picked.IsValid() && selection_ == picked &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        FocusSceneCameraOnSelection();
    }
}
