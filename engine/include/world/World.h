#pragma once

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

struct WorldEntity {
    EntityId id{};
    EntityId parent{};
    std::string name = "Entity";
    TransformComponent transform{};
    std::optional<MeshRendererComponent> meshRenderer;
    std::optional<MaterialOverrideComponent> materialOverride;
    std::optional<CameraComponent> camera;
    std::optional<LightComponent> light;
};

class World {
public:
    EntityId CreateEntity(std::string name = "Entity");
    EntityId DuplicateEntityHierarchy(EntityId source);
    bool DestroyEntity(EntityId id);
    bool SetParent(EntityId child, EntityId parent);
    bool MoveEntityBefore(EntityId entity, EntityId sibling);
    bool MoveEntityAfter(EntityId entity, EntityId sibling);
    bool SetPrimaryCamera(EntityId entity);

    WorldEntity* Find(EntityId id);
    const WorldEntity* Find(EntityId id) const;
    bool Contains(EntityId id) const;

    [[nodiscard]] std::vector<EntityId> GetRootEntities() const;
    [[nodiscard]] std::vector<EntityId> GetChildren(EntityId parent) const;
    [[nodiscard]] const std::vector<WorldEntity>& Entities() const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] bool TryGetWorldMatrix(EntityId id, DirectX::XMFLOAT4X4& result) const;

    void Clear();
    bool ReplaceEntities(std::vector<WorldEntity> entities, std::string* error = nullptr);

private:
    bool IsDescendantOf(EntityId candidate, EntityId ancestor) const;

    std::vector<WorldEntity> entities_;
};
