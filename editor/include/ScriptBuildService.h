#pragma once

#include <filesystem>
#include <string>

class ScriptBuildService {
public:
    static bool BuildIfNeeded(const std::filesystem::path& projectRoot,
                              std::string& error);
    static bool Build(const std::filesystem::path& projectRoot, std::string& error);
};
