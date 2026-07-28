#include "EditorScene.h"

#include "core/MathUtils.h"
#include "imgui.h"
#include "internal/EditorSceneAssetUtils.h"
#include "internal/EditorSceneViewportUtils.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "world/WorldSerializer.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

using namespace EditorSceneAssetUtils;
using namespace EditorSceneViewportUtils;

namespace {

struct SceneCameraAxes {
    DirectX::XMVECTOR right;
    DirectX::XMVECTOR up;
    DirectX::XMVECTOR forward;
};

SceneCameraAxes CalculateSceneCameraAxes(const DirectX::XMFLOAT3& rotation) {
    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    return {
        DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation),
        DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), orientation),
        DirectX::XMVector3TransformNormal(
            DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation),
    };
}

DirectX::XMVECTOR ReadSceneCameraKeyboardMovement(bool navigating,
                                                  const SceneCameraAxes& axes) {
    DirectX::XMVECTOR movement = DirectX::XMVectorZero();
    if (!navigating) {
        return movement;
    }
    if (ImGui::IsKeyDown(ImGuiKey_W)) {
        movement = DirectX::XMVectorAdd(movement, axes.forward);
    }
    if (ImGui::IsKeyDown(ImGuiKey_S)) {
        movement = DirectX::XMVectorSubtract(movement, axes.forward);
    }
    if (ImGui::IsKeyDown(ImGuiKey_D)) {
        movement = DirectX::XMVectorAdd(movement, axes.right);
    }
    if (ImGui::IsKeyDown(ImGuiKey_A)) {
        movement = DirectX::XMVectorSubtract(movement, axes.right);
    }
    if (ImGui::IsKeyDown(ImGuiKey_E)) {
        movement = DirectX::XMVectorAdd(movement, axes.up);
    }
    if (ImGui::IsKeyDown(ImGuiKey_Q)) {
        movement = DirectX::XMVectorSubtract(movement, axes.up);
    }
    return movement;
}

}  // namespace

void EditorScene::HandleSceneAssetDrop(const ImVec2& imageMin, const ImVec2& imageMax) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        CreateModelEntityFromAsset(static_cast<const char*>(payload->Data), position);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        const DirectX::XMFLOAT3 position = CalculateScenePlacementPosition(
            sceneViewCamera_, imageMin, imageMax, ImGui::GetMousePos());
        InstantiatePrefabAsset(static_cast<const char*>(payload->Data), {}, position);
    }
    ImGui::EndDragDropTarget();
}

void EditorScene::HandleSceneCameraControls(const ImVec2& imageMin, const ImVec2& imageMax,
                                            bool imageHovered) {
    if (imageHovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FocusSceneCameraOnSelection();
    }
    bool beginCapture = false;
    UpdateSceneCameraNavigationState(imageHovered, beginCapture);
    float pointerDeltaX = 0.0f;
    float pointerDeltaY = 0.0f;
    ReadSceneCameraPointerDelta(imageMin, imageMax, beginCapture, pointerDeltaX, pointerDeltaY);
    RotateSceneCamera(pointerDeltaX, pointerDeltaY);
    MoveSceneCamera(imageHovered, pointerDeltaX, pointerDeltaY);
}

void EditorScene::UpdateSceneCameraNavigationState(bool imageHovered, bool& beginCapture) {
    const bool beginLook = imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool beginPan = imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if (beginLook) {
        sceneCameraNavigating_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        sceneCameraNavigating_ = false;
    }
    if (beginPan) {
        sceneCameraPanning_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        sceneCameraPanning_ = false;
    }
    beginCapture = (beginLook || beginPan) && !sceneCameraCursorCaptured_;
}

