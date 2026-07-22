#include "runtime/BehaviorRegistry.h"

#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <utility>

bool BehaviorRegistry::Register(std::string type, Factory factory,
                                BehaviorRequirements requirements,
                                std::string sourceAsset,
                                std::vector<ScriptPropertyDefinition> properties) {
    const bool invalidProperty = std::ranges::any_of(
        properties, [&properties](const ScriptPropertyDefinition& property) {
            const bool duplicate = std::ranges::count(
                                       properties, property.name,
                                       &ScriptPropertyDefinition::name) != 1;
            const bool invalidFloat =
                property.type == ScriptPropertyType::Float &&
                (!std::isfinite(property.defaultFloat) ||
                 !std::isfinite(property.minimumFloat) ||
                 !std::isfinite(property.maximumFloat) ||
                 property.minimumFloat > property.maximumFloat ||
                 property.defaultFloat < property.minimumFloat ||
                 property.defaultFloat > property.maximumFloat);
            const bool invalidInteger =
                property.type == ScriptPropertyType::Integer &&
                (property.minimumInteger > property.maximumInteger ||
                 property.defaultInteger < property.minimumInteger ||
                 property.defaultInteger > property.maximumInteger);
            const bool invalidVector3 =
                property.type == ScriptPropertyType::Vector3 &&
                (!std::isfinite(property.defaultVector3.x) ||
                 !std::isfinite(property.defaultVector3.y) ||
                 !std::isfinite(property.defaultVector3.z));
            return property.name.empty() || property.name.size() > 128u ||
                   property.name.find('\0') != std::string::npos || duplicate ||
                   property.type < ScriptPropertyType::Float ||
                   property.type > ScriptPropertyType::Vector3 || invalidFloat ||
                   invalidInteger || invalidVector3;
        });
    if (type.empty() || type.size() > 128u || type.find('\0') != std::string::npos ||
        !factory || sourceAsset.size() > 1024u ||
        sourceAsset.find('\0') != std::string::npos ||
        properties.size() > 128u || invalidProperty ||
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
    for (const Entry& entry : entries_) {
        types.push_back(entry.type);
    }
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
        const auto stored = std::ranges::find(component.properties, definition.name,
                                              &ScriptPropertyValue::name);
        if (stored != component.properties.end() && stored->type == definition.type) {
            value.floatValue = stored->floatValue;
            value.entityValue = stored->entityValue;
            value.booleanValue = stored->booleanValue;
            value.integerValue = stored->integerValue;
            value.vector3Value = stored->vector3Value;
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
