#pragma once

#include <filesystem>
#include <string>

class BehaviorRegistry;
class Input;

class ProjectScriptLibrary {
public:
    ProjectScriptLibrary() = default;
    ~ProjectScriptLibrary();

    ProjectScriptLibrary(const ProjectScriptLibrary&) = delete;
    ProjectScriptLibrary& operator=(const ProjectScriptLibrary&) = delete;

    bool Load(const std::filesystem::path& projectRoot, Input* input,
              BehaviorRegistry& registry, std::string& error);
    [[nodiscard]] bool IsLoaded() const;
    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    void* module_ = nullptr;
    std::filesystem::path path_;
    std::filesystem::path loadedPath_;
};