void EditorScene::ReadSceneCameraPointerDelta(const ImVec2& imageMin, const ImVec2& imageMax,
                                              bool beginCapture, float& pointerDeltaX,
                                              float& pointerDeltaY) {
    const int cursorCenterX = static_cast<int>(std::lround((imageMin.x + imageMax.x) * 0.5f));
    const int cursorCenterY = static_cast<int>(std::lround((imageMin.y + imageMax.y) * 0.5f));
    if (beginCapture) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            sceneCameraCursorRestoreX_ = cursor.x;
            sceneCameraCursorRestoreY_ = cursor.y;
        }
        sceneCameraPointerTravel_ = 0.0f;
        sceneCameraCursorCaptured_ = true;
        SetCursorPos(cursorCenterX, cursorCenterY);
    }
    if (sceneCameraCursorCaptured_ && !beginCapture &&
        (sceneCameraNavigating_ || sceneCameraPanning_)) {
        POINT cursor{};
        if (GetCursorPos(&cursor)) {
            pointerDeltaX = static_cast<float>(cursor.x - cursorCenterX);
            pointerDeltaY = static_cast<float>(cursor.y - cursorCenterY);
            sceneCameraPointerTravel_ +=
                std::sqrt(pointerDeltaX * pointerDeltaX + pointerDeltaY * pointerDeltaY);
        }
        SetCursorPos(cursorCenterX, cursorCenterY);
    }
    if (sceneCameraCursorCaptured_ && !sceneCameraNavigating_ && !sceneCameraPanning_) {
        SetCursorPos(sceneCameraCursorRestoreX_, sceneCameraCursorRestoreY_);
        sceneCameraCursorCaptured_ = false;
    }
}

void EditorScene::RotateSceneCamera(float pointerDeltaX, float pointerDeltaY) {
    if (!sceneCameraNavigating_) {
        return;
    }
    constexpr float mouseSensitivity = 0.004f;
    DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    rotation.x = std::clamp(rotation.x + pointerDeltaY * mouseSensitivity,
                            -DirectX::XM_PIDIV2 + 0.01f, DirectX::XM_PIDIV2 - 0.01f);
    rotation.y += pointerDeltaX * mouseSensitivity;
    if (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f) {
        sceneViewCamera_.SetRotation(rotation);
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

void EditorScene::MoveSceneCamera(bool imageHovered, float pointerDeltaX, float pointerDeltaY) {
    const SceneCameraAxes axes = CalculateSceneCameraAxes(sceneViewCamera_.GetRotation());
    DirectX::XMVECTOR movement =
        ReadSceneCameraKeyboardMovement(sceneCameraNavigating_, axes);
    const ImGuiIO& io = ImGui::GetIO();
    const DirectX::XMFLOAT3 currentPosition = sceneViewCamera_.GetPosition();
    DirectX::XMVECTOR position = DirectX::XMLoadFloat3(&currentPosition);
    bool positionChanged = false;
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(movement)) > 0.0f) {
        const float deltaTime = std::clamp(io.DeltaTime, 0.0f, 0.1f);
        const float speed = io.KeyShift ? 12.0f : 4.0f;
        movement = DirectX::XMVectorScale(DirectX::XMVector3Normalize(movement), speed * deltaTime);
        position = DirectX::XMVectorAdd(position, movement);
        positionChanged = true;
    }
    if (sceneCameraPanning_ && (pointerDeltaX != 0.0f || pointerDeltaY != 0.0f)) {
        constexpr float panSensitivity = 0.01f;
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(axes.right, -pointerDeltaX * panSensitivity));
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(axes.up, pointerDeltaY * panSensitivity));
        positionChanged = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (imageHovered && io.MouseWheel != 0.0f) {
        position = DirectX::XMVectorAdd(
            position, DirectX::XMVectorScale(axes.forward, io.MouseWheel * 0.75f));
        positionChanged = true;
    }
    if (positionChanged) {
        DirectX::XMFLOAT3 updatedPosition{};
        DirectX::XMStoreFloat3(&updatedPosition, position);
        sceneViewCamera_.SetPosition(updatedPosition);
    }
}

