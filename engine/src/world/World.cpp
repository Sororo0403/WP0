#include "world/World.h"
#include "internal/WorldEntityValidation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
bool IsFinite(const DirectX::XMFLOAT2& value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

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

using EntityIdMap = std::unordered_map<EntityId, EntityId, EntityIdHash>;

void ResetDuplicatedRuntimeState(WorldEntity& entity) {
    if (entity.audioSource) {
        entity.audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
        entity.audioSource->pendingOneShots = 0u;
        entity.audioSource->runtimePlaying = false;
    }
    if (entity.animator) {
        entity.animator->runtimeCommand = AnimatorComponent::RuntimeCommand::None;
        entity.animator->runtimeRequestedClip.clear();
        entity.animator->runtimeClip.clear();
        entity.animator->runtimeLoop = true;
        entity.animator->runtimeFadeDuration = 0.0f;
        entity.animator->runtimePlaying = false;
        entity.animator->runtimeFinished = false;
        entity.animator->runtimeTime = 0.0f;
        entity.animator->runtimeDuration = 0.0f;
        entity.animator->runtimeNormalizedTime = 0.0f;
        entity.animator->runtimeTransitioning = false;
        entity.animator->runtimeTransitionProgress = 0.0f;
    }
}

void CopyDuplicatedComponents(const WorldEntity& source, WorldEntity& destination) {
    destination.active = source.active;
    destination.layer = source.layer;
    destination.transform = source.transform;
    destination.meshRenderer = source.meshRenderer;
    destination.materialOverride = source.materialOverride;
    destination.camera = source.camera;
    destination.light = source.light;
    destination.audioSource = source.audioSource;
    destination.audioListener = source.audioListener;
    destination.animator = source.animator;
    destination.canvas = source.canvas;
    destination.canvasGroup = source.canvasGroup;
    destination.eventSystem = source.eventSystem;
    destination.text = source.text;
    destination.image = source.image;
    destination.button = source.button;
    destination.toggle = source.toggle;
    destination.slider = source.slider;
    destination.dropdown = source.dropdown;
    destination.inputField = source.inputField;
    destination.scripts = source.scripts;
    destination.boxCollider = source.boxCollider;
    destination.characterController = source.characterController;
    ResetDuplicatedRuntimeState(destination);
    if (destination.camera) {
        destination.camera->primary = false;
    }
}

void RemapReference(const EntityIdMap& ids, EntityId& target,
                    bool clearExternalReference) {
    const auto remapped = ids.find(target);
    if (remapped != ids.end()) {
        target = remapped->second;
    } else if (clearExternalReference && target.IsValid()) {
        target = {};
    }
}

void RemapComponentReferences(WorldEntity& entity, const EntityIdMap& ids,
                              bool clearExternalReferences) {
    for (BehaviorComponent& script : entity.scripts) {
        for (ScriptPropertyValue& property : script.properties) {
            if (property.type == ScriptPropertyType::Entity) {
                RemapReference(ids, property.entityValue, clearExternalReferences);
            }
        }
    }
    if (entity.button) {
        RemapReference(ids, entity.button->selectOnLeft, clearExternalReferences);
        RemapReference(ids, entity.button->selectOnRight, clearExternalReferences);
        RemapReference(ids, entity.button->selectOnUp, clearExternalReferences);
        RemapReference(ids, entity.button->selectOnDown, clearExternalReferences);
    }
    if (entity.eventSystem) {
        RemapReference(ids, entity.eventSystem->firstSelected,
                       clearExternalReferences);
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
    std::ranges::copy_if(entities_, std::back_inserter(originals),
                         [this, source](const WorldEntity& entity) {
                             return entity.id != source &&
                                    IsDescendantOf(entity.id, source);
                         });

    EntityIdMap duplicateIds;
    duplicateIds.reserve(originals.size());
    for (const WorldEntity& original : originals) {
        const EntityId duplicateId = CreateEntity(original.name);
        duplicateIds.emplace(original.id, duplicateId);
        WorldEntity* duplicate = Find(duplicateId);
        CopyDuplicatedComponents(original, *duplicate);
    }

    for (const WorldEntity& original : originals) {
        WorldEntity* duplicate = Find(duplicateIds.at(original.id));
        if (original.id == source) {
            duplicate->name += " Copy";
            duplicate->parent = original.parent;
        } else {
            duplicate->parent = duplicateIds.at(original.parent);
        }
        RemapComponentReferences(*duplicate, duplicateIds, false);
    }
    return duplicateIds.at(source);
}

bool World::InstantiateEntityHierarchies(const World& source, EntityId parent,
                                         std::vector<EntityId>& roots,
                                         std::string* error) {
    roots.clear();
    if (source.Empty()) {
        SetError(error, "Entity template is empty.");
        return false;
    }
    if (parent.IsValid() && !Contains(parent)) {
        SetError(error, "Entity template parent does not exist.");
        return false;
    }

    const std::vector<WorldEntity> sourceEntities = source.entities_;
    std::unordered_set<EntityId, EntityIdHash> usedIds;
    usedIds.reserve(entities_.size() + sourceEntities.size());
    for (const WorldEntity& entity : entities_) {
        usedIds.insert(entity.id);
    }
    EntityIdMap instantiatedIds;
    instantiatedIds.reserve(sourceEntities.size());
    for (const WorldEntity& sourceEntity : sourceEntities) {
        EntityId instantiated{};
        do {
            instantiated = EntityId::New();
        } while (!instantiated.IsValid() || usedIds.contains(instantiated));
        usedIds.insert(instantiated);
        instantiatedIds.emplace(sourceEntity.id, instantiated);
    }

    std::vector<WorldEntity> combined = entities_;
    combined.reserve(combined.size() + sourceEntities.size());
    std::vector<EntityId> instantiatedRoots;
    for (const WorldEntity& sourceEntity : sourceEntities) {
        WorldEntity instantiated = sourceEntity;
        instantiated.id = instantiatedIds.at(sourceEntity.id);
        if (sourceEntity.parent.IsValid()) {
            const auto mappedParent = instantiatedIds.find(sourceEntity.parent);
            if (mappedParent == instantiatedIds.end()) {
                SetError(error, "Entity template contains an external parent.");
                return false;
            }
            instantiated.parent = mappedParent->second;
        } else {
            instantiated.parent = parent;
            instantiatedRoots.push_back(instantiated.id);
        }
        if (instantiated.camera) {
            instantiated.camera->primary = false;
        }
        RemapComponentReferences(instantiated, instantiatedIds, true);
        combined.push_back(std::move(instantiated));
    }
    if (instantiatedRoots.empty() || !ReplaceEntities(std::move(combined), error)) {
        if (instantiatedRoots.empty()) {
            SetError(error, "Entity template has no hierarchy root.");
        }
        return false;
    }
    roots = std::move(instantiatedRoots);
    return true;
}

bool World::SetPrimaryCamera(EntityId id) {
    const WorldEntity* target = Find(id);
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
    for (WorldEntity& entity : entities_) {
        for (BehaviorComponent& script : entity.scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type == ScriptPropertyType::Entity &&
                    removed.contains(property.entityValue)) {
                    property.entityValue = {};
                }
            }
        }
        if (entity.button) {
            const auto clearRemovedNavigation =
                [&](EntityId& target) {
                    if (removed.contains(target)) {
                        target = {};
                    }
                };
            clearRemovedNavigation(entity.button->selectOnLeft);
            clearRemovedNavigation(entity.button->selectOnRight);
            clearRemovedNavigation(entity.button->selectOnUp);
            clearRemovedNavigation(entity.button->selectOnDown);
        }
        if (entity.eventSystem &&
            removed.contains(entity.eventSystem->firstSelected)) {
            entity.eventSystem->firstSelected = {};
        }
    }
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

bool World::PlayAudioSource(EntityId id) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->audioSource || !entity->audioSource->enabled ||
        entity->audioSource->clipPath.empty() || !IsActiveInHierarchy(id)) {
        return false;
    }
    entity->audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::Play;
    return true;
}

