#include "EditorScene.h"

#include "imgui.h"
#include "ImGuizmo.h"
#include "internal/EditorSceneViewportUtils.h"

#include <cmath>
#include <ranges>
#include <string>

using namespace EditorSceneViewportUtils;

bool EditorScene::DrawSceneTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax) {
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (!TryGetSelectedTransformGizmoMatrix(worldMatrix)) {
        ResetTransformGizmoInteraction();
        return false;
    }
    const DirectX::XMFLOAT4X4 worldBeforeManipulation = worldMatrix;
    const bool manipulated = ManipulateTransformGizmo(imageMin, imageMax, worldMatrix);
    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !gizmoWasUsing_) {
        BeginTransformGizmoInteraction(worldBeforeManipulation);
    }
    if (manipulated && activeGizmoEntity_ == selection_) {
        ApplyTransformGizmoManipulation(worldMatrix);
    }
    FinishTransformGizmoInteraction(usingNow);
    return ImGuizmo::IsOver() || usingNow;
}

bool EditorScene::TryGetSelectedTransformGizmoMatrix(
    DirectX::XMFLOAT4X4& worldMatrix) {
    return world_.Find(selection_) != nullptr &&
           world_.TryGetWorldMatrix(selection_, worldMatrix);
}

void EditorScene::ResetTransformGizmoInteraction() {
    if (!gizmoWasUsing_) {
        return;
    }
    CommitHistoryEdit();
    gizmoWasUsing_ = false;
    activeGizmoEntity_ = {};
    activeGizmoWorldTransforms_.clear();
}

bool EditorScene::ManipulateTransformGizmo(const ImVec2& imageMin, const ImVec2& imageMax,
                                           DirectX::XMFLOAT4X4& worldMatrix) {
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMStoreFloat4x4(&view, sceneViewCamera_.GetView());
    DirectX::XMStoreFloat4x4(&projection, sceneViewCamera_.GetProj());
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x,
                     imageMax.y - imageMin.y);
    ImGuizmo::SetOrthographic(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (gizmoOperation_ == GizmoOperation::Rotate) {
        operation = ImGuizmo::ROTATE;
    } else if (gizmoOperation_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
    }
    const ImGuizmo::MODE mode =
        gizmoSpace_ == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    std::array<float, 3> snapValues{};
    FillTransformGizmoSnapValues(snapValues);
    const bool snapActive = gizmoSnapEnabled_ || ImGui::GetIO().KeyCtrl;
    return ImGuizmo::Manipulate(&view._11, &projection._11, operation, mode,
                                &worldMatrix._11, nullptr,
                                snapActive ? snapValues.data() : nullptr);
}

void EditorScene::FillTransformGizmoSnapValues(std::array<float, 3>& values) const {
    const float snap = gizmoOperation_ == GizmoOperation::Translate ? translationSnap_
                       : gizmoOperation_ == GizmoOperation::Rotate  ? rotationSnapDegrees_
                                                                    : scaleSnap_;
    std::ranges::fill(values, snap);
}

void EditorScene::BeginTransformGizmoInteraction(
    const DirectX::XMFLOAT4X4& worldMatrix) {
    SynchronizeHierarchySelection();
    const std::vector<EntityId> roots = GetTopLevelSelectedEntities();
    BeginHistoryEdit(roots.size() > 1u ? "Transform Entities" : "Transform Entity");
    activeGizmoEntity_ = selection_;
    activeGizmoStartWorld_ = worldMatrix;
    activeGizmoWorldTransforms_.clear();
    activeGizmoWorldTransforms_.reserve(roots.size());
    for (const EntityId root : roots) {
        DirectX::XMFLOAT4X4 initialWorld{};
        if (world_.TryGetWorldMatrix(root, initialWorld)) {
            activeGizmoWorldTransforms_.emplace_back(root, initialWorld);
        }
    }
}

void EditorScene::ApplyTransformGizmoManipulation(
    const DirectX::XMFLOAT4X4& worldMatrix) {
    DirectX::XMMATRIX delta{};
    if (!TryBuildTransformGizmoDelta(worldMatrix, delta)) {
        return;
    }
    bool changed = false;
    for (const auto& [entityId, initialWorld] : activeGizmoWorldTransforms_) {
        changed = ApplyTransformGizmoDeltaToEntity(entityId, initialWorld, delta) || changed;
    }
    if (changed) {
        RefreshDirty();
    }
}

bool EditorScene::TryBuildTransformGizmoDelta(
    const DirectX::XMFLOAT4X4& worldMatrix, DirectX::XMMATRIX& delta) const {
    using namespace DirectX;
    XMVECTOR determinant{};
    const XMMATRIX inverseStart =
        XMMatrixInverse(&determinant, XMLoadFloat4x4(&activeGizmoStartWorld_));
    const float determinantValue = XMVectorGetX(determinant);
    if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
        return false;
    }
    delta = inverseStart * XMLoadFloat4x4(&worldMatrix);
    return true;
}

bool EditorScene::ApplyTransformGizmoDeltaToEntity(
    const EntityId id, const DirectX::XMFLOAT4X4& initialWorld,
    const DirectX::XMMATRIX& delta) {
    WorldEntity* entity = world_.Find(id);
    if (entity == nullptr) {
        return false;
    }
    DirectX::XMMATRIX localMatrix = DirectX::XMLoadFloat4x4(&initialWorld) * delta;
    if (!ConvertGizmoWorldToLocal(*entity, localMatrix)) {
        return false;
    }
    TransformComponent transform = entity->transform;
    if (!TryDecomposeTransformComponent(localMatrix, transform)) {
        return false;
    }
    entity->transform = transform;
    return true;
}

bool EditorScene::ConvertGizmoWorldToLocal(const WorldEntity& entity,
                                           DirectX::XMMATRIX& matrix) const {
    using namespace DirectX;
    if (!entity.parent.IsValid()) {
        return true;
    }
    XMFLOAT4X4 parentWorld{};
    if (!world_.TryGetWorldMatrix(entity.parent, parentWorld)) {
        return false;
    }
    XMVECTOR determinant{};
    const XMMATRIX inverseParent =
        XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
    const float determinantValue = XMVectorGetX(determinant);
    if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
        return false;
    }
    matrix *= inverseParent;
    return true;
}

void EditorScene::FinishTransformGizmoInteraction(const bool usingNow) {
    if (!usingNow && gizmoWasUsing_) {
        CommitHistoryEdit();
        activeGizmoEntity_ = {};
        const size_t transformedCount = activeGizmoWorldTransforms_.size();
        activeGizmoWorldTransforms_.clear();
        status_ = transformedCount > 1u
                      ? "Transformed " + std::to_string(transformedCount) +
                            " entities from Scene View."
                      : "Transformed entity from Scene View.";
    }
    gizmoWasUsing_ = usingNow;
}
