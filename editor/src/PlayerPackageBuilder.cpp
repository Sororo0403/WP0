#include "PlayerPackageBuilder.h"

#include "internal/PlayerPackageBuilderInternal.h"

bool PlayerPackageBuilder::Build(const PlayerPackageRequest& request, std::string& error) {
    using namespace PlayerPackageBuilderInternal;

    error.clear();
    PackagePlan plan;
    if (!PreparePackagePlan(request, plan, error)) {
        return false;
    }
    if (!StagePlayerPackage(request, plan, error)) {
        RemovePackageStaging(plan);
        return false;
    }
    if (!PublishPlayerPackage(plan, error)) {
        RemovePackageStaging(plan);
        return false;
    }
    return true;
}
