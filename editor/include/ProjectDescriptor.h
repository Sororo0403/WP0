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
};
