#pragma once

#include "runtime/ScriptProperty.h"
#include "world/EntityId.h"

class World;

class Behavior {
public:
    virtual ~Behavior() = default;

    virtual void OnConfigure(const ScriptPropertyValueView* properties, size_t count) {
        (void)properties;
        (void)count;
    }

    virtual void OnStart(World& world, EntityId entity) {
        (void)world;
        (void)entity;
    }
    virtual void OnUpdate(World& world, EntityId entity, float deltaTime) {
        (void)world;
        (void)entity;
        (void)deltaTime;
    }
    virtual void OnTriggerEnter(World& world, EntityId entity, EntityId other) {
        (void)world;
        (void)entity;
        (void)other;
    }
    virtual void OnTriggerStay(World& world, EntityId entity, EntityId other) {
        (void)world;
        (void)entity;
        (void)other;
    }
    virtual void OnTriggerExit(World& world, EntityId entity, EntityId other) {
        (void)world;
        (void)entity;
        (void)other;
    }
    virtual void OnButtonClick(World& world, EntityId entity) {
        (void)world;
        (void)entity;
    }
    virtual void OnToggleValueChanged(World& world, EntityId entity,
                                      bool isOn) {
        (void)world;
        (void)entity;
        (void)isOn;
    }
    virtual void OnSliderValueChanged(World& world, EntityId entity,
                                      float value) {
        (void)world;
        (void)entity;
        (void)value;
    }
    virtual void OnDropdownValueChanged(World& world, EntityId entity,
                                        int32_t value) {
        (void)world;
        (void)entity;
        (void)value;
    }
    virtual void OnInputFieldValueChanged(World& world, EntityId entity,
                                          const char* text) {
        (void)world;
        (void)entity;
        (void)text;
    }
    virtual void OnInputFieldSubmit(World& world, EntityId entity,
                                    const char* text) {
        (void)world;
        (void)entity;
        (void)text;
    }
    virtual void OnStop(World& world, EntityId entity) {
        (void)world;
        (void)entity;
    }
};
