#include "AssetImportPlanner.h"
#include "ProjectDescriptor.h"
#include "RecentScenesStore.h"
#include "core/AssetManager.h"
#include "world/World.h"
#include "world/WorldSerializer.h"

#include <filesystem>
#include <cmath>
#include <fstream>
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
    childEntity->light = LightComponent{};
    childEntity->light->type = LightType::Point;
    childEntity->light->intensity = 2.0f;
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
                   restoredChild->light && restoredChild->light->type == LightType::Point &&
                   restoredChild->light->intensity == 2.0f &&
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
                   duplicateChild->light && duplicateChild->light->type == LightType::Point &&
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
