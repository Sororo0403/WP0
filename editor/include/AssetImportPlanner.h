#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace AssetImport {

struct File {
    std::filesystem::path source;
    std::filesystem::path relativeDestination;
};

[[nodiscard]] bool IsModelFile(const std::filesystem::path& path);
[[nodiscard]] bool IsTextureFile(const std::filesystem::path& path);
[[nodiscard]] bool IsAudioFile(const std::filesystem::path& path);
[[nodiscard]] bool IsSelectableFile(const std::filesystem::path& path);
bool BuildPlan(const std::vector<std::filesystem::path>& selectedFiles,
               std::vector<File>& files, std::string& errorMessage);
[[nodiscard]] bool HaveEqualContents(const std::filesystem::path& left,
                                     const std::filesystem::path& right);

} // namespace AssetImport
