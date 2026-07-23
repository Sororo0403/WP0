#pragma once

#include "world/PhysicsSettings.h"

#include "runtime/ScriptProperty.h"
#include "world/EntityId.h"

#include <DirectXMath.h>
#include <optional>
#include <string>
#include <vector>

struct TransformComponent {
    DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotationDegrees{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
};

enum class MeshSourceType : uint8_t {
    Primitive = 0,
    Model = 1,
};

enum class MeshPrimitive : uint8_t {
    Box = 0,
    Sphere = 1,
    Plane = 2,
    Cylinder = 3,
};

struct MeshRendererComponent {
    bool enabled = true;
    MeshSourceType sourceType = MeshSourceType::Primitive;
    MeshPrimitive primitive = MeshPrimitive::Box;
    std::string modelPath;
};

enum class MaterialPbrTexturePacking : uint8_t {
    Separate = 0,
    OcclusionRoughnessMetallic = 1,
    MetallicRoughness = 2,
};

enum class MaterialSurfaceBlendMode : uint8_t {
    Opaque = 0,
    Cutout = 1,
    Transparent = 2,
};

enum class MaterialSurfaceCullMode : uint8_t {
    None = 0,
    Front = 1,
    Back = 2,
};

struct MaterialOverrideComponent {
    bool enabled = true;
    DirectX::XMFLOAT4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    std::string baseColorTexturePath;
    std::string normalTexturePath;
    float normalStrength = 1.0f;
    std::string roughnessTexturePath;
    std::string metallicTexturePath;
    MaterialPbrTexturePacking pbrTexturePacking = MaterialPbrTexturePacking::Separate;
    MaterialSurfaceBlendMode blendMode = MaterialSurfaceBlendMode::Opaque;
    float alphaCutoff = 0.5f;
    MaterialSurfaceCullMode cullMode = MaterialSurfaceCullMode::Back;
    bool depthWrite = true;
};

enum class CameraProjection : uint8_t {
    Perspective = 0,
    Orthographic = 1,
};

struct CameraComponent {
    bool enabled = true;
    bool primary = false;
    CameraProjection projection = CameraProjection::Perspective;
    float fieldOfViewDegrees = 45.0f;
    float orthographicHeight = 10.0f;
    float nearClip = 0.1f;
    float farClip = 1000.0f;
};

enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct LightComponent {
    bool enabled = true;
    LightType type = LightType::Directional;
    DirectX::XMFLOAT3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerAngleDegrees = 25.0f;
    float outerAngleDegrees = 45.0f;
};

struct AudioSourceComponent {
    static constexpr float kMinPitch = 0.1f;
    static constexpr float kMaxPitch = 3.0f;
    static constexpr uint32_t kMaxOneShotVoices = 32u;

    enum class RuntimeCommand : uint8_t {
        None,
        Play,
        Stop,
    };

    bool enabled = true;
    std::string clipPath;
    bool playOnAwake = true;
    bool loop = false;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool spatial = false;
    float minDistance = 1.0f;
    float maxDistance = 50.0f;
    RuntimeCommand runtimeCommand = RuntimeCommand::None;
    uint32_t pendingOneShots = 0u;
    bool runtimePlaying = false;
};

struct AudioListenerComponent {
    bool enabled = true;
};

struct AnimatorComponent {
    enum class RuntimeCommand : uint8_t {
        None,
        Play,
        Stop,
    };

    bool enabled = true;
    std::string clip;
    bool playOnAwake = true;
    bool loop = true;
    float speed = 1.0f;
    RuntimeCommand runtimeCommand = RuntimeCommand::None;
    std::string runtimeClip;
    bool runtimeLoop = true;
    bool runtimePlaying = false;
    bool runtimeFinished = false;
};

struct ScriptPropertyValue {
    std::string name;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float floatValue = 0.0f;
    EntityId entityValue{};
    bool booleanValue = false;
    int32_t integerValue = 0;
    ScriptVector3 vector3Value{};
};

struct BehaviorComponent {
    bool enabled = true;
    std::string type;
    std::string scriptAssetPath;
    std::vector<ScriptPropertyValue> properties;
};

struct BoxColliderComponent {
    bool enabled = true;
    DirectX::XMFLOAT3 center{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 size{1.0f, 1.0f, 1.0f};
    bool isTrigger = false;
};

struct CharacterControllerComponent {
    bool enabled = true;
    DirectX::XMFLOAT3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.5f;
    float height = 2.0f;
    float slopeLimitDegrees = 45.0f;
    float stepOffset = 0.3f;
    float skinWidth = 0.05f;
    float minMoveDistance = 0.0f;
};

struct WorldEntity {
    EntityId id{};
    EntityId parent{};
    std::string name = "Entity";
    bool active = true;
    uint8_t layer = 0u;
    TransformComponent transform{};
    std::optional<MeshRendererComponent> meshRenderer;
    std::optional<MaterialOverrideComponent> materialOverride;
    std::optional<CameraComponent> camera;
    std::optional<LightComponent> light;
    std::optional<AudioSourceComponent> audioSource;
    std::optional<AudioListenerComponent> audioListener;
    std::optional<AnimatorComponent> animator;
    std::vector<BehaviorComponent> scripts;
    std::optional<BoxColliderComponent> boxCollider;
    std::optional<CharacterControllerComponent> characterController;
};

class World {
public:
    EntityId CreateEntity(std::string name = "Entity");
    EntityId DuplicateEntityHierarchy(EntityId source);
    bool InstantiateEntityHierarchies(const World& source, EntityId parent,
                                      std::vector<EntityId>& roots,
                                      std::string* error = nullptr);
    bool DestroyEntity(EntityId id);
    bool SetParent(EntityId child, EntityId parent);
    bool MoveEntityBefore(EntityId entity, EntityId sibling);
    bool MoveEntityAfter(EntityId entity, EntityId sibling);
    bool SetPrimaryCamera(EntityId entity);
    bool PlayAudioSource(EntityId entity);
    bool PlayAudioSourceOneShot(EntityId entity);
    bool StopAudioSource(EntityId entity);
    [[nodiscard]] bool IsAudioSourcePlaying(EntityId entity) const;
    bool PlayAnimation(EntityId entity, std::string clip, bool loop = true);
    bool StopAnimation(EntityId entity);
    [[nodiscard]] bool IsAnimationPlaying(EntityId entity) const;
    [[nodiscard]] bool IsAnimationFinished(EntityId entity) const;

    WorldEntity* Find(EntityId id);
    const WorldEntity* Find(EntityId id) const;
    bool Contains(EntityId id) const;
    [[nodiscard]] bool IsActiveInHierarchy(EntityId id) const;

    [[nodiscard]] std::vector<EntityId> GetRootEntities() const;
    [[nodiscard]] std::vector<EntityId> GetChildren(EntityId parent) const;
    [[nodiscard]] const std::vector<WorldEntity>& Entities() const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] bool TryGetWorldMatrix(EntityId id, DirectX::XMFLOAT4X4& result) const;
    void SetPhysicsSettings(const PhysicsSettings& settings);
    [[nodiscard]] const PhysicsSettings& GetPhysicsSettings() const;
    [[nodiscard]] bool LayersCollide(uint8_t first, uint8_t second) const;

    void Clear();
    bool ReplaceEntities(std::vector<WorldEntity> entities, std::string* error = nullptr);

private:
    bool IsDescendantOf(EntityId candidate, EntityId ancestor) const;

    std::vector<WorldEntity> entities_;
    PhysicsSettings physicsSettings_ = PhysicsSettings::Defaults();
};
