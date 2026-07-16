#pragma once

#include "world/EntityId.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

struct TransformComponent {
    DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotationDegrees{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldEntity {
    EntityId id{};
    EntityId parent{};
    std::string name = "Entity";
    TransformComponent transform{};
};

class World {
public:
    EntityId CreateEntity(std::string name = "Entity");
    bool DestroyEntity(EntityId id);
    bool SetParent(EntityId child, EntityId parent);

    WorldEntity* Find(EntityId id);
    const WorldEntity* Find(EntityId id) const;
    bool Contains(EntityId id) const;

    [[nodiscard]] std::vector<EntityId> GetRootEntities() const;
    [[nodiscard]] std::vector<EntityId> GetChildren(EntityId parent) const;
    [[nodiscard]] const std::vector<WorldEntity>& Entities() const;
    [[nodiscard]] bool Empty() const;

    void Clear();
    bool ReplaceEntities(std::vector<WorldEntity> entities, std::string* error = nullptr);

private:
    bool IsDescendantOf(EntityId candidate, EntityId ancestor) const;

    std::vector<WorldEntity> entities_;
};
