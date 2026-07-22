#include "world/WorldCollision.h"

#include "world/World.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMinimumBlockingPenetration = 1.0e-5f;
constexpr int kMaximumMovementSteps = 256;

float SquaredDistanceToBox(const DirectX::XMFLOAT3& point,
                           const DirectX::XMFLOAT3& halfSize) {
    const float dx = (std::max)(std::abs(point.x) - halfSize.x, 0.0f);
    const float dy = (std::max)(std::abs(point.y) - halfSize.y, 0.0f);
    const float dz = (std::max)(std::abs(point.z) - halfSize.z, 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

float SquaredDistanceSegmentToBox(const DirectX::XMFLOAT3& start,
                                  const DirectX::XMFLOAT3& end,
                                  const DirectX::XMFLOAT3& halfSize) {
    const auto sample = [&](float t) {
        const DirectX::XMFLOAT3 point{
            start.x + (end.x - start.x) * t,
            start.y + (end.y - start.y) * t,
            start.z + (end.z - start.z) * t,
        };
        return SquaredDistanceToBox(point, halfSize);
    };

    // Point-to-box distance along a segment is convex. Ternary search is stable here and
    // keeps the capsule query independent from the box orientation.
    float low = 0.0f;
    float high = 1.0f;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float first = low + (high - low) / 3.0f;
        const float second = high - (high - low) / 3.0f;
        if (sample(first) < sample(second)) {
            high = second;
        } else {
            low = first;
        }
    }
    return (std::min)({sample(0.0f), sample(1.0f), sample((low + high) * 0.5f)});
}

bool CapsuleOverlapsBox(const CharacterCapsule& capsule, const OBB& box) {
    using namespace DirectX;
    const float segmentHalfHeight = (std::max)(0.0f, capsule.height * 0.5f - capsule.radius);
    const XMVECTOR center = XMLoadFloat3(&capsule.center);
    const XMVECTOR start = center + XMVectorSet(0.0f, -segmentHalfHeight, 0.0f, 0.0f);
    const XMVECTOR end = center + XMVectorSet(0.0f, segmentHalfHeight, 0.0f, 0.0f);
    const XMVECTOR boxCenter = XMLoadFloat3(&box.center);
    const XMVECTOR inverseRotation = XMQuaternionInverse(XMLoadFloat4(&box.rotation));
    XMFLOAT3 localStart{};
    XMFLOAT3 localEnd{};
    XMStoreFloat3(&localStart, XMVector3Rotate(start - boxCenter, inverseRotation));
    XMStoreFloat3(&localEnd, XMVector3Rotate(end - boxCenter, inverseRotation));
    const XMFLOAT3 halfSize{box.size.x * 0.5f, box.size.y * 0.5f,
                            box.size.z * 0.5f};
    const float effectiveRadius = (std::max)(0.0f, capsule.radius - capsule.skinWidth);
    return SquaredDistanceSegmentToBox(localStart, localEnd, halfSize) <
           effectiveRadius * effectiveRadius - kMinimumBlockingPenetration;
}

bool HasBlockingOverlap(const World& world, EntityId movingEntity) {
    CharacterCapsule movingCapsule{};
    if (!TryBuildWorldCharacterCapsule(world, movingEntity, movingCapsule)) {
        return false;
    }

    for (const WorldEntity& other : world.Entities()) {
        if (other.id == movingEntity || !other.boxCollider || !other.boxCollider->enabled ||
            other.boxCollider->isTrigger) {
            continue;
        }

        OBB otherCollider{};
        if (!TryBuildWorldBoxCollider(world, other.id, otherCollider)) {
            continue;
        }
        if (CapsuleOverlapsBox(movingCapsule, otherCollider)) {
            return true;
        }
    }
    return false;
}

DirectX::XMFLOAT3 WorldMotionToLocal(const World& world, const WorldEntity& entity,
                                     const DirectX::XMFLOAT3& motion) {
    if (!entity.parent.IsValid()) {
        return motion;
    }
    DirectX::XMFLOAT4X4 parentWorld{};
    if (!world.TryGetWorldMatrix(entity.parent, parentWorld)) {
        return {};
    }
    DirectX::XMVECTOR determinant{};
    const DirectX::XMMATRIX inverse =
        DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&parentWorld));
    if (std::abs(DirectX::XMVectorGetX(determinant)) <= 1.0e-8f) {
        return {};
    }
    DirectX::XMFLOAT3 local{};
    DirectX::XMStoreFloat3(
        &local, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&motion), inverse));
    return local;
}

