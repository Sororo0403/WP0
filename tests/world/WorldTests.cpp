#include "world/World.h"
#include "world/WorldSerializer.h"

#include <filesystem>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
} // namespace

int main() {
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
    if (WorldEntity* rootEntity = source.Find(root)) {
        rootEntity->transform.position = {4.0f, 0.0f, 0.0f};
    }

    DirectX::XMFLOAT4X4 childWorld{};
    if (!Check(source.TryGetWorldMatrix(child, childWorld) &&
                   std::abs(childWorld._41 - 5.0f) < 0.001f &&
                   std::abs(childWorld._42 - 2.0f) < 0.001f,
               "Parent and child transforms were not composed.")) {
        return 5;
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
                   restoredChild->meshRenderer->primitive == MeshPrimitive::Sphere,
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
                   duplicateChild->meshRenderer->primitive == MeshPrimitive::Sphere,
               "Hierarchy duplication did not preserve entity data and parenting.")) {
        return 8;
    }
    if (!Check(!source.DuplicateEntityHierarchy({}).IsValid(),
               "An invalid entity hierarchy was duplicated.")) {
        return 9;
    }

    const std::filesystem::path testPath =
        std::filesystem::temp_directory_path() / ("wp0-world-" + root.ToString() + ".wp0scene");
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
    return 0;
}
