#include "runtime/Behavior.h"
#include "runtime/ScriptModuleApi.h"
#include "world/World.h"

#include <cmath>
#include <new>

namespace {
class Rotator final : public Behavior {
public:
    void OnUpdate(World& world, EntityId entity, float deltaTime) override;
};

void Rotator::OnUpdate(World& world, EntityId entity, float deltaTime) {
    WorldEntity* target = world.Find(entity);
    if (target == nullptr) {
        return;
    }
    target->transform.rotationDegrees.y =
        std::fmod(target->transform.rotationDegrees.y + 45.0f * deltaTime, 360.0f);
}

Behavior* CreateRotator(Input* input) {
    (void)input;
    return new (std::nothrow) Rotator();
}
}

ScriptTypeRegistration GetRotatorScriptRegistration() {
    return {"Rotator", "asset://Scripts/Rotator.cpp", &CreateRotator, {}};
}
