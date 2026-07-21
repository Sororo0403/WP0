#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
bool IsFinite(const DirectX::XMFLOAT3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const DirectX::XMFLOAT4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
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

EntityId World::DuplicateEntityHierarchy(EntityId source) {
    const WorldEntity* sourceEntity = Find(source);
    if (sourceEntity == nullptr) {
        return {};
    }

    std::vector<WorldEntity> originals;
    originals.reserve(entities_.size());
    originals.push_back(*sourceEntity);
    for (const WorldEntity& entity : entities_) {
        if (entity.id != source && IsDescendantOf(entity.id, source)) {
            originals.push_back(entity);
        }
    }

    std::unordered_map<EntityId, EntityId, EntityIdHash> duplicateIds;
    duplicateIds.reserve(originals.size());
    for (const WorldEntity& original : originals) {
        const EntityId duplicateId = CreateEntity(original.name);
        duplicateIds.emplace(original.id, duplicateId);
        WorldEntity* duplicate = Find(duplicateId);
        duplicate->transform = original.transform;
        duplicate->meshRenderer = original.meshRenderer;
        duplicate->materialOverride = original.materialOverride;
        duplicate->camera = original.camera;
        duplicate->light = original.light;
        if (duplicate->camera) {
            duplicate->camera->primary = false;
        }
    }

    for (const WorldEntity& original : originals) {
        WorldEntity* duplicate = Find(duplicateIds.at(original.id));
        if (original.id == source) {
            duplicate->name += " Copy";
            duplicate->parent = original.parent;
        } else {
            duplicate->parent = duplicateIds.at(original.parent);
        }
    }
    return duplicateIds.at(source);
}

bool World::SetPrimaryCamera(EntityId id) {
    WorldEntity* target = Find(id);
    if (target == nullptr || !target->camera) {
        return false;
    }
    for (WorldEntity& entity : entities_) {
        if (entity.camera) {
            entity.camera->primary = entity.id == id;
        }
    }
    return true;
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

bool World::MoveEntityBefore(EntityId entity, EntityId sibling) {
    const auto entityIterator = std::ranges::find(entities_, entity, &WorldEntity::id);
    const auto siblingIterator = std::ranges::find(entities_, sibling, &WorldEntity::id);
    if (entityIterator == entities_.end() || siblingIterator == entities_.end() ||
        entityIterator == siblingIterator || entityIterator->parent != siblingIterator->parent) {
        return false;
    }
    const size_t entityIndex =
        static_cast<size_t>(std::distance(entities_.begin(), entityIterator));
    const size_t siblingIndex =
        static_cast<size_t>(std::distance(entities_.begin(), siblingIterator));
    if (entityIndex + 1u == siblingIndex) {
        return false;
    }
    if (entityIndex < siblingIndex) {
        std::rotate(entities_.begin() + entityIndex, entities_.begin() + entityIndex + 1u,
                    entities_.begin() + siblingIndex);
    } else {
        std::rotate(entities_.begin() + siblingIndex, entities_.begin() + entityIndex,
                    entities_.begin() + entityIndex + 1u);
    }
    return true;
}

bool World::MoveEntityAfter(EntityId entity, EntityId sibling) {
    const auto entityIterator = std::ranges::find(entities_, entity, &WorldEntity::id);
    const auto siblingIterator = std::ranges::find(entities_, sibling, &WorldEntity::id);
    if (entityIterator == entities_.end() || siblingIterator == entities_.end() ||
        entityIterator == siblingIterator || entityIterator->parent != siblingIterator->parent) {
        return false;
    }
    const size_t entityIndex =
        static_cast<size_t>(std::distance(entities_.begin(), entityIterator));
    const size_t siblingIndex =
        static_cast<size_t>(std::distance(entities_.begin(), siblingIterator));
    if (siblingIndex + 1u == entityIndex) {
        return false;
    }
    if (entityIndex < siblingIndex) {
        std::rotate(entities_.begin() + entityIndex, entities_.begin() + entityIndex + 1u,
                    entities_.begin() + siblingIndex + 1u);
    } else {
        std::rotate(entities_.begin() + siblingIndex + 1u, entities_.begin() + entityIndex,
                    entities_.begin() + entityIndex + 1u);
    }
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

bool World::TryGetWorldMatrix(EntityId id, DirectX::XMFLOAT4X4& result) const {
    using namespace DirectX;
    const WorldEntity* entity = Find(id);
    if (entity == nullptr) {
        return false;
    }

    XMMATRIX world = XMMatrixIdentity();
    EntityId current = id;
    for (size_t depth = 0; depth <= entities_.size(); ++depth) {
        entity = Find(current);
        if (entity == nullptr) {
            return false;
        }
        const TransformComponent& transform = entity->transform;
        const XMMATRIX local =
            XMMatrixScaling(transform.scale.x, transform.scale.y, transform.scale.z) *
            XMMatrixRotationRollPitchYaw(XMConvertToRadians(transform.rotationDegrees.x),
                                         XMConvertToRadians(transform.rotationDegrees.y),
                                         XMConvertToRadians(transform.rotationDegrees.z)) *
            XMMatrixTranslation(transform.position.x, transform.position.y, transform.position.z);
        world *= local;
        if (!entity->parent.IsValid()) {
            XMStoreFloat4x4(&result, world);
            return true;
        }
        current = entity->parent;
    }
    return false;
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
        if (entity.meshRenderer) {
            const MeshRendererComponent& renderer = *entity.meshRenderer;
            const bool validSource = renderer.sourceType == MeshSourceType::Primitive ||
                                     renderer.sourceType == MeshSourceType::Model;
            const bool validPrimitive = renderer.primitive >= MeshPrimitive::Box &&
                                        renderer.primitive <= MeshPrimitive::Cylinder;
            if (!validSource || !validPrimitive || renderer.modelPath.size() > 1024u ||
                renderer.modelPath.find('\0') != std::string::npos) {
                SetError(error, "Scene contains an invalid MeshRenderer component.");
                return false;
            }
        }
        if (entity.materialOverride) {
            const MaterialOverrideComponent& material = *entity.materialOverride;
            if (!IsFinite(material.baseColor) || !std::isfinite(material.metallic) ||
                !std::isfinite(material.roughness) || material.baseColor.x < 0.0f ||
                material.baseColor.y < 0.0f || material.baseColor.z < 0.0f ||
                material.baseColor.w < 0.0f || material.baseColor.w > 1.0f ||
                material.metallic < 0.0f || material.metallic > 1.0f ||
                material.roughness < 0.0f || material.roughness > 1.0f ||
                material.baseColorTexturePath.size() > 1024u ||
                material.baseColorTexturePath.find('\0') != std::string::npos) {
                SetError(error, "Scene contains an invalid MaterialOverride component.");
                return false;
            }
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
