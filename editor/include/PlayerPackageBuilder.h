#pragma once

#include <filesystem>
#include <string>

struct PlayerPackageRequest {
    std::filesystem::path executable;
    std::filesystem::path projectRoot;
    std::filesystem::path manifest;
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
    std::filesystem::path destination;
    std::string configuration;
};

class PlayerPackageBuilder {
public:
    static bool Build(const PlayerPackageRequest& request, std::string& error);
};
