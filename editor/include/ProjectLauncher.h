#pragma once

#include "RecentProjectsStore.h"

#include <filesystem>
#include <optional>
#include <vector>

class ProjectLauncher {
public:
    static std::optional<std::filesystem::path>
    ChooseProject(const std::vector<RecentProject>& recentProjects);
};