bool World::PlayAudioSourceOneShot(EntityId id) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->audioSource || !entity->audioSource->enabled ||
        entity->audioSource->clipPath.empty() || !IsActiveInHierarchy(id) ||
        entity->audioSource->pendingOneShots >= AudioSourceComponent::kMaxOneShotVoices) {
        return false;
    }
    ++entity->audioSource->pendingOneShots;
    return true;
}

bool World::StopAudioSource(EntityId id) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->audioSource) {
        return false;
    }
    entity->audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::Stop;
    entity->audioSource->pendingOneShots = 0u;
    return true;
}

bool World::IsAudioSourcePlaying(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->audioSource && entity->audioSource->runtimePlaying;
}

bool World::PlayAnimation(EntityId id, std::string clip, bool loop) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->animator || !entity->animator->enabled || clip.empty() ||
        clip.size() > 256u || clip.find('\0') != std::string::npos ||
        !IsActiveInHierarchy(id)) {
        return false;
    }
    entity->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::Play;
    entity->animator->runtimeRequestedClip = std::move(clip);
    entity->animator->runtimeLoop = loop;
    return true;
}

bool World::CrossFadeAnimation(EntityId id, std::string clip, float duration, bool loop) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->animator || !entity->animator->enabled || clip.empty() ||
        clip.size() > 256u || clip.find('\0') != std::string::npos ||
        !std::isfinite(duration) || duration < 0.0f || duration > 10.0f ||
        !IsActiveInHierarchy(id)) {
        return false;
    }
    entity->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::CrossFade;
    entity->animator->runtimeRequestedClip = std::move(clip);
    entity->animator->runtimeLoop = loop;
    entity->animator->runtimeFadeDuration = duration;
    return true;
}

