#include "core/AssetManager.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string_view>
#include <system_error>

namespace {

std::filesystem::path SafeCurrentPath() {
    std::error_code ec;
    try {
        const std::filesystem::path path = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(L".") : path;
    } catch (const std::exception&) {
        return std::filesystem::path(L".");
    }
}

std::filesystem::path gAssetRoot;
std::filesystem::path gEngineResourceRoot;
std::filesystem::path gProjectAssetRoot;
std::filesystem::path gUserDataRoot;
std::mutex gAssetRootMutex;

bool StripScheme(const std::filesystem::path& path, std::wstring_view scheme,
                 std::filesystem::path& relative) {
    try {
        std::wstring value = path.generic_wstring();
        const std::wstring prefix = std::wstring(scheme) + L"://";
        if (value.rfind(prefix, 0u) != 0u) {
            return false;
        }
        relative = std::filesystem::path(value.substr(prefix.size())).lexically_normal();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::filesystem::path CanonicalizePath(const std::filesystem::path& path) {
    std::error_code ec;
    try {
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec) {
            return canonical;
        }
        return path.lexically_normal();
    } catch (const std::exception&) {
        return path;
    }
}

std::filesystem::path ResolveRoot(const std::filesystem::path& path) {
    try {
        return CanonicalizePath(path.is_absolute() ? path : SafeCurrentPath() / path);
    } catch (const std::exception&) {
        return CanonicalizePath(SafeCurrentPath());
    }
}

bool HasParentTraversal(const std::filesystem::path& path) {
    const std::filesystem::path parent(L"..");
    try {
        return std::any_of(path.begin(), path.end(),
                           [&parent](const std::filesystem::path& part) { return part == parent; });
    } catch (const std::exception&) {
        return true;
    }
}

bool IsWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    try {
        const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
        return !ec && !relative.is_absolute() && !HasParentTraversal(relative);
    } catch (const std::exception&) {
        return false;
    }
}

std::filesystem::path ResolveMountedPath(const std::filesystem::path& root,
                                         const std::filesystem::path& relative) {
    if (root.empty() || relative.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory() ||
        HasParentTraversal(relative)) {
        return {};
    }
    const std::filesystem::path canonicalRoot = CanonicalizePath(root);
    const std::filesystem::path resolved = CanonicalizePath(canonicalRoot / relative);
    return IsWithinRoot(canonicalRoot, resolved) ? resolved : std::filesystem::path{};
}

bool ExistsNoThrow(const std::filesystem::path& path) {
    std::error_code ec;
    try {
        return std::filesystem::exists(path, ec);
    } catch (const std::exception&) {
        return false;
    }
}

bool LooksLikeRepositoryAssetRoot(const std::filesystem::path& path) {
    try {
        return ExistsNoThrow(path / L"engine" / L"resources") &&
               (ExistsNoThrow(path / L"build.bat") || ExistsNoThrow(path / L"build.ps1") ||
                ExistsNoThrow(path / L"build.cmd"));
    } catch (const std::exception&) {
        return false;
    }
}

bool HasLocalResources(const std::filesystem::path& path) {
    try {
        return ExistsNoThrow(path / L"resources");
    } catch (const std::exception&) {
        return false;
    }
}

template <typename Predicate>
std::filesystem::path FindAncestor(const std::filesystem::path& start, Predicate predicate) {
    try {
        for (std::filesystem::path dir = start; !dir.empty(); dir = dir.parent_path()) {
            if (predicate(dir)) {
                return CanonicalizePath(dir);
            }

            if (dir == dir.root_path()) {
                break;
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    return {};
}

std::filesystem::path FindDefaultAssetRoot() {
    const std::filesystem::path start = ResolveRoot(SafeCurrentPath());
    if (const std::filesystem::path repoRoot = FindAncestor(start, LooksLikeRepositoryAssetRoot);
        !repoRoot.empty()) {
        return repoRoot;
    }
    if (const std::filesystem::path localResourceRoot = FindAncestor(start, HasLocalResources);
        !localResourceRoot.empty()) {
        return localResourceRoot;
    }
    return start;
}

} // namespace

void AssetManager::SetAssetRoot(std::filesystem::path assetRoot) {
    const std::filesystem::path resolvedRoot = ResolveRoot(assetRoot);
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    try {
        gAssetRoot = resolvedRoot;
    } catch (const std::exception&) {
        gAssetRoot.clear();
    }
}

void AssetManager::SetEngineResourceRoot(std::filesystem::path root) {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    gEngineResourceRoot = ResolveRoot(root);
}

void AssetManager::SetProjectAssetRoot(std::filesystem::path root) {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    gProjectAssetRoot = ResolveRoot(root);
}

void AssetManager::SetUserDataRoot(std::filesystem::path root) {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    gUserDataRoot = ResolveRoot(root);
}

std::filesystem::path AssetManager::GetEngineResourceRoot() {
    {
        std::lock_guard<std::mutex> lock(gAssetRootMutex);
        if (!gEngineResourceRoot.empty()) {
            return gEngineResourceRoot;
        }
    }
    return GetAssetRoot() / L"engine" / L"resources";
}

std::filesystem::path AssetManager::GetProjectAssetRoot() {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    return gProjectAssetRoot;
}

std::filesystem::path AssetManager::GetUserDataRoot() {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    return gUserDataRoot;
}

std::filesystem::path AssetManager::GetAssetRoot() {
    std::lock_guard<std::mutex> lock(gAssetRootMutex);
    if (gAssetRoot.empty()) {
        try {
            gAssetRoot = FindDefaultAssetRoot();
        } catch (const std::exception&) {
            gAssetRoot = std::filesystem::path(L".");
        }
    }
    return gAssetRoot;
}

std::filesystem::path AssetManager::ResolvePath(const std::filesystem::path& relativePath) {
    std::filesystem::path uriPath;
    if (StripScheme(relativePath, L"engine", uriPath)) {
        return ResolveMountedPath(GetEngineResourceRoot(), uriPath);
    }
    if (StripScheme(relativePath, L"asset", uriPath)) {
        return ResolvePathStrict(relativePath);
    }
    if (StripScheme(relativePath, L"user", uriPath)) {
        return ResolveMountedPath(GetUserDataRoot(), uriPath);
    }
    std::filesystem::path normalized;
    try {
        normalized = relativePath.lexically_normal();
    } catch (const std::exception&) {
        return GetAssetRoot();
    }
    if (normalized.is_absolute()) {
        return Canonicalize(normalized);
    }

    const std::filesystem::path assetRoot = GetAssetRoot();

    std::filesystem::path rooted;
    try {
        rooted = assetRoot / normalized;
    } catch (const std::exception&) {
        return assetRoot;
    }
    if (ExistsNoThrow(rooted)) {
        return Canonicalize(rooted);
    }

    try {
        for (std::filesystem::path dir = assetRoot; !dir.empty(); dir = dir.parent_path()) {
            const std::filesystem::path candidate = dir / normalized;
            if (ExistsNoThrow(candidate)) {
                return Canonicalize(candidate);
            }

            if (dir == dir.root_path()) {
                break;
            }
        }
    } catch (const std::exception&) {
        return Canonicalize(rooted);
    }

    return Canonicalize(rooted);
}

std::filesystem::path AssetManager::ResolvePathStrict(const std::filesystem::path& relativePath) {
    if (relativePath.empty()) {
        return {};
    }

    const std::filesystem::path assetRoot = Canonicalize(GetProjectAssetRoot());
    if (assetRoot.empty()) {
        return {};
    }
    std::filesystem::path normalized;
    try {
        if (!StripScheme(relativePath, L"asset", normalized)) {
            normalized = relativePath.lexically_normal();
            if (normalized.begin() != normalized.end() && *normalized.begin() == L"assets") {
                normalized = normalized.lexically_relative(L"assets");
            }
        }
    } catch (const std::exception&) {
        return {};
    }

    if (normalized.is_absolute()) {
        const std::filesystem::path canonical = Canonicalize(normalized);
        return IsWithinRoot(assetRoot, canonical) ? canonical : std::filesystem::path{};
    }

    if (normalized.has_root_name() || normalized.has_root_directory() ||
        HasParentTraversal(normalized)) {
        return {};
    }

    std::filesystem::path resolvedPath;
    try {
        resolvedPath = Canonicalize(assetRoot / normalized);
    } catch (const std::exception&) {
        return {};
    }
    return IsWithinRoot(assetRoot, resolvedPath) ? resolvedPath : std::filesystem::path{};
}

std::filesystem::path AssetManager::Canonicalize(const std::filesystem::path& path) {
    return CanonicalizePath(path);
}
