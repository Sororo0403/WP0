#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct RecentProject {
    std::string name;
    std::filesystem::path manifestPath;
};

class RecentProjectsStore {
public:
    explicit RecentProjectsStore(std::filesystem::path path);

    [[nodiscard]] std::vector<RecentProject> Load() const;
    bool Record(const RecentProject& project) const;

private:
    std::filesystem::path path_;
};
