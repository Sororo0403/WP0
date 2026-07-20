#pragma once

#include <filesystem>
#include <vector>

class RecentScenesStore {
public:
    RecentScenesStore(std::filesystem::path settingsPath,
                      std::filesystem::path sceneRoot);

    [[nodiscard]] std::vector<std::filesystem::path> Load() const;
    bool Save(const std::vector<std::filesystem::path>& scenes) const;

private:
    std::filesystem::path settingsPath_;
    std::filesystem::path sceneRoot_;
};
