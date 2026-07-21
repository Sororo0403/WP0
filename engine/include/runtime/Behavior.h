#pragma once

#include "world/EntityId.h"

class World;

class Behavior {
public:
    virtual ~Behavior() = default;

    virtual void OnStart(World& world, EntityId entity) {
        (void)world;
        (void)entity;
    }
    virtual void OnUpdate(World& world, EntityId entity, float deltaTime) {
        (void)world;
        (void)entity;
        (void)deltaTime;
    }
    virtual void OnStop(World& world, EntityId entity) {
        (void)world;
        (void)entity;
    }
};
