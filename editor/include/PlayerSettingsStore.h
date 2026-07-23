#pragma once

#include <filesystem>
#include <string>

struct PlayerSettings {
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
};

class PlayerSettingsStore {
public:
    explicit PlayerSettingsStore(std::filesystem::path path);

    bool Load(PlayerSettings& settings, std::string& error) const;
    bool Save(const PlayerSettings& settings, std::string& error) const;

    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    std::filesystem::path path_;
};
