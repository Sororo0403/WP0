#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace {
bool IsFinite(const DirectX::XMFLOAT3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void SetError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}
} // namespace

EntityId World::CreateEntity(std::string name) {
    EntityId id{};
    do {
        id = EntityId::New();
    } while (!id.IsValid() || Contains(id));

    WorldEntity entity{};
    entity.id = id;
    entity.name = name.empty() ? "Entity" : std::move(name);
    entities_.push_back(std::move(entity));
    return id;
}

bool World::DestroyEntity(EntityId id) {
    if (!Contains(id)) {
        return false;
    }
    std::unordered_set<EntityId, EntityIdHash> removed;
    for (const WorldEntity& entity : entities_) {
        if (entity.id == id || IsDescendantOf(entity.id, id)) {
            removed.insert(entity.id);
        }
    }
    std::erase_if(entities_, [&removed](const WorldEntity& entity) {
        return removed.contains(entity.id);
    });
    return true;
}

bool World::SetParent(EntityId child, EntityId parent) {
    WorldEntity* childEntity = Find(child);
    if (childEntity == nullptr || child == parent || (parent.IsValid() && !Contains(parent)) ||
        (parent.IsValid() && IsDescendantOf(parent, child))) {
        return false;
    }
    childEntity->parent = parent;
    return true;
}

WorldEntity* World::Find(EntityId id) {
    const auto found = std::ranges::find(entities_, id, &WorldEntity::id);
    return found == entities_.end() ? nullptr : &*found;
}

const WorldEntity* World::Find(EntityId id) const {
    const auto found = std::ranges::find(entities_, id, &WorldEntity::id);
    return found == entities_.end() ? nullptr : &*found;
}

bool World::Contains(EntityId id) const {
    return Find(id) != nullptr;
}

std::vector<EntityId> World::GetRootEntities() const {
    std::vector<EntityId> result;
    for (const WorldEntity& entity : entities_) {
        if (!entity.parent.IsValid()) {
            result.push_back(entity.id);
        }
    }
    return result;
}

std::vector<EntityId> World::GetChildren(EntityId parent) const {
    std::vector<EntityId> result;
    for (const WorldEntity& entity : entities_) {
        if (entity.parent == parent) {
            result.push_back(entity.id);
        }
    }
    return result;
}

const std::vector<WorldEntity>& World::Entities() const {
    return entities_;
}

bool World::Empty() const {
    return entities_.empty();
}

void World::Clear() {
    entities_.clear();
}

bool World::ReplaceEntities(std::vector<WorldEntity> entities, std::string* error) {
    std::unordered_set<EntityId, EntityIdHash> ids;
    ids.reserve(entities.size());
    for (WorldEntity& entity : entities) {
        if (!entity.id.IsValid() || !ids.insert(entity.id).second) {
            SetError(error, "Scene contains an invalid or duplicate entity id.");
            return false;
        }
        if (!IsFinite(entity.transform.position) || !IsFinite(entity.transform.rotationDegrees) ||
            !IsFinite(entity.transform.scale)) {
            SetError(error, "Scene contains a non-finite transform value.");
            return false;
        }
        if (entity.name.empty()) {
            entity.name = "Entity";
        }
    }
    for (const WorldEntity& entity : entities) {
        if (entity.parent.IsValid() && !ids.contains(entity.parent)) {
            SetError(error, "Scene contains a missing parent entity.");
            return false;
        }
    }

    World candidate;
    candidate.entities_ = entities;
    for (const WorldEntity& entity : candidate.entities_) {
        if (entity.parent.IsValid() && candidate.IsDescendantOf(entity.parent, entity.id)) {
            SetError(error, "Scene hierarchy contains a cycle.");
            return false;
        }
    }
    entities_ = std::move(entities);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool World::IsDescendantOf(EntityId candidate, EntityId ancestor) const {
    EntityId current = candidate;
    for (size_t depth = 0; depth <= entities_.size(); ++depth) {
        const WorldEntity* entity = Find(current);
        if (entity == nullptr || !entity->parent.IsValid()) {
            return false;
        }
        if (entity->parent == ancestor) {
            return true;
        }
        current = entity->parent;
    }
    return true;
}
