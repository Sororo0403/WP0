#include "world/WorldCollision.h"

#include "world/World.h"

#include <DirectXMath.h>

#include <cmath>

bool TryBuildWorldBoxCollider(const World& world, EntityId entityId, OBB& result) {
    const WorldEntity* entity = world.Find(entityId);
    if (entity == nullptr || !entity->boxCollider) {
        return false;
    }

    DirectX::XMFLOAT4X4 storedWorld{};
    if (!world.TryGetWorldMatrix(entityId, storedWorld)) {
        return false;
    }
    const DirectX::XMMATRIX worldMatrix = DirectX::XMLoadFloat4x4(&storedWorld);
    DirectX::XMVECTOR scale{};
    DirectX::XMVECTOR rotation{};
    DirectX::XMVECTOR translation{};
    if (!DirectX::XMMatrixDecompose(&scale, &rotation, &translation, worldMatrix)) {
        return false;
    }

    const BoxColliderComponent& collider = *entity->boxCollider;
    DirectX::XMStoreFloat3(
        &result.center,
        DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&collider.center),
                                         worldMatrix));
    DirectX::XMFLOAT3 storedScale{};
    DirectX::XMStoreFloat3(&storedScale, scale);
    result.size = {std::abs(collider.size.x * storedScale.x),
                   std::abs(collider.size.y * storedScale.y),
                   std::abs(collider.size.z * storedScale.z)};
    DirectX::XMStoreFloat4(&result.rotation, DirectX::XMQuaternionNormalize(rotation));
    return std::isfinite(result.center.x) && std::isfinite(result.center.y) &&
           std::isfinite(result.center.z) && std::isfinite(result.size.x) &&
           std::isfinite(result.size.y) && std::isfinite(result.size.z) &&
           result.size.x > 0.0f && result.size.y > 0.0f && result.size.z > 0.0f;
}
