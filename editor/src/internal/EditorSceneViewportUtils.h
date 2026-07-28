#pragma once

#include "core/MathUtils.h"
#include "imgui.h"
#include "model/Model.h"
#include "world/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>

namespace EditorSceneViewportUtils {
inline ImU32 PhysicsDebugLayerColor(uint8_t layer, bool enabled = true) {
    constexpr std::array<std::array<uint8_t, 3>, 8> colors = {{
        {80, 230, 130},
        {80, 170, 255},
        {255, 105, 105},
        {245, 205, 75},
        {185, 120, 255},
        {65, 220, 215},
        {255, 150, 75},
        {245, 110, 190},
    }};
    const auto& color = colors[layer % colors.size()];
    return IM_COL32(color[0], color[1], color[2], enabled ? 220 : 80);
}

inline bool TryDecomposeTransformComponent(const DirectX::XMMATRIX& matrix,
                                            TransformComponent& transform) {
    using namespace DirectX;
    XMVECTOR scale{};
    XMVECTOR rotation{};
    XMVECTOR translation{};
    if (!XMMatrixDecompose(&scale, &rotation, &translation, matrix)) {
        return false;
    }
    XMFLOAT3 decomposedScale{};
    XMFLOAT3 decomposedTranslation{};
    XMStoreFloat3(&decomposedScale, scale);
    XMStoreFloat3(&decomposedTranslation, translation);
    const XMFLOAT3 decomposedRotation =
        MathUtils::RotationDegreesFromQuaternion(rotation, transform.rotationDegrees);
    const float values[] = {
        decomposedTranslation.x, decomposedTranslation.y, decomposedTranslation.z,
        decomposedRotation.x,    decomposedRotation.y,    decomposedRotation.z,
        decomposedScale.x,       decomposedScale.y,       decomposedScale.z,
    };
    const bool finite =
        std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
    if (!finite) {
        return false;
    }
    transform.position = decomposedTranslation;
    transform.rotationDegrees = decomposedRotation;
    transform.scale = decomposedScale;
    return true;
}

inline bool TryGetModelBounds(const Model& model, DirectX::XMFLOAT3& boundsMin,
                              DirectX::XMFLOAT3& boundsMax) {
    bool found = false;
    for (const ModelSubMesh& subMesh : model.subMeshes) {
        if (subMesh.vertexCount == 0u) {
            continue;
        }
        if (!found) {
            boundsMin = subMesh.sourceBoundsMin;
            boundsMax = subMesh.sourceBoundsMax;
            found = true;
            continue;
        }
        boundsMin.x = (std::min)(boundsMin.x, subMesh.sourceBoundsMin.x);
        boundsMin.y = (std::min)(boundsMin.y, subMesh.sourceBoundsMin.y);
        boundsMin.z = (std::min)(boundsMin.z, subMesh.sourceBoundsMin.z);
        boundsMax.x = (std::max)(boundsMax.x, subMesh.sourceBoundsMax.x);
        boundsMax.y = (std::max)(boundsMax.y, subMesh.sourceBoundsMax.y);
        boundsMax.z = (std::max)(boundsMax.z, subMesh.sourceBoundsMax.z);
    }
    return found;
}

inline bool IntersectRayBounds(DirectX::FXMVECTOR rayOrigin, DirectX::FXMVECTOR rayDirection,
                               const DirectX::XMFLOAT3& boundsMin,
                               const DirectX::XMFLOAT3& boundsMax, float& distance) {
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);

    const float extent = (std::max)({boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y,
                                     boundsMax.z - boundsMin.z});
    const float padding = (std::max)(0.01f, extent * 0.005f);
    const float minimum[3] = {boundsMin.x - padding, boundsMin.y - padding, boundsMin.z - padding};
    const float maximum[3] = {boundsMax.x + padding, boundsMax.y + padding, boundsMax.z + padding};
    const float rayOriginValues[3] = {origin.x, origin.y, origin.z};
    const float rayDirectionValues[3] = {direction.x, direction.y, direction.z};
    float entry = 0.0f;
    float exit = (std::numeric_limits<float>::max)();
    for (size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(rayDirectionValues[axis]) < 1.0e-7f) {
            if (rayOriginValues[axis] < minimum[axis] || rayOriginValues[axis] > maximum[axis]) {
                return false;
            }
            continue;
        }
        float nearDistance = (minimum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        float farDistance = (maximum[axis] - rayOriginValues[axis]) / rayDirectionValues[axis];
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
        }
        entry = (std::max)(entry, nearDistance);
        exit = (std::min)(exit, farDistance);
        if (entry > exit) {
            return false;
        }
    }
    distance = entry;
    return exit >= 0.0f;
}

