#pragma once

#include "world/World.h"

#include <string_view>

class SceneLoader {
public:
    static bool LoadScene(World& world, std::string_view scene) {
        return world.RequestSceneLoad(std::string(scene));
    }
};