void ApplyWorldMotion(const World& world, WorldEntity& entity,
                      const DirectX::XMFLOAT3& motion) {
    const DirectX::XMFLOAT3 local = WorldMotionToLocal(world, entity, motion);
    entity.transform.position.x += local.x;
    entity.transform.position.y += local.y;
    entity.transform.position.z += local.z;
}

CharacterCollisionFlags Combine(CharacterCollisionFlags left,
                                CharacterCollisionFlags right) {
    return static_cast<CharacterCollisionFlags>(static_cast<uint8_t>(left) |
                                                static_cast<uint8_t>(right));
}
} // namespace

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

bool TryBuildWorldCharacterCapsule(const World& world, EntityId entityId,
                                   CharacterCapsule& result) {
    const WorldEntity* entity = world.Find(entityId);
    if (entity == nullptr || !entity->characterController) {
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
    DirectX::XMFLOAT3 storedScale{};
    DirectX::XMStoreFloat3(&storedScale, scale);
    const CharacterControllerComponent& controller = *entity->characterController;
    DirectX::XMStoreFloat3(
        &result.center,
        DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&controller.center),
                                         worldMatrix));
    const float radialScale = (std::max)(std::abs(storedScale.x), std::abs(storedScale.z));
    result.radius = controller.radius * radialScale;
    result.height = (std::max)(controller.height * std::abs(storedScale.y),
                               result.radius * 2.0f);
    result.skinWidth = controller.skinWidth * radialScale;
    return std::isfinite(result.center.x) && std::isfinite(result.center.y) &&
           std::isfinite(result.center.z) && std::isfinite(result.radius) &&
           std::isfinite(result.height) && std::isfinite(result.skinWidth) &&
           result.radius > 0.0f && result.height >= result.radius * 2.0f;
}

bool CheckCharacterControllerBoxOverlap(const World& world, EntityId characterEntity,
                                        EntityId boxEntity) {
    CharacterCapsule capsule{};
    OBB box{};
    return TryBuildWorldCharacterCapsule(world, characterEntity, capsule) &&
           TryBuildWorldBoxCollider(world, boxEntity, box) &&
           CapsuleOverlapsBox(capsule, box);
}

CharacterMoveResult MoveCharacterController(
    World& world, EntityId entityId, const DirectX::XMFLOAT3& motion) {
    CharacterMoveResult result{};
    WorldEntity* entity = world.Find(entityId);
    if (entity == nullptr || !std::isfinite(motion.x) || !std::isfinite(motion.y) ||
        !std::isfinite(motion.z)) {
        return result;
    }

    const CharacterControllerComponent* controller =
        entity->characterController ? &*entity->characterController : nullptr;
    if (controller == nullptr || !controller->enabled) {
        return result;
    }
    const float motionLength = std::sqrt(motion.x * motion.x + motion.y * motion.y +
                                         motion.z * motion.z);
    if (motionLength < controller->minMoveDistance) {
        return result;
    }

    const float longestAxis =
        (std::max)({std::abs(motion.x), std::abs(motion.y), std::abs(motion.z)});
    const float maximumStep = (std::clamp)(controller->radius * 0.25f, 0.001f, 0.1f);
    const int stepCount = (std::clamp)(
        static_cast<int>(std::ceil(longestAxis / maximumStep)), 1, kMaximumMovementSteps);
    const DirectX::XMFLOAT3 step{motion.x / static_cast<float>(stepCount),
                                 motion.y / static_cast<float>(stepCount),
                                 motion.z / static_cast<float>(stepCount)};

    const auto tryAxis = [&](const DirectX::XMFLOAT3& axisMotion,
                             CharacterCollisionFlags collisionFlag) {
        const float amount = std::abs(axisMotion.x) + std::abs(axisMotion.y) +
                             std::abs(axisMotion.z);
        if (amount == 0.0f) {
            return;
        }
        const DirectX::XMFLOAT3 previousPosition = entity->transform.position;
        ApplyWorldMotion(world, *entity, axisMotion);
        if (HasBlockingOverlap(world, entityId)) {
            entity->transform.position = previousPosition;
            result.flags = Combine(result.flags, collisionFlag);
            return;
        }
        result.appliedMotion.x += axisMotion.x;
        result.appliedMotion.y += axisMotion.y;
        result.appliedMotion.z += axisMotion.z;
    };

    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex) {
        tryAxis({step.x, 0.0f, 0.0f}, CharacterCollisionFlags::Sides);
        tryAxis({0.0f, step.y, 0.0f},
                step.y < 0.0f ? CharacterCollisionFlags::Below
                              : CharacterCollisionFlags::Above);
        tryAxis({0.0f, 0.0f, step.z}, CharacterCollisionFlags::Sides);
    }
    return result;
}
