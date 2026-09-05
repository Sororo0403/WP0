#include "runtime/BehaviorRegistry.h"

#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace {
bool IsBoundedString(const std::string& value, size_t maximumLength) {
    return value.size() <= maximumLength &&
           value.find('\0') == std::string::npos;
}

bool IsValidFloatDefinition(const ScriptPropertyDefinition& property) {
    return std::isfinite(property.defaultFloat) &&
           std::isfinite(property.minimumFloat) &&
           std::isfinite(property.maximumFloat) &&
           property.minimumFloat <= property.maximumFloat &&
           property.defaultFloat >= property.minimumFloat &&
           property.defaultFloat <= property.maximumFloat;
}

bool IsValidIntegerDefinition(const ScriptPropertyDefinition& property) {
    return property.minimumInteger <= property.maximumInteger &&
           property.defaultInteger >= property.minimumInteger &&
           property.defaultInteger <= property.maximumInteger;
}

bool IsValidVector3Definition(const ScriptPropertyDefinition& property) {
    return std::isfinite(property.defaultVector3.x) &&
           std::isfinite(property.defaultVector3.y) &&
           std::isfinite(property.defaultVector3.z);
}

bool IsValidInputActionKind(const ScriptPropertyDefinition& property) {
    if (property.inputActionKind < ScriptInputActionKind::Any ||
        property.inputActionKind > ScriptInputActionKind::Axis) {
        return false;
    }
    return property.type == ScriptPropertyType::InputAction ||
           property.inputActionKind == ScriptInputActionKind::Any;
}

bool IsValidDefaultValue(const ScriptPropertyDefinition& property) {
    switch (property.type) {
    case ScriptPropertyType::Float:
        return IsValidFloatDefinition(property);
    case ScriptPropertyType::Integer:
        return IsValidIntegerDefinition(property);
    case ScriptPropertyType::Vector3:
        return IsValidVector3Definition(property);
    case ScriptPropertyType::String:
    case ScriptPropertyType::AnimationClip:
    case ScriptPropertyType::Scene:
        return IsBoundedString(property.defaultString, 1024u);
    case ScriptPropertyType::InputAction:
        return IsBoundedString(property.defaultString, 64u);
    case ScriptPropertyType::Entity:
    case ScriptPropertyType::Boolean:
        return true;
    }
    return false;
}

bool IsValidPropertyDefinition(
    const ScriptPropertyDefinition& property,
    const std::vector<ScriptPropertyDefinition>& properties) {
    return !property.name.empty() && IsBoundedString(property.name, 128u) &&
           std::ranges::count(properties, property.name,
                              &ScriptPropertyDefinition::name) == 1 &&
           property.type >= ScriptPropertyType::Float &&
           property.type <= ScriptPropertyType::Scene &&
           IsValidDefaultValue(property) && IsValidInputActionKind(property);
}
} // namespace

bool BehaviorRegistry::Register(std::string type, Factory factory,
                                BehaviorRequirements requirements,
                                std::string sourceAsset,
                                std::vector<ScriptPropertyDefinition> properties) {
    const bool invalidProperty = std::ranges::any_of(
        properties, [&properties](const ScriptPropertyDefinition& property) {
            return !IsValidPropertyDefinition(property, properties);
        });
    if (type.empty() || !IsBoundedString(type, 128u) || !factory ||
        !IsBoundedString(sourceAsset, 1024u) || properties.size() > 128u ||
        invalidProperty ||
        std::ranges::any_of(entries_, [&type](const Entry& entry) {
            return entry.type == type;
        }) ||
        (!sourceAsset.empty() &&
         std::ranges::any_of(entries_, [&sourceAsset](const Entry& entry) {
             return entry.sourceAsset == sourceAsset;
        }))) {
        return false;
    }
    entries_.push_back({std::move(type), std::move(factory), requirements,
                        std::move(sourceAsset), std::move(properties)});
    return true;
}

std::unique_ptr<Behavior> BehaviorRegistry::Create(std::string_view type) const {
    const auto found = std::ranges::find(entries_, type, &Entry::type);
    return found == entries_.end() ? nullptr : found->factory();
}

std::vector<std::string_view> BehaviorRegistry::Types() const {
    std::vector<std::string_view> types;
    types.reserve(entries_.size());
    std::ranges::transform(entries_, std::back_inserter(types),
                           [](const Entry& entry) { return std::string_view(entry.type); });
    return types;
}

const BehaviorRequirements* BehaviorRegistry::Requirements(std::string_view type) const {
    const auto found = std::ranges::find(entries_, type, &Entry::type);
    return found == entries_.end() ? nullptr : &found->requirements;
}

std::string_view BehaviorRegistry::TypeFromSourceAsset(
    std::string_view sourceAsset) const {
    const auto found = std::ranges::find(entries_, sourceAsset, &Entry::sourceAsset);
    return found == entries_.end() ? std::string_view{} : std::string_view(found->type);
}

std::string_view BehaviorRegistry::SourceAsset(std::string_view type) const {
    const auto found = std::ranges::find(entries_, type, &Entry::type);
    return found == entries_.end() ? std::string_view{} :
                                     std::string_view(found->sourceAsset);
}

const std::vector<ScriptPropertyDefinition>* BehaviorRegistry::Properties(
    std::string_view type) const {
    const auto found = std::ranges::find(entries_, type, &Entry::type);
    return found == entries_.end() ? nullptr : &found->properties;
}

bool BehaviorRegistry::Configure(std::string_view type,
                                 const BehaviorComponent& component,
                                 Behavior& behavior) const {
    const std::vector<ScriptPropertyDefinition>* definitions = Properties(type);
    if (definitions == nullptr) {
        return false;
    }
    std::vector<ScriptPropertyValueView> values;
    values.reserve(definitions->size());
    for (const ScriptPropertyDefinition& definition : *definitions) {
        ScriptPropertyValueView value{};
        value.name = definition.name.c_str();
        value.type = definition.type;
        value.floatValue = definition.defaultFloat;
        value.booleanValue = definition.defaultBoolean;
        value.integerValue = definition.defaultInteger;
        value.vector3Value = definition.defaultVector3;
        value.stringValue = definition.defaultString.c_str();
        const auto stored = std::ranges::find(component.properties, definition.name,
                                              &ScriptPropertyValue::name);
        if (stored != component.properties.end() && stored->type == definition.type) {
            value.floatValue = stored->floatValue;
            value.entityValue = stored->entityValue;
            value.booleanValue = stored->booleanValue;
            value.integerValue = stored->integerValue;
            value.vector3Value = stored->vector3Value;
            value.stringValue = stored->stringValue.c_str();
        }
        values.push_back(value);
    }
    behavior.OnConfigure(values.data(), values.size());
    return true;
}

bool BehaviorRegistry::ValidateRequirements(std::string_view type,
                                            const WorldEntity& entity,
                                            std::string* error) const {
    const BehaviorRequirements* requirements = Requirements(type);
    if (requirements == nullptr) {
        if (error != nullptr) {
            *error = "Behavior type is not registered.";
        }
        return false;
    }
    if (requirements->characterController && !entity.characterController) {
        if (error != nullptr) {
            *error = "CharacterController component is required.";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool BehaviorRegistry::EnsureRequirements(std::string_view type,
                                          WorldEntity& entity) const {
    const BehaviorRequirements* requirements = Requirements(type);
    if (requirements == nullptr) {
        return false;
    }
    if (requirements->characterController && !entity.characterController) {
        entity.characterController = CharacterControllerComponent{};
    }
    return true;
}
