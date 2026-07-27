#include "world/World.h"

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
        duplicate->active = original.active;
        duplicate->layer = original.layer;
        duplicate->transform = original.transform;
        duplicate->meshRenderer = original.meshRenderer;
        duplicate->materialOverride = original.materialOverride;
        duplicate->camera = original.camera;
        duplicate->light = original.light;
        duplicate->audioSource = original.audioSource;
        duplicate->audioListener = original.audioListener;
        duplicate->animator = original.animator;
        duplicate->canvas = original.canvas;
        duplicate->text = original.text;
        duplicate->image = original.image;
        duplicate->button = original.button;
        if (duplicate->audioSource) {
            duplicate->audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
            duplicate->audioSource->pendingOneShots = 0u;
            duplicate->audioSource->runtimePlaying = false;
        }
        if (duplicate->animator) {
            duplicate->animator->runtimeCommand = AnimatorComponent::RuntimeCommand::None;
            duplicate->animator->runtimeRequestedClip.clear();
            duplicate->animator->runtimeClip.clear();
            duplicate->animator->runtimeLoop = true;
            duplicate->animator->runtimeFadeDuration = 0.0f;
            duplicate->animator->runtimePlaying = false;
            duplicate->animator->runtimeFinished = false;
            duplicate->animator->runtimeTime = 0.0f;
            duplicate->animator->runtimeDuration = 0.0f;
            duplicate->animator->runtimeNormalizedTime = 0.0f;
            duplicate->animator->runtimeTransitioning = false;
            duplicate->animator->runtimeTransitionProgress = 0.0f;
        }
        duplicate->scripts = original.scripts;
        duplicate->boxCollider = original.boxCollider;
        duplicate->characterController = original.characterController;
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
        for (BehaviorComponent& script : duplicate->scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type != ScriptPropertyType::Entity) {
                    continue;
                }
                const auto remapped = duplicateIds.find(property.entityValue);
                if (remapped != duplicateIds.end()) {
                    property.entityValue = remapped->second;
                }
            }
        }
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
    std::unordered_map<EntityId, EntityId, EntityIdHash> instantiatedIds;
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
        for (BehaviorComponent& script : instantiated.scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type != ScriptPropertyType::Entity ||
                    !property.entityValue.IsValid()) {
                    continue;
                }
                const auto mapped = instantiatedIds.find(property.entityValue);
                property.entityValue = mapped != instantiatedIds.end()
                                           ? mapped->second
                                           : EntityId{};
            }
        }
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
    for (WorldEntity& entity : entities_) {
        for (BehaviorComponent& script : entity.scripts) {
            for (ScriptPropertyValue& property : script.properties) {
                if (property.type == ScriptPropertyType::Entity &&
                    removed.contains(property.entityValue)) {
                    property.entityValue = {};
                }
            }
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
    std::optional<std::string> request = std::move(pendingSceneLoad_);
    pendingSceneLoad_.reset();
    return request;
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
    std::unordered_set<EntityId, EntityIdHash> ids;
    ids.reserve(entities.size());
    for (WorldEntity& entity : entities) {
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
        if (!entity.id.IsValid() || !ids.insert(entity.id).second) {
            SetError(error, "Scene contains an invalid or duplicate entity id.");
            return false;
        }
        if (entity.layer >= PhysicsSettings::kLayerCount) {
            SetError(error, "Scene contains an invalid Entity Layer.");
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
                material.baseColorTexturePath.find('\0') != std::string::npos ||
                material.normalTexturePath.size() > 1024u ||
                material.normalTexturePath.find('\0') != std::string::npos ||
                !std::isfinite(material.normalStrength) || material.normalStrength < 0.0f ||
                material.roughnessTexturePath.size() > 1024u ||
                material.roughnessTexturePath.find('\0') != std::string::npos ||
                material.metallicTexturePath.size() > 1024u ||
                material.metallicTexturePath.find('\0') != std::string::npos ||
                material.pbrTexturePacking < MaterialPbrTexturePacking::Separate ||
                material.pbrTexturePacking > MaterialPbrTexturePacking::MetallicRoughness ||
                material.blendMode < MaterialSurfaceBlendMode::Opaque ||
                material.blendMode > MaterialSurfaceBlendMode::Transparent ||
                !std::isfinite(material.alphaCutoff) || material.alphaCutoff < 0.0f ||
                material.alphaCutoff > 1.0f || material.cullMode < MaterialSurfaceCullMode::None ||
                material.cullMode > MaterialSurfaceCullMode::Back) {
                SetError(error, "Scene contains an invalid MaterialOverride component.");
                return false;
            }
        }
        if (entity.animator) {
            const AnimatorComponent& animator = *entity.animator;
            if (animator.clip.size() > 256u || animator.clip.find('\0') != std::string::npos ||
                !std::isfinite(animator.speed) || animator.speed < 0.0f ||
                animator.speed > 100.0f) {
                SetError(error, "Scene contains an invalid Animator component.");
                return false;
            }
        }
        if (entity.audioSource) {
            const AudioSourceComponent& source = *entity.audioSource;
            if (source.clipPath.size() > 1024u ||
                source.clipPath.find('\0') != std::string::npos ||
                !std::isfinite(source.volume) || source.volume < 0.0f ||
                source.volume > 1.0f || !std::isfinite(source.pitch) ||
                source.pitch < AudioSourceComponent::kMinPitch ||
                source.pitch > AudioSourceComponent::kMaxPitch ||
                !std::isfinite(source.minDistance) ||
                !std::isfinite(source.maxDistance) || source.minDistance < 0.0f ||
                source.maxDistance <= source.minDistance) {
                SetError(error, "Scene contains an invalid AudioSource component.");
                return false;
            }
        }
        if (entity.canvas) {
            const CanvasComponent& canvas = *entity.canvas;
            if (!IsFinite(canvas.referenceResolution) ||
                canvas.referenceResolution.x < 1.0f ||
                canvas.referenceResolution.y < 1.0f ||
                canvas.referenceResolution.x > 16384.0f ||
                canvas.referenceResolution.y > 16384.0f ||
                canvas.sortingOrder < -1000000 ||
                canvas.sortingOrder > 1000000) {
                SetError(error, "Scene contains an invalid Canvas component.");
                return false;
            }
        }
        if (entity.text) {
            const TextComponent& text = *entity.text;
            if (text.text.size() > 4096u ||
                text.text.find('\0') != std::string::npos ||
                text.fontPath.size() > 1024u ||
                text.fontPath.find('\0') != std::string::npos ||
                !IsFinite(text.position) || std::abs(text.position.x) > 1000000.0f ||
                std::abs(text.position.y) > 1000000.0f ||
                !std::isfinite(text.fontSize) || text.fontSize < 1.0f ||
                text.fontSize > 512.0f ||
                !std::isfinite(text.lineSpacing) ||
                text.lineSpacing < 0.0f || text.lineSpacing > 512.0f ||
                !std::isfinite(text.wrapWidth) ||
                text.wrapWidth < 0.0f || text.wrapWidth > 16384.0f ||
                !IsFinite(text.color) ||
                text.color.x < 0.0f || text.color.x > 1.0f ||
                text.color.y < 0.0f || text.color.y > 1.0f ||
                text.color.z < 0.0f || text.color.z > 1.0f ||
                text.color.w < 0.0f || text.color.w > 1.0f ||
                text.alignment < TextAlignment::Left ||
                text.alignment > TextAlignment::Right ||
                text.anchor < UiAnchor::TopLeft ||
                text.anchor > UiAnchor::BottomRight) {
                SetError(error, "Scene contains an invalid Text component.");
                return false;
            }
        }
        if (entity.image) {
            const ImageComponent& image = *entity.image;
            if (image.texturePath.size() > 1024u ||
                image.texturePath.find('\0') != std::string::npos ||
                !IsFinite(image.position) || !IsFinite(image.size) ||
                !IsFinite(image.pivot) ||
                image.size.x < 0.0f || image.size.y < 0.0f ||
                image.size.x > 1000000.0f || image.size.y > 1000000.0f ||
                std::abs(image.position.x) > 1000000.0f ||
                std::abs(image.position.y) > 1000000.0f ||
                image.pivot.x < 0.0f || image.pivot.x > 1.0f ||
                image.pivot.y < 0.0f || image.pivot.y > 1.0f ||
                !IsFinite(image.color) || image.color.x < 0.0f ||
                image.color.x > 1.0f || image.color.y < 0.0f ||
                image.color.y > 1.0f || image.color.z < 0.0f ||
                image.color.z > 1.0f || image.color.w < 0.0f ||
                image.color.w > 1.0f ||
                image.anchor < UiAnchor::TopLeft ||
                image.anchor > UiAnchor::BottomRight ||
                image.type < ImageType::Simple ||
                image.type > ImageType::Filled ||
                image.fillMethod < ImageFillMethod::Horizontal ||
                image.fillMethod > ImageFillMethod::Vertical ||
                !std::isfinite(image.fillAmount) ||
                image.fillAmount < 0.0f || image.fillAmount > 1.0f) {
                SetError(error, "Scene contains an invalid Image component.");
                return false;
            }
        }
        if (entity.button) {
            const ButtonComponent& button = *entity.button;
            const auto validColor = [](const DirectX::XMFLOAT4& color) {
                return IsFinite(color) && color.x >= 0.0f && color.x <= 1.0f &&
                       color.y >= 0.0f && color.y <= 1.0f &&
                       color.z >= 0.0f && color.z <= 1.0f &&
                       color.w >= 0.0f && color.w <= 1.0f;
            };
            if (!validColor(button.normalColor) ||
                !validColor(button.hoveredColor) ||
                !validColor(button.pressedColor) ||
                !validColor(button.disabledColor) ||
                !std::isfinite(button.fadeDuration) ||
                button.fadeDuration < 0.0f ||
                button.fadeDuration > 10.0f) {
                SetError(error, "Scene contains an invalid Button component.");
                return false;
            }
        }
        for (const BehaviorComponent& script : entity.scripts) {
            if (script.type.size() > 128u ||
                script.type.find('\0') != std::string::npos ||
                script.scriptAssetPath.size() > 1024u ||
                script.scriptAssetPath.find('\0') != std::string::npos ||
                (script.type.empty() &&
                 (!script.scriptAssetPath.empty() || !script.properties.empty()))) {
                SetError(error, "Scene contains an invalid Script component.");
                return false;
            }
            if (script.properties.size() > 128u) {
                SetError(error, "Scene contains too many Script properties.");
                return false;
            }
            std::unordered_set<std::string> propertyNames;
            for (const ScriptPropertyValue& property : script.properties) {
                if (property.name.empty() || property.name.size() > 128u ||
                    property.name.find('\0') != std::string::npos ||
                    !propertyNames.insert(property.name).second ||
                    property.type < ScriptPropertyType::Float ||
                    property.type > ScriptPropertyType::Scene ||
                    (property.type == ScriptPropertyType::Float &&
                     !std::isfinite(property.floatValue)) ||
                    (property.type == ScriptPropertyType::Vector3 &&
                     (!std::isfinite(property.vector3Value.x) ||
                      !std::isfinite(property.vector3Value.y) ||
                      !std::isfinite(property.vector3Value.z))) ||
                    ((property.type == ScriptPropertyType::String ||
                      property.type == ScriptPropertyType::AnimationClip ||
                      property.type == ScriptPropertyType::Scene) &&
                     (property.stringValue.size() > 1024u ||
                      property.stringValue.find('\0') != std::string::npos)) ||
                    (property.type == ScriptPropertyType::InputAction &&
                     (property.stringValue.size() > 64u ||
                      property.stringValue.find('\0') != std::string::npos))) {
                    SetError(error, "Scene contains an invalid Script property.");
                    return false;
                }
            }
        }
        if (entity.boxCollider) {
            const BoxColliderComponent& collider = *entity.boxCollider;
            if (!IsFinite(collider.center) || !IsFinite(collider.size) ||
                collider.size.x < 0.001f || collider.size.y < 0.001f ||
                collider.size.z < 0.001f || collider.size.x > 1000000.0f ||
                collider.size.y > 1000000.0f || collider.size.z > 1000000.0f) {
                SetError(error, "Scene contains an invalid BoxCollider component.");
                return false;
            }
        }
        if (entity.characterController) {
            const CharacterControllerComponent& controller = *entity.characterController;
            if (!IsFinite(controller.center) || !std::isfinite(controller.radius) ||
                !std::isfinite(controller.height) ||
                !std::isfinite(controller.slopeLimitDegrees) ||
                !std::isfinite(controller.stepOffset) ||
                !std::isfinite(controller.skinWidth) ||
                !std::isfinite(controller.minMoveDistance) || controller.radius < 0.001f ||
                controller.height < controller.radius * 2.0f ||
                controller.slopeLimitDegrees < 0.0f ||
                controller.slopeLimitDegrees > 90.0f || controller.stepOffset < 0.0f ||
                controller.stepOffset > controller.height || controller.skinWidth < 0.0f ||
                controller.skinWidth >= controller.radius || controller.minMoveDistance < 0.0f) {
                SetError(error, "Scene contains an invalid CharacterController component.");
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
