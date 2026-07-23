#pragma once

#include <filesystem>
#include <string>

struct ProjectDescriptor {
    uint32_t schemaVersion = 1u;
    std::string projectId;
    std::string name;
    std::filesystem::path manifestPath;
    std::filesystem::path root;
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
    std::filesystem::path startupScene;

    static bool Load(const std::filesystem::path& path, ProjectDescriptor& descriptor,
                     std::string& error);
    static bool Create(const std::filesystem::path& directory, const std::string& name,
                       ProjectDescriptor& descriptor, std::string& error);
    static bool SetStartupScene(const std::filesystem::path& project,
                                const std::filesystem::path& scene,
                                ProjectDescriptor& descriptor, std::string& error);
};
