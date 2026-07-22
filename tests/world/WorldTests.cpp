#include "AssetImportPlanner.h"
#include "ProjectDescriptor.h"
#include "ProjectScriptLibrary.h"
#include "ScriptBuildService.h"
#include "RecentScenesStore.h"
#include "ScriptAsset.h"
#include "collision/CollisionUtil.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "runtime/BehaviorRegistry.h"
#include "runtime/BehaviorSystem.h"
#include "world/World.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"
#include "../../engine/src/model/internal/ModelPrimitiveFactory.h"

#include <filesystem>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
class LifecycleBehavior final : public Behavior {
public:
    LifecycleBehavior(int& startCount, int& updateCount, int& stopCount, float& lastDeltaTime)
        : startCount_(startCount), updateCount_(updateCount), stopCount_(stopCount),
          lastDeltaTime_(lastDeltaTime) {}

    void OnStart(World& world, EntityId entity) override {
        ++startCount_;
        if (WorldEntity* target = world.Find(entity)) {
            target->transform.position.x += 1.0f;
        }
    }

    void OnUpdate(World& world, EntityId entity, float deltaTime) override {
        ++updateCount_;
        lastDeltaTime_ = deltaTime;
        if (WorldEntity* target = world.Find(entity)) {
            target->transform.position.y += deltaTime;
        }
    }

    void OnStop(World& world, EntityId entity) override {
        (void)world;
        (void)entity;
        ++stopCount_;
    }

private:
    int& startCount_;
    int& updateCount_;
    int& stopCount_;
    float& lastDeltaTime_;
};

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool IsVerticallyCentered(const ModelPrimitiveFactory::PrimitiveMeshData& primitive) {
    float minimum = (std::numeric_limits<float>::max)();
    float maximum = (std::numeric_limits<float>::lowest)();
    for (const ModelVertex& vertex : primitive.vertices) {
        minimum = (std::min)(minimum, vertex.position.y);
        maximum = (std::max)(maximum, vertex.position.y);
    }
    return !primitive.vertices.empty() && std::abs(minimum + maximum) < 0.0001f;
}

bool HasOutwardWinding(const ModelPrimitiveFactory::PrimitiveMeshData& primitive) {
    if (primitive.indices.empty() || primitive.indices.size() % 3u != 0u) {
        return false;
    }
    for (size_t index = 0; index < primitive.indices.size(); index += 3u) {
        const ModelVertex& a = primitive.vertices[primitive.indices[index]];
        const ModelVertex& b = primitive.vertices[primitive.indices[index + 1u]];
        const ModelVertex& c = primitive.vertices[primitive.indices[index + 2u]];
        const DirectX::XMFLOAT3 ab{b.position.x - a.position.x, b.position.y - a.position.y,
                                  b.position.z - a.position.z};
        const DirectX::XMFLOAT3 ac{c.position.x - a.position.x, c.position.y - a.position.y,
                                  c.position.z - a.position.z};
        const DirectX::XMFLOAT3 cross{ab.y * ac.z - ab.z * ac.y,
                                     ab.z * ac.x - ab.x * ac.z,
                                     ab.x * ac.y - ab.y * ac.x};
        const DirectX::XMFLOAT3 normal{a.normal.x + b.normal.x + c.normal.x,
                                      a.normal.y + b.normal.y + c.normal.y,
                                      a.normal.z + b.normal.z + c.normal.z};
        const float areaSquared = cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
        const float facing = cross.x * normal.x + cross.y * normal.y + cross.z * normal.z;
        if (areaSquared > 0.00000001f && facing <= 0.0f) {
            return false;
        }
    }
    return true;
}

bool RotationRoundTrips(const DirectX::XMFLOAT3& degrees) {
    using namespace DirectX;
    const XMVECTOR quaternion = XMQuaternionRotationRollPitchYaw(
        XMConvertToRadians(degrees.x), XMConvertToRadians(degrees.y),
        XMConvertToRadians(degrees.z));
    const XMFLOAT3 restored = MathUtils::RotationDegreesFromQuaternion(quaternion, degrees);
    XMFLOAT4X4 expected{};
    XMFLOAT4X4 actual{};
    XMStoreFloat4x4(&expected, XMMatrixRotationRollPitchYaw(
                                   XMConvertToRadians(degrees.x),
                                   XMConvertToRadians(degrees.y),
                                   XMConvertToRadians(degrees.z)));
    XMStoreFloat4x4(&actual, XMMatrixRotationRollPitchYaw(
                                 XMConvertToRadians(restored.x),
                                 XMConvertToRadians(restored.y),
                                 XMConvertToRadians(restored.z)));
    const float* expectedValues = &expected._11;
    const float* actualValues = &actual._11;
    for (size_t index = 0; index < 16u; ++index) {
        if (std::abs(expectedValues[index] - actualValues[index]) > 0.0001f) {
            return false;
        }
    }
    return true;
}
} // namespace

