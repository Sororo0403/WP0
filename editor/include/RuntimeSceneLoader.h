#pragma once

#include "world/PhysicsSettings.h"

#include <filesystem>
#include <string>
#include <string_view>

class World;

class RuntimeSceneLoader {
public:
    static bool Load(const std::filesystem::path& sceneRoot,
                     std::string_view request,
                     const PhysicsSettings& physicsSettings, World& world,
                     std::filesystem::path& loadedPath, std::string& error);
};
