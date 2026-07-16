#pragma once

#include <filesystem>
#include <string>
#include <string_view>

class World;

class WorldSerializer {
public:
    static std::string Serialize(const World& world);
    static bool Deserialize(std::string_view text, World& world, std::string* error = nullptr);
    static bool Save(const World& world, const std::filesystem::path& path,
                     std::string* error = nullptr);
    static bool Load(const std::filesystem::path& path, World& world,
                     std::string* error = nullptr);
};