int main() {
    if (!Check(ScriptAssets::IsScriptFile("Player.cpp") &&
                   ScriptAssets::IsScriptSourceFile("Player.cpp") &&
                   ScriptAssets::IsScriptSourceFile("Player.h") &&
                   !ScriptAssets::IsScriptFile("Player.h"),
               "Script asset validation is invalid.")) {
        return 1;
    }

    const std::filesystem::path repositoryRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    ProjectScriptLibrary projectScripts;
    BehaviorRegistry projectBehaviorRegistry;
    std::string projectScriptError;
    const std::filesystem::path testProjectRoot =
        repositoryRoot / L"projects" / L"test";
    const bool projectScriptsLoaded =
        ScriptBuildService::BuildIfNeeded(testProjectRoot, projectScriptError) &&
        projectScripts.Load(testProjectRoot, nullptr, projectBehaviorRegistry,
                            projectScriptError);
    std::unique_ptr<Behavior> firstPerson =
        projectBehaviorRegistry.Create("FirstPersonController");
    std::unique_ptr<Behavior> rotator = projectBehaviorRegistry.Create("Rotator");
    if (!Check(projectScriptsLoaded && projectScripts.IsLoaded() && firstPerson && rotator &&
                   projectBehaviorRegistry.Requirements("FirstPersonController") != nullptr &&
                   projectBehaviorRegistry.Requirements("FirstPersonController")
                       ->characterController &&
                   projectBehaviorRegistry.TypeFromSourceAsset(
                       "asset://Scripts/FirstPersonController.cpp") ==
                       "FirstPersonController" &&
                   projectBehaviorRegistry.SourceAsset("Rotator") ==
                       "asset://Scripts/Rotator.cpp",
               projectScriptError.empty() ?
                   "Project Script module registration or factory is invalid." :
                   projectScriptError.c_str())) {
        return 140;
    }

    World behaviorWorld;
    const EntityId behaviorEntity = behaviorWorld.CreateEntity("Behavior Entity");
    int behaviorStartCount = 0;
    int behaviorUpdateCount = 0;
    int behaviorStopCount = 0;
    float behaviorLastDeltaTime = 0.0f;
    BehaviorRegistry behaviorRegistry;
    if (!Check(behaviorRegistry.Register("Lifecycle", [&] {
                   return std::make_unique<LifecycleBehavior>(
                       behaviorStartCount, behaviorUpdateCount, behaviorStopCount,
                       behaviorLastDeltaTime);
               }) &&
                   !behaviorRegistry.Register("Lifecycle", [] {
                       return std::unique_ptr<Behavior>{};
                   }) &&
                   behaviorRegistry.Create("Missing") == nullptr &&
                   behaviorRegistry.Types().size() == 1u,
               "Behavior type registration is invalid.")) {
        return 127;
    }
    if (!Check(behaviorRegistry.Register(
                   "ControllerDependency", [] { return std::make_unique<Behavior>(); },
                   {.characterController = true}),
               "Behavior requirements could not be registered.")) {
        return 139;
    }
    WorldEntity* dependencyTarget = behaviorWorld.Find(behaviorEntity);
    std::string dependencyError;
    if (!Check(dependencyTarget != nullptr &&
                   !behaviorRegistry.ValidateRequirements("ControllerDependency",
                                                          *dependencyTarget,
                                                          &dependencyError) &&
                   dependencyError.find("CharacterController") != std::string::npos &&
                   behaviorRegistry.EnsureRequirements("ControllerDependency",
                                                       *dependencyTarget) &&
                   dependencyTarget->characterController &&
                   behaviorRegistry.ValidateRequirements("ControllerDependency",
                                                          *dependencyTarget,
                                                          &dependencyError) &&
                   !behaviorRegistry.EnsureRequirements("Missing", *dependencyTarget),
               "Behavior component requirements were not enforced.")) {
        return 140;
    }
    BehaviorSystem behaviors;
    if (!Check(behaviors.Attach(behaviorEntity, behaviorRegistry.Create("Lifecycle")),
               "A valid Runtime Behavior could not be attached.")) {
        return 125;
    }
    behaviors.Start(behaviorWorld);
    behaviors.Update(0.25f);
    behaviors.Stop();
    const WorldEntity* behaviorTarget = behaviorWorld.Find(behaviorEntity);
    if (!Check(!behaviors.IsRunning() && behaviors.Size() == 1u &&
                   behaviorStartCount == 1 && behaviorUpdateCount == 1 &&
                   behaviorStopCount == 1 && behaviorLastDeltaTime == 0.25f &&
                   behaviorTarget != nullptr && behaviorTarget->transform.position.x == 1.0f &&
                   behaviorTarget->transform.position.y == 0.25f,
               "Runtime Behavior lifecycle did not run in order.")) {
        return 126;
    }

    if (!Check(RotationRoundTrips({25.0f, -40.0f, 70.0f}) &&
                   RotationRoundTrips({120.0f, 215.0f, -150.0f}) &&
                   RotationRoundTrips({89.999f, 35.0f, -20.0f}),
               "Editor Euler rotation conversion changed the rotation matrix.")) {
        return 124;
    }
    const auto boxPrimitive =
        ModelPrimitiveFactory::BuildBox(0u, Material{}, 1.0f, 2.0f, 1.0f);
    const auto cylinderPrimitive =
        ModelPrimitiveFactory::BuildCylinder(0u, Material{}, 16u, 0.5f, 0.5f, 2.0f);
    const auto planePrimitive = ModelPrimitiveFactory::BuildPlane(0u, Material{});
    const auto spherePrimitive =
        ModelPrimitiveFactory::BuildSphere(0u, Material{}, 16u, 8u, 0.5f);
    if (!Check(boxPrimitive && IsVerticallyCentered(*boxPrimitive),
               "Box primitive is not centered on its origin.")) {
        return 116;
    }
    if (!Check(cylinderPrimitive && IsVerticallyCentered(*cylinderPrimitive),
               "Cylinder primitive is not centered on its origin.")) {
        return 117;
    }
    if (!Check(planePrimitive && boxPrimitive && spherePrimitive && cylinderPrimitive &&
                   HasOutwardWinding(*planePrimitive) &&
                   HasOutwardWinding(*boxPrimitive) &&
                   HasOutwardWinding(*spherePrimitive) &&
                   HasOutwardWinding(*cylinderPrimitive),
               "Primitive front-face winding is inconsistent.")) {
        return 123;
    }

    World source;
    const EntityId root = source.CreateEntity("Root");
    const EntityId child = source.CreateEntity("Child");
    if (!Check(root.IsValid() && child.IsValid() && root != child, "Entity ids are invalid.")) {
        return 1;
    }

    EntityId parsed{};
    if (!Check(EntityId::TryParse(root.ToString(), parsed) && parsed == root,
               "Entity id text round-trip failed.")) {
        return 2;
    }
    if (!Check(source.SetParent(child, root), "Valid parenting failed.") ||
        !Check(!source.SetParent(root, child), "Hierarchy cycle was accepted.")) {
        return 3;
    }

    WorldEntity* childEntity = source.Find(child);
    if (!Check(childEntity != nullptr, "Child entity was not found.")) {
        return 4;
    }
    childEntity->transform.position = {1.0f, 2.0f, 3.0f};
    childEntity->transform.rotationDegrees = {10.0f, 20.0f, 30.0f};
    childEntity->meshRenderer = MeshRendererComponent{};
    childEntity->meshRenderer->primitive = MeshPrimitive::Sphere;
    childEntity->materialOverride = MaterialOverrideComponent{};
    childEntity->materialOverride->baseColor = {0.25f, 0.5f, 0.75f, 0.8f};
    childEntity->materialOverride->metallic = 0.7f;
    childEntity->materialOverride->roughness = 0.2f;
    childEntity->materialOverride->baseColorTexturePath = "asset://textures/test.png";
    childEntity->materialOverride->normalTexturePath = "asset://textures/test_normal.png";
    childEntity->materialOverride->normalStrength = 1.5f;
    childEntity->materialOverride->roughnessTexturePath = "asset://textures/test_orm.png";
    childEntity->materialOverride->metallicTexturePath = "asset://textures/test_orm.png";
    childEntity->materialOverride->pbrTexturePacking =
        MaterialPbrTexturePacking::OcclusionRoughnessMetallic;
    childEntity->materialOverride->blendMode = MaterialSurfaceBlendMode::Transparent;
    childEntity->materialOverride->alphaCutoff = 0.35f;
    childEntity->materialOverride->cullMode = MaterialSurfaceCullMode::None;
    childEntity->materialOverride->depthWrite = false;
    childEntity->light = LightComponent{};
    childEntity->light->type = LightType::Point;
    childEntity->light->intensity = 2.0f;
    childEntity->scripts.push_back(
        {true, "Rotator", "asset://Scripts/Rotator.cpp"});
    childEntity->scripts.push_back(
        {false, "FirstPersonController",
         "asset://Scripts/FirstPersonController.cpp"});
    childEntity->scripts.emplace_back();
    childEntity->boxCollider = BoxColliderComponent{};
    childEntity->boxCollider->center = {0.25f, 0.5f, -0.25f};
    childEntity->boxCollider->size = {1.0f, 2.0f, 3.0f};
    childEntity->characterController = CharacterControllerComponent{};
    childEntity->characterController->center = {0.0f, 1.0f, 0.0f};
    childEntity->characterController->radius = 0.4f;
    childEntity->characterController->height = 1.8f;
    if (WorldEntity* rootEntity = source.Find(root)) {
        rootEntity->transform.position = {4.0f, 0.0f, 0.0f};
        rootEntity->camera = CameraComponent{};
        rootEntity->camera->primary = true;
        rootEntity->camera->fieldOfViewDegrees = 60.0f;
    }

    DirectX::XMFLOAT4X4 childWorld{};
    if (!Check(source.TryGetWorldMatrix(child, childWorld) &&
                   std::abs(childWorld._41 - 5.0f) < 0.001f &&
                   std::abs(childWorld._42 - 2.0f) < 0.001f,
               "Parent and child transforms were not composed.")) {
        return 5;
    }
    OBB childCollider{};
    if (!Check(TryBuildWorldBoxCollider(source, child, childCollider) &&
                   std::abs(childCollider.size.x - 1.0f) < 0.001f &&
                   std::abs(childCollider.size.y - 2.0f) < 0.001f &&
                   std::abs(childCollider.size.z - 3.0f) < 0.001f,
               "World BoxCollider did not follow the entity transform.")) {
        return 130;
    }
    OBB overlappingCollider = childCollider;
    overlappingCollider.center.x += 0.25f;
    if (!Check(CollisionUtil::CheckOBB(childCollider, overlappingCollider),
               "Overlapping World BoxColliders were not detected.")) {
        return 131;
    }
    overlappingCollider.center.x += 100.0f;
    if (!Check(!CollisionUtil::CheckOBB(childCollider, overlappingCollider),
               "Separated World BoxColliders reported a collision.")) {
        return 132;
    }

    World movementWorld;
    const EntityId mover = movementWorld.CreateEntity("Mover");
    const EntityId wall = movementWorld.CreateEntity("Wall");
    movementWorld.Find(mover)->characterController = CharacterControllerComponent{};
    movementWorld.Find(wall)->boxCollider = BoxColliderComponent{};
    movementWorld.Find(wall)->transform.position.x = 1.0f;
    const CharacterMoveResult blockedMovement =
        MoveCharacterController(movementWorld, mover, {2.0f, 0.0f, 0.5f});
    if (!Check(blockedMovement.appliedMotion.x >= 0.0f &&
                   blockedMovement.appliedMotion.x < 0.1f &&
                   std::abs(blockedMovement.appliedMotion.z - 0.5f) < 0.001f &&
                   (static_cast<uint8_t>(blockedMovement.flags) &
                    static_cast<uint8_t>(CharacterCollisionFlags::Sides)) != 0u &&
                   movementWorld.Find(mover)->transform.position.x < 0.1f &&
                   std::abs(movementWorld.Find(mover)->transform.position.z - 0.5f) < 0.001f,
               "CharacterController did not stop at a solid BoxCollider and slide.")) {
        return 134;
    }
    movementWorld.Find(wall)->boxCollider->isTrigger = true;
    const float positionBeforeTrigger = movementWorld.Find(mover)->transform.position.x;
    const CharacterMoveResult triggerMovement =
        MoveCharacterController(movementWorld, mover, {2.0f, 0.0f, 0.0f});
    if (!Check(std::abs(triggerMovement.appliedMotion.x - 2.0f) < 0.001f &&
                   std::abs(movementWorld.Find(mover)->transform.position.x -
                            (positionBeforeTrigger + 2.0f)) < 0.001f,
               "Trigger BoxCollider incorrectly blocked movement.")) {
        return 135;
    }

    World groundWorld;
    const EntityId groundedController = groundWorld.CreateEntity("Grounded Controller");
    const EntityId floor = groundWorld.CreateEntity("Floor");
    groundWorld.Find(groundedController)->characterController =
        CharacterControllerComponent{};
    groundWorld.Find(groundedController)->transform.position.y = 1.5f;
    groundWorld.Find(floor)->boxCollider = BoxColliderComponent{};
    groundWorld.Find(floor)->boxCollider->size = {10.0f, 1.0f, 10.0f};
    const CharacterMoveResult downwardMovement =
        MoveCharacterController(groundWorld, groundedController, {0.0f, -1.0f, 0.0f});
    if (!Check(downwardMovement.appliedMotion.y > -0.1f &&
                   (static_cast<uint8_t>(downwardMovement.flags) &
                    static_cast<uint8_t>(CharacterCollisionFlags::Below)) != 0u,
               "CharacterController did not report a collision below.")) {
        return 138;
    }

    const std::string serialized = WorldSerializer::Serialize(source);
    World restored;
    std::string error;
    if (!Check(WorldSerializer::Deserialize(serialized, restored, &error), error.c_str())) {
        return 6;
    }
    const WorldEntity* restoredChild = restored.Find(child);
    if (!Check(restored.Entities().size() == 2u && restoredChild != nullptr &&
                   restoredChild->parent == root && restoredChild->transform.position.x == 1.0f &&
                   restoredChild->transform.rotationDegrees.z == 30.0f &&
                   restoredChild->meshRenderer &&
                   restoredChild->meshRenderer->primitive == MeshPrimitive::Sphere &&
                   restoredChild->materialOverride &&
                   restoredChild->materialOverride->baseColor.z == 0.75f &&
                   restoredChild->materialOverride->baseColor.w == 0.8f &&
                   restoredChild->materialOverride->metallic == 0.7f &&
                   restoredChild->materialOverride->roughness == 0.2f &&
                   restoredChild->materialOverride->baseColorTexturePath ==
                       "asset://textures/test.png" &&
                   restoredChild->materialOverride->normalTexturePath ==
                       "asset://textures/test_normal.png" &&
                   restoredChild->materialOverride->normalStrength == 1.5f &&
                   restoredChild->materialOverride->roughnessTexturePath ==
                       "asset://textures/test_orm.png" &&
                   restoredChild->materialOverride->metallicTexturePath ==
                       "asset://textures/test_orm.png" &&
                   restoredChild->materialOverride->pbrTexturePacking ==
                       MaterialPbrTexturePacking::OcclusionRoughnessMetallic &&
                   restoredChild->materialOverride->blendMode ==
                       MaterialSurfaceBlendMode::Transparent &&
                   restoredChild->materialOverride->alphaCutoff == 0.35f &&
                   restoredChild->materialOverride->cullMode ==
                       MaterialSurfaceCullMode::None &&
                   !restoredChild->materialOverride->depthWrite &&
                   restoredChild->light && restoredChild->light->type == LightType::Point &&
                   restoredChild->light->intensity == 2.0f &&
                   restoredChild->scripts.size() == 3u &&
                   restoredChild->scripts[0].enabled &&
                   restoredChild->scripts[0].type == "Rotator" &&
                   restoredChild->scripts[0].scriptAssetPath ==
                       "asset://Scripts/Rotator.cpp" &&
                   !restoredChild->scripts[1].enabled &&
                   restoredChild->scripts[1].type == "FirstPersonController" &&
                   restoredChild->scripts[2].type.empty() &&
                   restoredChild->scripts[2].scriptAssetPath.empty() &&
                   restoredChild->boxCollider && !restoredChild->boxCollider->isTrigger &&
                   restoredChild->boxCollider->center.y == 0.5f &&
                   restoredChild->boxCollider->size.z == 3.0f &&
                   restoredChild->characterController &&
                   restoredChild->characterController->center.y == 1.0f &&
                   restoredChild->characterController->radius == 0.4f &&
                   restoredChild->characterController->height == 1.8f &&
                   restored.Find(root)->camera && restored.Find(root)->camera->primary &&
                   restored.Find(root)->camera->fieldOfViewDegrees == 60.0f,
               "World JSON round-trip changed entity data.")) {
        return 7;
    }

    const EntityId duplicateRoot = source.DuplicateEntityHierarchy(root);
    const WorldEntity* duplicateRootEntity = source.Find(duplicateRoot);
    const std::vector<EntityId> duplicateChildren = source.GetChildren(duplicateRoot);
    const WorldEntity* duplicateChild =
        duplicateChildren.size() == 1u ? source.Find(duplicateChildren.front()) : nullptr;
    if (!Check(duplicateRoot.IsValid() && duplicateRoot != root && duplicateRootEntity != nullptr &&
                   duplicateRootEntity->name == "Root Copy" && !duplicateRootEntity->parent.IsValid() &&
                   duplicateChild != nullptr && duplicateChild->id != child &&
                   duplicateChild->name == "Child" && duplicateChild->meshRenderer &&
                   duplicateChild->meshRenderer->primitive == MeshPrimitive::Sphere &&
                   duplicateChild->materialOverride &&
                   duplicateChild->materialOverride->metallic == 0.7f &&
                   duplicateChild->materialOverride->baseColorTexturePath ==
                       "asset://textures/test.png" &&
                   duplicateChild->materialOverride->normalTexturePath ==
                       "asset://textures/test_normal.png" &&
                   duplicateChild->materialOverride->normalStrength == 1.5f &&
                   duplicateChild->materialOverride->roughnessTexturePath ==
                       "asset://textures/test_orm.png" &&
                   duplicateChild->materialOverride->pbrTexturePacking ==
                       MaterialPbrTexturePacking::OcclusionRoughnessMetallic &&
                   duplicateChild->materialOverride->blendMode ==
                       MaterialSurfaceBlendMode::Transparent &&
                   duplicateChild->materialOverride->cullMode ==
                       MaterialSurfaceCullMode::None &&
                   duplicateChild->light && duplicateChild->light->type == LightType::Point &&
                   duplicateChild->scripts.size() == 3u &&
                   duplicateChild->scripts[0].type == "Rotator" &&
                   duplicateChild->scripts[0].scriptAssetPath ==
                       "asset://Scripts/Rotator.cpp" &&
                   duplicateChild->scripts[1].type == "FirstPersonController" &&
                   duplicateChild->scripts[2].type.empty() &&
                   duplicateChild->boxCollider &&
                   duplicateChild->boxCollider->center.x == 0.25f &&
                   duplicateChild->boxCollider->size.y == 2.0f &&
                   duplicateChild->characterController &&
                   duplicateChild->characterController->radius == 0.4f &&
                   duplicateChild->characterController->height == 1.8f &&
                   duplicateRootEntity->camera && !duplicateRootEntity->camera->primary,
               "Hierarchy duplication did not preserve entity data and parenting.")) {
        return 8;
    }
    if (!Check(!source.DuplicateEntityHierarchy({}).IsValid(),
               "An invalid entity hierarchy was duplicated.")) {
        return 9;
    }

    const std::filesystem::path testPath =
        std::filesystem::temp_directory_path() /
        ("likeengine-world-" + root.ToString() + ".likescene");
    World fileRestored;
    if (!Check(WorldSerializer::Save(source, testPath, &error), error.c_str()) ||
        !Check(WorldSerializer::Load(testPath, fileRestored, &error), error.c_str()) ||
        !Check(fileRestored.Find(child) != nullptr, "Scene file round-trip lost an entity.")) {
        std::error_code cleanupError;
        std::filesystem::remove(testPath, cleanupError);
        return 10;
    }
    std::error_code cleanupError;
    std::filesystem::remove(testPath, cleanupError);

    std::vector<WorldEntity> invalidEntities(1u);
    invalidEntities[0].id = EntityId::New();
    invalidEntities[0].parent = EntityId::New();
    if (!Check(!restored.ReplaceEntities(std::move(invalidEntities), &error),
               "Missing hierarchy parent was accepted.")) {
        return 11;
    }
    if (!Check(restored.Entities().size() == 2u,
               "Failed replacement modified the existing world.")) {
        return 12;
    }
    std::vector<WorldEntity> invalidColliderEntities(1u);
    invalidColliderEntities[0].id = EntityId::New();
    invalidColliderEntities[0].boxCollider = BoxColliderComponent{};
    invalidColliderEntities[0].boxCollider->size.y = 0.0f;
    if (!Check(!restored.ReplaceEntities(std::move(invalidColliderEntities), &error),
               "Invalid direct BoxCollider replacement was accepted.")) {
        return 133;
    }
    std::vector<WorldEntity> invalidControllerEntities(1u);
    invalidControllerEntities[0].id = EntityId::New();
    invalidControllerEntities[0].characterController = CharacterControllerComponent{};
    invalidControllerEntities[0].characterController->height = 0.5f;
    if (!Check(!restored.ReplaceEntities(std::move(invalidControllerEntities), &error),
               "Invalid direct CharacterController replacement was accepted.")) {
        return 136;
    }

    if (!Check(restored.DestroyEntity(root) && restored.Empty(),
               "Recursive hierarchy deletion failed.")) {
        return 13;
    }

    const std::string invalidRenderer =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MeshRenderer":{"enabled":true,"source":"Primitive","primitive":99,"model":""}}}]})";
    World rejected;
    if (!Check(!WorldSerializer::Deserialize(invalidRenderer, rejected, &error),
               "Invalid MeshRenderer data was accepted.")) {
        return 14;
    }
    const std::string invalidCamera =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Camera","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Camera":{"enabled":true,"primary":true,"projection":"Perspective","fieldOfView":180,"orthographicHeight":10,"nearClip":1,"farClip":0.5}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidCamera, rejected, &error),
               "Invalid Camera data was accepted.")) {
        return 114;
    }
    const std::string invalidLight =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Light","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Light":{"enabled":true,"type":"Spot","color":[1,1,1],"intensity":1,"range":10,"innerAngle":60,"outerAngle":30}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidLight, rejected, &error),
               "Invalid Light data was accepted.")) {
        return 115;
    }
    const std::string legacyBehavior =
        R"({"version":1,"entities":[{"id":"00000000-0000-0001-0000-000000000001","parent":null,"name":"Legacy Behavior","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Behavior":{"enabled":true,"type":"Rotator"}}}]})";
    World legacyBehaviorWorld;
    if (!Check(WorldSerializer::Deserialize(legacyBehavior, legacyBehaviorWorld, &error) &&
                   legacyBehaviorWorld.Entities().size() == 1u &&
                   legacyBehaviorWorld.Entities().front().scripts.size() == 1u &&
                   legacyBehaviorWorld.Entities().front().scripts.front().type == "Rotator",
               "Legacy Behavior data was not migrated to a Script component.")) {
        return 127;
    }
    const std::string invalidBehavior =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Behavior","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"Behavior":{"enabled":true,"type":""}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidBehavior, rejected, &error),
               "Invalid Behavior data was accepted.")) {
        return 128;
    }
    const std::string invalidBoxCollider =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Collider","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"BoxCollider":{"enabled":true,"center":[0,0,0],"size":[1,0,1],"isTrigger":false}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidBoxCollider, rejected, &error),
               "Invalid BoxCollider data was accepted.")) {
        return 129;
    }
    const std::string invalidCharacterController =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Controller","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"CharacterController":{"enabled":true,"center":[0,0,0],"radius":1,"height":1,"slopeLimit":45,"stepOffset":0.3,"skinWidth":0.05,"minMoveDistance":0}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidCharacterController, rejected, &error),
               "Invalid CharacterController data was accepted.")) {
        return 137;
    }
    const std::string invalidMaterial =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Material","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,2],"metallic":-1,"roughness":0.5}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidMaterial, rejected, &error),
               "Invalid MaterialOverride data was accepted.")) {
        return 118;
    }
    const std::string invalidNormalMaterial =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Normal","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"normalTexture":"asset://normal.png","normalStrength":-1}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidNormalMaterial, rejected, &error),
               "Invalid Normal texture settings were accepted.")) {
        return 120;
    }
    const std::string invalidPbrPacking =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Packing","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"pbrTexturePacking":"Unknown"}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidPbrPacking, rejected, &error),
               "Invalid PBR texture packing was accepted.")) {
        return 121;
    }
    const std::string invalidMaterialSurface =
        R"({"version":1,"entities":[{"id":"0000000000000001-0000000000000001","parent":null,"name":"Bad Surface","components":{"Transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},"MaterialOverride":{"enabled":true,"baseColor":[1,1,1,1],"metallic":0,"roughness":0.5,"blendMode":"Additive","alphaCutoff":2,"cullMode":"Sideways","depthWrite":true}}}]})";
    if (!Check(!WorldSerializer::Deserialize(invalidMaterialSurface, rejected, &error),
               "Invalid Material surface settings were accepted.")) {
        return 122;
    }
    const std::filesystem::path projectAssets =
        std::filesystem::temp_directory_path() / L"engine-external-project" / L"assets";
    AssetManager::SetProjectAssetRoot(projectAssets);
    const std::filesystem::path resolvedAsset =
        AssetManager::ResolvePathStrict(L"asset://models/brain_stem/BrainStem.glb");
    if (!Check(resolvedAsset ==
                   (projectAssets / L"models" / L"brain_stem" / L"BrainStem.glb")
                       .lexically_normal(),
               "asset:// URI did not resolve against the project asset root.")) {
        return 15;
    }
    if (!Check(AssetManager::ResolvePathStrict(L"asset://../outside.glb").empty(),
               "Project asset traversal was accepted.")) {
        return 16;
    }
    AssetManager::SetEngineResourceRoot(projectAssets / L"engine-resources");
    if (!Check(AssetManager::ResolvePath(L"engine://../outside.hlsl").empty(),
               "Engine resource traversal was accepted.")) {
        return 17;
    }

    const std::filesystem::path projectDirectory =
        std::filesystem::temp_directory_path() / ("engine-project-" + root.ToString());
    std::error_code projectFilesystemError;
    std::filesystem::remove_all(projectDirectory, projectFilesystemError);
    projectFilesystemError.clear();
    if (!Check(std::filesystem::create_directory(projectDirectory, projectFilesystemError) &&
                   !projectFilesystemError,
               "Project test directory could not be created.")) {
        return 18;
    }
    ProjectDescriptor createdProject;
    if (!Check(ProjectDescriptor::Create(projectDirectory, "Created Project", createdProject,
                                         error),
               error.c_str()) ||
        !Check(createdProject.name == "Created Project" &&
                   createdProject.assetRoot == projectDirectory / L"assets" &&
                   createdProject.sceneRoot == projectDirectory / L"scenes" &&
                   std::filesystem::is_regular_file(createdProject.manifestPath) &&
                   std::filesystem::is_regular_file(projectDirectory / L".gitignore") &&
                   std::filesystem::is_directory(createdProject.assetRoot) &&
                   std::filesystem::is_directory(createdProject.sceneRoot),
               "Created project structure is invalid.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 19;
    }
    ProjectDescriptor duplicateProject;
    if (!Check(!ProjectDescriptor::Create(projectDirectory, "Duplicate", duplicateProject, error),
               "Project creation accepted a non-empty directory.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 20;
    }
    if (!Check(WorldSerializer::Save(source, createdProject.startupScene, &error), error.c_str())) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 21;
    }
    const std::filesystem::path recentSettings = projectDirectory / L"settings" / L"recent.json";
    RecentScenesStore recentScenes(recentSettings, createdProject.sceneRoot);
    if (!Check(recentScenes.Save({createdProject.startupScene,
                                  projectDirectory.parent_path() / L"outside.likescene"}),
               "Recent scene settings could not be saved.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 22;
    }
    const std::vector<std::filesystem::path> restoredScenes = recentScenes.Load();
    if (!Check(restoredScenes.size() == 1u &&
                   restoredScenes.front() ==
                       std::filesystem::weakly_canonical(createdProject.startupScene),
               "Recent scenes were not safely restored.")) {
        std::filesystem::remove_all(projectDirectory, projectFilesystemError);
        return 23;
    }
    std::filesystem::remove_all(projectDirectory, projectFilesystemError);
    if (!Check(!projectFilesystemError, "Project test directory cleanup failed.")) {
        return 24;
    }
    World ordered;
    const EntityId first = ordered.CreateEntity("First");
    const EntityId second = ordered.CreateEntity("Second");
    const EntityId third = ordered.CreateEntity("Third");
    if (!Check(ordered.MoveEntityBefore(third, first) &&
                   ordered.GetRootEntities() == std::vector<EntityId>{third, first, second},
               "Moving a root entity before its sibling failed.")) {
        return 25;
    }
    if (!Check(ordered.MoveEntityAfter(third, second) &&
                   ordered.GetRootEntities() == std::vector<EntityId>{first, second, third},
               "Moving a root entity after its sibling failed.")) {
        return 26;
    }
    const EntityId firstChild = ordered.CreateEntity("First Child");
    const EntityId secondChild = ordered.CreateEntity("Second Child");
    if (!Check(ordered.SetParent(firstChild, first) && ordered.SetParent(secondChild, first) &&
                   ordered.MoveEntityBefore(secondChild, firstChild) &&
                   ordered.GetChildren(first) ==
                       std::vector<EntityId>{secondChild, firstChild},
               "Moving a child entity before its sibling failed.")) {
        return 27;
    }
    if (!Check(!ordered.MoveEntityBefore(firstChild, second),
               "Entity ordering accepted a sibling from another parent.")) {
        return 28;
    }
    const EntityId fourth = ordered.CreateEntity("Fourth");
    ordered.MoveEntityBefore(first, fourth);
    ordered.MoveEntityBefore(second, fourth);
    if (!Check(ordered.GetRootEntities() ==
                   std::vector<EntityId>{third, first, second, fourth},
               "Moving multiple siblings before a target did not preserve their order.")) {
        return 29;
    }
    ordered.MoveEntityAfter(second, third);
    ordered.MoveEntityAfter(first, third);
    if (!Check(ordered.GetRootEntities() ==
                   std::vector<EntityId>{third, first, second, fourth},
               "Moving multiple siblings after a target did not preserve their order.")) {
        return 30;
    }

    const std::filesystem::path importDirectory =
        std::filesystem::temp_directory_path() / ("asset-import-" + root.ToString());
    std::error_code importFilesystemError;
    std::filesystem::remove_all(importDirectory, importFilesystemError);
    importFilesystemError.clear();
    std::filesystem::create_directories(importDirectory / "gltf/data", importFilesystemError);
    std::filesystem::create_directories(importDirectory / "gltf/textures",
                                        importFilesystemError);
    std::filesystem::create_directories(importDirectory / "obj/materials",
                                        importFilesystemError);
    std::filesystem::create_directories(importDirectory / "obj/textures",
                                        importFilesystemError);
    const auto writeFile = [](const std::filesystem::path& path, std::string_view contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(stream);
    };
    const bool importFilesCreated = !importFilesystemError &&
        writeFile(importDirectory / "gltf/model.gltf",
                  R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data/model.bin"}],"images":[{"uri":"textures/albedo.png"}]})") &&
        writeFile(importDirectory / "gltf/data/model.bin", "mesh-data") &&
        writeFile(importDirectory / "gltf/textures/albedo.png", "image-data") &&
        writeFile(importDirectory / "obj/model.obj", "mtllib materials/model.mtl\n") &&
        writeFile(importDirectory / "obj/materials/model.mtl",
                  "map_Kd -s 1 1 1 ../textures/diffuse\\ image.png\n"
                  "bump ../textures/normal.png\n") &&
        writeFile(importDirectory / "obj/textures/diffuse image.png", "diffuse") &&
        writeFile(importDirectory / "obj/textures/normal.png", "normal");
    if (!Check(importFilesCreated, "Asset import test files could not be created.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 31;
    }

    std::vector<AssetImport::File> importPlan;
    std::string importError;
    if (!Check(AssetImport::IsTextureFile(importDirectory / "gltf/textures/albedo.png") &&
                   AssetImport::BuildPlan(
                       {importDirectory / "gltf/textures/albedo.png"}, importPlan,
                       importError) &&
                   importPlan.size() == 1u,
               "Standalone texture import plan failed.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 119;
    }
    const bool gltfPlanBuilt = AssetImport::BuildPlan(
        {importDirectory / "gltf/model.gltf"}, importPlan, importError);
    if (!Check(gltfPlanBuilt, importError.c_str()) ||
        !Check(importPlan.size() == 3u,
               "glTF import did not collect its buffer and image dependencies.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 32;
    }
    const bool objPlanBuilt = AssetImport::BuildPlan(
        {importDirectory / "obj/model.obj"}, importPlan, importError);
    if (!Check(objPlanBuilt, importError.c_str()) ||
        !Check(importPlan.size() == 4u,
               "OBJ import did not collect its MTL and texture dependencies.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 33;
    }
    std::filesystem::remove(importDirectory / "obj/textures/normal.png",
                            importFilesystemError);
    if (!Check(!AssetImport::BuildPlan({importDirectory / "obj/model.obj"}, importPlan,
                                       importError) &&
                   importError.find("Missing OBJ dependency") != std::string::npos,
               "OBJ import accepted a missing texture dependency.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 34;
    }
    if (!Check(writeFile(importDirectory / "outside.png", "outside") &&
                   writeFile(importDirectory / "obj/materials/model.mtl",
                             "map_Kd ../../outside.png\n") &&
                   !AssetImport::BuildPlan({importDirectory / "obj/model.obj"}, importPlan,
                                           importError) &&
                   importError.find("escapes its source folder") != std::string::npos,
               "OBJ import accepted a dependency outside the OBJ folder.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 35;
    }
    if (!Check(AssetImport::HaveEqualContents(importDirectory / "gltf/data/model.bin",
                                              importDirectory / "gltf/data/model.bin") &&
                   !AssetImport::HaveEqualContents(importDirectory / "gltf/data/model.bin",
                                                   importDirectory / "gltf/textures/albedo.png"),
               "Asset import content comparison is incorrect.")) {
        std::filesystem::remove_all(importDirectory, importFilesystemError);
        return 36;
    }
    std::filesystem::remove_all(importDirectory, importFilesystemError);
    if (!Check(!importFilesystemError, "Asset import test cleanup failed.")) {
        return 37;
    }
    return 0;
}