bool World::StopAnimation(EntityId id) {
    WorldEntity* entity = Find(id);
    if (entity == nullptr || !entity->animator) {
        return false;
    }
    entity->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::Stop;
    return true;
}

bool World::IsAnimationPlaying(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->animator && entity->animator->runtimePlaying;
}

bool World::IsAnimationFinished(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->animator && entity->animator->runtimeFinished;
}

std::string World::GetCurrentAnimation(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->animator ? entity->animator->runtimeClip : std::string{};
}

float World::GetAnimationNormalizedTime(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->animator ? entity->animator->runtimeNormalizedTime : 0.0f;
}

bool World::IsAnimationTransitioning(EntityId id) const {
    const WorldEntity* entity = Find(id);
    return entity != nullptr && entity->animator && entity->animator->runtimeTransitioning;
}

bool World::IsActiveInHierarchy(EntityId id) const {
    EntityId current = id;
    for (size_t depth = 0; depth <= entities_.size(); ++depth) {
        const WorldEntity* entity = Find(current);
        if (entity == nullptr || !entity->active) {
            return false;
        }
        if (!entity->parent.IsValid()) {
            return true;
        }
        current = entity->parent;
    }
    return false;
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
    pendingSceneLoad_.reset();
}

bool World::RequestSceneLoad(std::string scene) {
    if (scene.empty() || scene.size() > 1024u ||
        scene.find('\0') != std::string::npos || pendingSceneLoad_) {
        return false;
    }
    pendingSceneLoad_ = std::move(scene);
    return true;
}

std::optional<std::string> World::ConsumeSceneLoadRequest() {
    return std::exchange(pendingSceneLoad_, std::nullopt);
}

void World::SetPhysicsSettings(const PhysicsSettings& settings) {
    physicsSettings_ = settings;
}

const PhysicsSettings& World::GetPhysicsSettings() const {
    return physicsSettings_;
}

bool World::LayersCollide(uint8_t first, uint8_t second) const {
    return physicsSettings_.LayersCollide(first, second);
}

bool World::ReplaceEntities(std::vector<WorldEntity> entities, std::string* error) {
    std::string validationError;
    if (!WorldEntityValidation::PrepareAndValidate(entities, validationError)) {
        SetError(error, validationError.c_str());
        return false;
    }

    std::unordered_set<EntityId, EntityIdHash> ids;
    ids.reserve(entities.size());
    for (const WorldEntity& entity : entities) {
        ids.insert(entity.id);
    }
    const bool hasMissingParent = std::ranges::any_of(
        entities, [&ids](const WorldEntity& entity) {
            return entity.parent.IsValid() && !ids.contains(entity.parent);
        });
    if (hasMissingParent) {
        SetError(error, "Scene contains a missing parent entity.");
        return false;
    }

    World candidate;
    candidate.entities_ = entities;
    const bool hasHierarchyCycle = std::ranges::any_of(
        candidate.entities_, [&candidate](const WorldEntity& entity) {
            return entity.parent.IsValid() &&
                   candidate.IsDescendantOf(entity.parent, entity.id);
        });
    if (hasHierarchyCycle) {
        SetError(error, "Scene hierarchy contains a cycle.");
        return false;
    }

    entities_ = std::move(entities);
    pendingSceneLoad_.reset();
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
