#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeFloatProperty(const Json& encodedValue, ScriptPropertyValue& property,
                         std::string* error) {
    if (!encodedValue.is_number()) {
        SetError(error, "Scene Script float property is invalid.");
        return false;
    }
    property.type = ScriptPropertyType::Float;
    property.floatValue = encodedValue.get<float>();
    if (!std::isfinite(property.floatValue)) {
        SetError(error, "Scene Script float property is invalid.");
        return false;
    }
    return true;
}

bool DecodeEntityProperty(const Json& encodedValue, ScriptPropertyValue& property,
                          std::string* error) {
    property.type = ScriptPropertyType::Entity;
    if (encodedValue.is_null()) {
        return true;
    }
    if (!encodedValue.is_string() ||
        !EntityId::TryParse(encodedValue.get_ref<const std::string&>(),
                            property.entityValue)) {
        SetError(error, "Scene Script Entity property is invalid.");
        return false;
    }
    return true;
}

bool DecodeIntegerProperty(const Json& encodedValue, ScriptPropertyValue& property,
                           std::string* error) {
    if (!encodedValue.is_number_integer() && !encodedValue.is_number_unsigned()) {
        SetError(error, "Scene Script Integer property is invalid.");
        return false;
    }
    const bool unsignedOverflow =
        encodedValue.is_number_unsigned() &&
        encodedValue.get<uint64_t>() >
            static_cast<uint64_t>((std::numeric_limits<int32_t>::max)());
    const bool signedOverflow =
        encodedValue.is_number_integer() && !encodedValue.is_number_unsigned() &&
        (encodedValue.get<int64_t>() <
             static_cast<int64_t>((std::numeric_limits<int32_t>::min)()) ||
         encodedValue.get<int64_t>() >
             static_cast<int64_t>((std::numeric_limits<int32_t>::max)()));
    if (unsignedOverflow || signedOverflow) {
        SetError(error, "Scene Script Integer property is invalid.");
        return false;
    }
    property.type = ScriptPropertyType::Integer;
    property.integerValue = encodedValue.get<int32_t>();
    return true;
}

bool TryResolveStringPropertyType(std::string_view type,
                                  ScriptPropertyType& resolved) {
    if (type == "String") {
        resolved = ScriptPropertyType::String;
    } else if (type == "AnimationClip") {
        resolved = ScriptPropertyType::AnimationClip;
    } else if (type == "InputAction") {
        resolved = ScriptPropertyType::InputAction;
    } else if (type == "Scene") {
        resolved = ScriptPropertyType::Scene;
    } else {
        return false;
    }
    return true;
}

bool DecodeStringProperty(std::string_view type, const Json& encodedValue,
                          ScriptPropertyValue& property, std::string* error) {
    if (!encodedValue.is_string() ||
        !TryResolveStringPropertyType(type, property.type)) {
        SetError(error, "Scene Script string property is invalid.");
        return false;
    }
    property.stringValue = encodedValue.get<std::string>();
    const size_t maximumLength =
        property.type == ScriptPropertyType::InputAction ? 64u : 1024u;
    if (property.stringValue.size() > maximumLength ||
        property.stringValue.find('\0') != std::string::npos) {
        SetError(error, "Scene Script string property is invalid.");
        return false;
    }
    return true;
}

bool DecodePropertyValue(std::string_view type, const Json& encodedValue,
                         ScriptPropertyValue& property, std::string* error) {
    if (type == "Float") {
        return DecodeFloatProperty(encodedValue, property, error);
    }
    if (type == "Entity") {
        return DecodeEntityProperty(encodedValue, property, error);
    }
    if (type == "Boolean") {
        if (!encodedValue.is_boolean()) {
            SetError(error, "Scene Script Boolean property is invalid.");
            return false;
        }
        property.type = ScriptPropertyType::Boolean;
        property.booleanValue = encodedValue.get<bool>();
        return true;
    }
    if (type == "Integer") {
        return DecodeIntegerProperty(encodedValue, property, error);
    }
    if (type == "Vector3") {
        DirectX::XMFLOAT3 value{};
        if (!DecodeFloat3(encodedValue, value)) {
            SetError(error, "Scene Script Vector3 property is invalid.");
            return false;
        }
        property.type = ScriptPropertyType::Vector3;
        property.vector3Value = {value.x, value.y, value.z};
        return true;
    }
    if (TryResolveStringPropertyType(type, property.type)) {
        return DecodeStringProperty(type, encodedValue, property, error);
    }
    SetError(error, "Scene Script property type is invalid.");
    return false;
}

