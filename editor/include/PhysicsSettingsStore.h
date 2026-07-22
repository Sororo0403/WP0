#pragma once

#include "world/PhysicsSettings.h"

#include <filesystem>
#include <string>

class PhysicsSettingsStore {
public:
    explicit PhysicsSettingsStore(std::filesystem::path path);

    bool Load(PhysicsSettings& settings, std::string& error) const;
    bool Save(const PhysicsSettings& settings, std::string& error) const;

    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    std::filesystem::path path_;
};
