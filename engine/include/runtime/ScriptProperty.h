#pragma once

#include "world/EntityId.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

enum class ScriptPropertyType : uint32_t {
    Float = 0,
    Entity = 1,
};

struct ScriptPropertyDescriptor {
    const char* name = nullptr;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float defaultFloat = 0.0f;
    float minimumFloat = 0.0f;
    float maximumFloat = 0.0f;
};

struct ScriptPropertyValueView {
    const char* name = nullptr;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float floatValue = 0.0f;
    EntityId entityValue{};
};

[[nodiscard]] inline const ScriptPropertyValueView* FindScriptProperty(
    const ScriptPropertyValueView* properties, size_t count, const char* name,
    ScriptPropertyType type) {
    if (properties == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t index = 0u; index < count; ++index) {
        const ScriptPropertyValueView& property = properties[index];
        if (property.name != nullptr && property.type == type &&
            std::strcmp(property.name, name) == 0) {
            return &property;
        }
    }
    return nullptr;
}