bool DecodeScriptProperty(std::string_view name, const Json& encodedProperty,
                          ScriptPropertyValue& property, std::string* error) {
    if (name.empty() || name.size() > 128u || name.find('\0') != std::string_view::npos ||
        !encodedProperty.is_object() || !encodedProperty.contains("type") ||
        !encodedProperty["type"].is_string() || !encodedProperty.contains("value")) {
        SetError(error, "Scene Script property is invalid.");
        return false;
    }

    property.name = name;
    const Json& encodedValue = encodedProperty["value"];
    const std::string propertyType = encodedProperty["type"].get<std::string>();
    return DecodePropertyValue(propertyType, encodedValue, property, error);
}

bool HasValidScriptHeader(const Json& encodedScript) {
    return encodedScript.is_object() && encodedScript.contains("enabled") &&
           encodedScript["enabled"].is_boolean() &&
           encodedScript.contains("type") && encodedScript["type"].is_string();
}

bool HasValidScriptIdentity(const BehaviorComponent& component,
                            bool allowUnassigned) {
    return (allowUnassigned || !component.type.empty()) &&
           component.type.size() <= 128u &&
           component.type.find('\0') == std::string::npos &&
           component.scriptAssetPath.size() <= 1024u &&
           component.scriptAssetPath.find('\0') == std::string::npos &&
           (!component.type.empty() ||
            (component.scriptAssetPath.empty() && component.properties.empty()));
}

bool DecodeScript(const Json& encodedScript, BehaviorComponent& component,
                  bool allowUnassigned, std::string* error) {
    if (!HasValidScriptHeader(encodedScript)) {
        SetError(error, "Scene Script component is invalid.");
        return false;
    }
    component.enabled = encodedScript["enabled"].get<bool>();
    component.type = encodedScript["type"].get<std::string>();
    if (encodedScript.contains("script")) {
        if (!encodedScript["script"].is_string()) {
            SetError(error, "Scene Behavior script asset path is invalid.");
            return false;
        }
        component.scriptAssetPath = encodedScript["script"].get<std::string>();
    }
    if (encodedScript.contains("properties")) {
        const Json& properties = encodedScript["properties"];
        if (!properties.is_object() || properties.size() > 128u) {
            SetError(error, "Scene Script properties are invalid.");
            return false;
        }
        component.properties.reserve(properties.size());
        for (const auto& [name, encodedProperty] : properties.items()) {
            ScriptPropertyValue property{};
            if (!DecodeScriptProperty(name, encodedProperty, property, error)) {
                return false;
            }
            component.properties.push_back(std::move(property));
        }
    }
    if (!HasValidScriptIdentity(component, allowUnassigned)) {
        SetError(error, "Scene Script type is invalid.");
        return false;
    }
    return true;
}

} // namespace

bool DecodeScriptsComponent(const Json& components, WorldEntity& entity,
                            std::string* error) {
    if (components.contains("Scripts")) {
        const Json& scripts = components["Scripts"];
        if (!scripts.is_array() || scripts.size() > 1024u) {
            SetError(error, "Scene Scripts component list is invalid.");
            return false;
        }
        entity.scripts.reserve(scripts.size());
        for (const Json& encodedScript : scripts) {
            BehaviorComponent component{};
            if (!DecodeScript(encodedScript, component, true, error)) {
                return false;
            }
            entity.scripts.push_back(std::move(component));
        }
    } else if (components.contains("Behavior")) {
        BehaviorComponent component{};
        if (!DecodeScript(components["Behavior"], component, false, error)) {
            return false;
        }
        entity.scripts.push_back(std::move(component));
    }
    return true;
}

} // namespace WorldSerializerDecoding
