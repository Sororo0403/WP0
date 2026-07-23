#pragma once

#include "world/EntityId.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

enum class ScriptPropertyType : uint32_t {
    Float = 0,
    Entity = 1,
    Boolean = 2,
    Integer = 3,
    Vector3 = 4,
    String = 5,
};

struct ScriptVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ScriptPropertyDescriptor {
    const char* name = nullptr;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float defaultFloat = 0.0f;
    float minimumFloat = 0.0f;
    float maximumFloat = 0.0f;
    bool defaultBoolean = false;
    int32_t defaultInteger = 0;
    int32_t minimumInteger = (std::numeric_limits<int32_t>::min)();
    int32_t maximumInteger = (std::numeric_limits<int32_t>::max)();
    ScriptVector3 defaultVector3{};
    const char* defaultString = nullptr;
};

struct ScriptPropertyValueView {
    const char* name = nullptr;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float floatValue = 0.0f;
    EntityId entityValue{};
    bool booleanValue = false;
    int32_t integerValue = 0;
    ScriptVector3 vector3Value{};
    const char* stringValue = nullptr;
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
