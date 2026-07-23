#include "PlayerProjectValidator.h"

#include "ProjectDescriptor.h"
#include "world/World.h"
#include "world/WorldSerializer.h"

bool PlayerProjectValidator::Validate(const ProjectDescriptor& project,
                                      std::string& error) {
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(project.startupScene,
                                          filesystemError) ||
        filesystemError) {
        error = "Startup Scene was not found: " +
                project.startupScene.generic_string();
        return false;
    }
    World startupWorld;
    std::string sceneError;
    if (!WorldSerializer::Load(project.startupScene, startupWorld,
                               &sceneError)) {
        error = "Startup Scene is invalid: " + sceneError;
        return false;
    }
    return true;
}
