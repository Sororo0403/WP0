#pragma once

#include "runtime/Behavior.h"

class Input;

class FirstPersonController final : public Behavior {
public:
    explicit FirstPersonController(Input* input);

    void OnUpdate(World& world, EntityId entity, float deltaTime) override;

private:
    Input* input_ = nullptr;
};
