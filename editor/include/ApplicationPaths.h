#pragma once

#include <filesystem>

struct ApplicationPaths {
    std::filesystem::path executable;
    std::filesystem::path installRoot;
    std::filesystem::path engineResources;
    std::filesystem::path editorResources;
    std::filesystem::path userData;
    std::filesystem::path cache;

    static ApplicationPaths Discover();
};