bool EditorScene::FocusSceneCameraOnSelection() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    if (entity == nullptr || !world_.TryGetWorldMatrix(selection_, worldMatrix)) {
        status_ = "Select an entity before focusing the Scene camera.";
        return false;
    }

    DirectX::XMFLOAT3 localCenter{};
    float radius = 1.0f;
    if (entity->meshRenderer && ctx_ != nullptr && ctx_->rendering.model != nullptr) {
        const ModelHandle handle = ResolveModel(*entity->meshRenderer);
        const Model* model = handle.IsValid() ? ctx_->rendering.model->GetModel(handle) : nullptr;
        DirectX::XMFLOAT3 boundsMin{};
        DirectX::XMFLOAT3 boundsMax{};
        if (model != nullptr && TryGetModelBounds(*model, boundsMin, boundsMax)) {
            localCenter = {(boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f,
                           (boundsMin.z + boundsMax.z) * 0.5f};
            const float extentX = (boundsMax.x - boundsMin.x) * 0.5f;
            const float extentY = (boundsMax.y - boundsMin.y) * 0.5f;
            const float extentZ = (boundsMax.z - boundsMin.z) * 0.5f;
            radius = (std::max)(0.1f, std::sqrt(extentX * extentX + extentY * extentY +
                                                extentZ * extentZ));
        }
    }

    const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&worldMatrix);
    const DirectX::XMVECTOR center =
        DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&localCenter), world);
    const float scaleX = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[0]));
    const float scaleY = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[1]));
    const float scaleZ = DirectX::XMVectorGetX(DirectX::XMVector3Length(world.r[2]));
    radius *= (std::max)({scaleX, scaleY, scaleZ, 0.001f});

    const DirectX::XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    const DirectX::XMMATRIX orientation =
        DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, 0.0f);
    const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation);
    const float distance =
        (std::max)(1.0f, radius / std::tan(sceneViewCamera_.GetFovY() * 0.5f) * 1.25f);
    const DirectX::XMVECTOR position =
        DirectX::XMVectorSubtract(center, DirectX::XMVectorScale(forward, distance));
    DirectX::XMFLOAT3 focusedPosition{};
    DirectX::XMStoreFloat3(&focusedPosition, position);
    sceneViewCamera_.SetPosition(focusedPosition);
    sceneViewCamera_.SetClipRange(0.01f, (std::max)(1000.0f, distance + radius * 4.0f));
    status_ = "Focused the Scene camera on " + entity->name + ".";
    return true;
}

bool EditorScene::AlignSelectedCameraToSceneView() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before aligning it to the Scene View.";
        return false;
    }

    using namespace DirectX;
    XMFLOAT4X4 currentWorld{};
    TransformComponent currentWorldTransform{};
    if (!world_.TryGetWorldMatrix(entity->id, currentWorld) ||
        !TryDecomposeTransformComponent(XMLoadFloat4x4(&currentWorld), currentWorldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }

    const XMFLOAT3 position = sceneViewCamera_.GetPosition();
    const XMFLOAT3 rotation = sceneViewCamera_.GetRotation();
    XMMATRIX local = XMMatrixScaling(currentWorldTransform.scale.x, currentWorldTransform.scale.y,
                                     currentWorldTransform.scale.z) *
                     XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
                     XMMatrixTranslation(position.x, position.y, position.z);
    if (entity->parent.IsValid()) {
        XMFLOAT4X4 parentWorld{};
        if (!world_.TryGetWorldMatrix(entity->parent, parentWorld)) {
            status_ = "Could not read the Camera parent transform.";
            return false;
        }
        XMVECTOR determinant{};
        const XMMATRIX inverseParent = XMMatrixInverse(&determinant, XMLoadFloat4x4(&parentWorld));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) || std::abs(determinantValue) <= 1.0e-8f) {
            status_ = "Cannot align a Camera under a singular parent transform.";
            return false;
        }
        local *= inverseParent;
    }

    TransformComponent aligned{};
    if (!TryDecomposeTransformComponent(local, aligned)) {
        status_ = "Could not calculate the aligned Camera transform.";
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->transform = aligned;
    RecordImmediateEdit("Align Camera to Scene View", before, selectionBefore);
    status_ = "Aligned " + entity->name + " to the Scene View.";
    return true;
}

bool EditorScene::AlignSceneViewToSelectedCamera() {
    const WorldEntity* entity = world_.Find(selection_);
    DirectX::XMFLOAT4X4 worldMatrix{};
    TransformComponent worldTransform{};
    if (entity == nullptr || !entity->camera) {
        status_ = "Select a Camera before moving the Scene View.";
        return false;
    }
    if (!world_.TryGetWorldMatrix(entity->id, worldMatrix) ||
        !TryDecomposeTransformComponent(DirectX::XMLoadFloat4x4(&worldMatrix), worldTransform)) {
        status_ = "Could not read the Camera world transform.";
        return false;
    }
    sceneViewCamera_.SetPosition(worldTransform.position);
    sceneViewCamera_.SetRotation({DirectX::XMConvertToRadians(worldTransform.rotationDegrees.x),
                                  DirectX::XMConvertToRadians(worldTransform.rotationDegrees.y),
                                  DirectX::XMConvertToRadians(worldTransform.rotationDegrees.z)});
    status_ = "Moved the Scene View to " + entity->name + ".";
    return true;
}
