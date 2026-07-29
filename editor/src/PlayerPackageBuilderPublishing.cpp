#include "internal/PlayerPackageBuilderInternal.h"

#include <system_error>

namespace PlayerPackageBuilderInternal {
namespace {
bool PublishNewPackage(const PackagePlan& plan, std::string& error) {
    std::error_code filesystemError;
    std::filesystem::rename(plan.staging, plan.destination, filesystemError);
    if (filesystemError) {
        error = "Could not finish the Player package.";
        return false;
    }
    return true;
}

bool RestorePreviousPackage(const PackagePlan& plan,
                            const std::filesystem::path& previous,
                            std::string& error) {
    std::error_code restoreError;
    std::filesystem::rename(previous, plan.destination, restoreError);
    if (restoreError) {
        error = "Could not publish the Player package or restore the previous package.";
        return false;
    }
    error = "Could not publish the updated Player package.";
    return false;
}

bool PublishReplacementPackage(const PackagePlan& plan, std::string& error) {
    const std::filesystem::path previous = plan.destination.wstring() + L".previous";
    std::error_code filesystemError;
    if (std::filesystem::exists(previous, filesystemError) || filesystemError) {
        error = "A previous Player package backup still exists.";
        return false;
    }
    std::filesystem::rename(plan.destination, previous, filesystemError);
    if (filesystemError) {
        error = "Could not replace the existing Player package.";
        return false;
    }
    std::filesystem::rename(plan.staging, plan.destination, filesystemError);
    if (filesystemError) {
        return RestorePreviousPackage(plan, previous, error);
    }
    std::error_code cleanupError;
    std::filesystem::remove_all(previous, cleanupError);
    if (cleanupError) {
        error = "Player package was updated, but its previous backup could not be removed.";
    }
    return true;
}
}  // namespace

bool PublishPlayerPackage(const PackagePlan& plan, std::string& error) {
    return plan.replacingPackage ? PublishReplacementPackage(plan, error)
                                 : PublishNewPackage(plan, error);
}

void RemovePackageStaging(const PackagePlan& plan) {
    std::error_code cleanupError;
    std::filesystem::remove_all(plan.staging, cleanupError);
}
}  // namespace PlayerPackageBuilderInternal
