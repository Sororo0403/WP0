#pragma once

#include "PlayerPackageBuilder.h"

#include <filesystem>
#include <string>

namespace PlayerPackageBuilderInternal {
inline constexpr wchar_t kPackageMarker[] = L".likeplayerpackage";

struct PackagePlan {
    std::filesystem::path destination;
    std::filesystem::path staging;
    bool replacingPackage = false;
};

bool PreparePackagePlan(const PlayerPackageRequest& request, PackagePlan& plan,
                        std::string& error);
bool StagePlayerPackage(const PlayerPackageRequest& request, const PackagePlan& plan,
                        std::string& error);
bool PublishPlayerPackage(const PackagePlan& plan, std::string& error);
void RemovePackageStaging(const PackagePlan& plan);
}  // namespace PlayerPackageBuilderInternal
