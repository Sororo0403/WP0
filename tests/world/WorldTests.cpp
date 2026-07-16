#include "world/World.h"
#include "world/WorldSerializer.h"

#include <filesystem>
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

    const std::string serialized = WorldSerializer::Serialize(source);
    World restored;
    std::string error;
    if (!Check(WorldSerializer::Deserialize(serialized, restored, &error), error.c_str())) {
        return 5;
    }
    const WorldEntity* restoredChild = restored.Find(child);
    if (!Check(restored.Entities().size() == 2u && restoredChild != nullptr &&
                   restoredChild->parent == root && restoredChild->transform.position.x == 1.0f &&
                   restoredChild->transform.rotationDegrees.z == 30.0f,
               "World JSON round-trip changed entity data.")) {
        return 6;
    }

    const std::filesystem::path testPath =
        std::filesystem::temp_directory_path() / ("wp0-world-" + root.ToString() + ".wp0scene");
    World fileRestored;
    if (!Check(WorldSerializer::Save(source, testPath, &error), error.c_str()) ||
        !Check(WorldSerializer::Load(testPath, fileRestored, &error), error.c_str()) ||
        !Check(fileRestored.Find(child) != nullptr, "Scene file round-trip lost an entity.")) {
        std::error_code cleanupError;
        std::filesystem::remove(testPath, cleanupError);
        return 7;
    }
    std::error_code cleanupError;
    std::filesystem::remove(testPath, cleanupError);

    std::vector<WorldEntity> invalidEntities(1u);
    invalidEntities[0].id = EntityId::New();
    invalidEntities[0].parent = EntityId::New();
    if (!Check(!restored.ReplaceEntities(std::move(invalidEntities), &error),
               "Missing hierarchy parent was accepted.")) {
        return 8;
    }
    if (!Check(restored.Entities().size() == 2u,
               "Failed replacement modified the existing world.")) {
        return 9;
    }

    if (!Check(restored.DestroyEntity(root) && restored.Empty(),
               "Recursive hierarchy deletion failed.")) {
        return 10;
    }
    return 0;
}
