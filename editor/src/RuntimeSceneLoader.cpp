#include "RuntimeSceneLoader.h"

#include "world/World.h"
#include "world/WorldSerializer.h"

#include <algorithm>
#include <cwctype>

namespace {
bool IsSafeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    const std::filesystem::path parent(L"..");
    return std::none_of(path.begin(), path.end(),
                        [&parent](const std::filesystem::path& part) {
                            return part == parent;
                        });
}

bool IsInside(const std::filesystem::path& root,
              const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(path, root, error);
    return !error && IsSafeRelative(relative);
}
} // namespace

bool RuntimeSceneLoader::Load(const std::filesystem::path& sceneRoot,
                              std::string_view request,
                              const PhysicsSettings& physicsSettings,
                              World& world,
                              std::filesystem::path& loadedPath,
                              std::string& error) {
    error.clear();
    if (request.empty() || request.size() > 1024u ||
        request.find('\0') != std::string_view::npos) {
        error = "Runtime Scene name is empty or too long.";
        return false;
    }
    std::filesystem::path relative;
    try {
        std::u8string utf8Request;
        utf8Request.reserve(request.size());
        for (const unsigned char character : request) {
            utf8Request.push_back(static_cast<char8_t>(character));
        }
        relative = std::filesystem::path(utf8Request).lexically_normal();
    } catch (const std::exception&) {
        error = "Runtime Scene name is invalid.";
        return false;
    }
    if (relative.extension().empty()) {
        relative += L".likescene";
    }
    std::wstring extension = relative.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    if (!IsSafeRelative(relative) || extension != L".likescene") {
        error = "Runtime Scene must be a relative .likescene path.";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path root =
        std::filesystem::absolute(sceneRoot, filesystemError).lexically_normal();
    const std::filesystem::path scene =
        std::filesystem::absolute(root / relative, filesystemError)
            .lexically_normal();
    if (filesystemError || !IsInside(root, scene) ||
        !std::filesystem::is_regular_file(scene, filesystemError) ||
        filesystemError) {
        error = "Runtime Scene was not found: " + relative.generic_string();
        return false;
    }
    World loaded;
    if (!WorldSerializer::Load(scene, loaded, &error)) {
        error = "Runtime Scene is invalid: " + error;
        return false;
    }
    loaded.SetPhysicsSettings(physicsSettings);
    world = std::move(loaded);
    loadedPath = scene;
    error.clear();
    return true;
}