inline bool BuildSceneRay(const Camera& camera, const ImVec2& imageMin, const ImVec2& imageMax,
                          const ImVec2& screenPosition, DirectX::XMVECTOR& rayOrigin,
                          DirectX::XMVECTOR& rayDirection) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    using namespace DirectX;
    rayOrigin = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 0.0f, 1.0f), 0.0f,
        0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(), XMMatrixIdentity());
    const XMVECTOR farPoint = XMVector3Unproject(
        XMVectorSet(screenPosition.x - imageMin.x, screenPosition.y - imageMin.y, 1.0f, 1.0f), 0.0f,
        0.0f, width, height, 0.0f, 1.0f, camera.GetProj(), camera.GetView(), XMMatrixIdentity());
    rayDirection = XMVector3Normalize(XMVectorSubtract(farPoint, rayOrigin));
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    XMStoreFloat3(&origin, rayOrigin);
    XMStoreFloat3(&direction, rayDirection);
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
}

inline bool ProjectScenePoint(const Camera& camera, const DirectX::XMFLOAT3& worldPosition,
                              const ImVec2& imageMin, const ImVec2& imageMax,
                              ImVec2& screenPosition, bool requireInside = true) {
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
        DirectX::XMVectorSet(worldPosition.x, worldPosition.y, worldPosition.z, 1.0f),
        camera.GetViewProjection());
    const float clipW = DirectX::XMVectorGetW(clip);
    if (!std::isfinite(clipW) || clipW <= 1.0e-5f) {
        return false;
    }
    const float ndcX = DirectX::XMVectorGetX(clip) / clipW;
    const float ndcY = DirectX::XMVectorGetY(clip) / clipW;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || std::abs(ndcX) > 10000.0f ||
        std::abs(ndcY) > 10000.0f) {
        return false;
    }
    screenPosition = {imageMin.x + (ndcX * 0.5f + 0.5f) * width,
                      imageMin.y + (0.5f - ndcY * 0.5f) * height};
    return !requireInside || (screenPosition.x >= imageMin.x && screenPosition.x <= imageMax.x &&
                              screenPosition.y >= imageMin.y && screenPosition.y <= imageMax.y);
}

inline bool TryGetCameraPreviewRect(const ImVec2& imageMin, const ImVec2& imageMax,
                                    ImVec2& previewMin, ImVec2& previewMax) {
    const float availableWidth = imageMax.x - imageMin.x;
    const float availableHeight = imageMax.y - imageMin.y;
    if (availableWidth < 360.0f || availableHeight < 240.0f) {
        return false;
    }
    constexpr float margin = 12.0f;
    const float width = (std::min)(240.0f, availableWidth * 0.36f);
    const float height = width * 9.0f / 16.0f;
    previewMax = {imageMax.x - margin, imageMin.y + margin + height};
    previewMin = {previewMax.x - width, imageMin.y + margin};
    return true;
}

inline DirectX::XMFLOAT3 CalculateScenePlacementPosition(const Camera& camera,
                                                         const ImVec2& imageMin,
                                                         const ImVec2& imageMax,
                                                         const ImVec2& screenPosition) {
    DirectX::XMVECTOR rayOrigin{};
    DirectX::XMVECTOR rayDirection{};
    DirectX::XMFLOAT3 position{};
    if (!BuildSceneRay(camera, imageMin, imageMax, screenPosition, rayOrigin, rayDirection)) {
        return position;
    }
    DirectX::XMFLOAT3 origin{};
    DirectX::XMFLOAT3 direction{};
    DirectX::XMStoreFloat3(&origin, rayOrigin);
    DirectX::XMStoreFloat3(&direction, rayDirection);
    float distance = 5.0f;
    if (std::abs(direction.y) > 1.0e-5f) {
        const float groundDistance = -origin.y / direction.y;
        if (groundDistance >= 0.0f) {
            distance = groundDistance;
        }
    }
    DirectX::XMStoreFloat3(
        &position, DirectX::XMVectorAdd(rayOrigin, DirectX::XMVectorScale(rayDirection, distance)));
    position.y = 0.0f;
    return position;
}

} // namespace EditorSceneViewportUtils
