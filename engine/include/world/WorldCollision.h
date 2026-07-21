#pragma once

#include "collision/OBB.h"
#include "world/EntityId.h"

#include <cstdint>

class World;

[[nodiscard]] bool TryBuildWorldBoxCollider(const World& world, EntityId entity,
                                            OBB& result);

struct CharacterCapsule {
    DirectX::XMFLOAT3 center{};
    float radius = 0.5f;
    float height = 2.0f;
    float skinWidth = 0.05f;
};

enum class CharacterCollisionFlags : uint8_t {
    None = 0,
    Sides = 1 << 0,
    Above = 1 << 1,
    Below = 1 << 2,
};

struct CharacterMoveResult {
    DirectX::XMFLOAT3 appliedMotion{};
    CharacterCollisionFlags flags = CharacterCollisionFlags::None;
};

[[nodiscard]] bool TryBuildWorldCharacterCapsule(const World& world, EntityId entity,
                                                  CharacterCapsule& result);

// Moves an entity through its CharacterController. The requested motion is in world space.
[[nodiscard]] CharacterMoveResult MoveCharacterController(
    World& world, EntityId entity, const DirectX::XMFLOAT3& motion);
