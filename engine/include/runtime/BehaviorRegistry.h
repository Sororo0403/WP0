#pragma once

#include "runtime/Behavior.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct WorldEntity;
struct BehaviorComponent;

struct BehaviorRequirements {
    bool characterController = false;
};

struct ScriptPropertyDefinition {
    std::string name;
    ScriptPropertyType type = ScriptPropertyType::Float;
    float defaultFloat = 0.0f;
    float minimumFloat = 0.0f;
    float maximumFloat = 0.0f;
    bool defaultBoolean = false;
    int32_t defaultInteger = 0;
    int32_t minimumInteger = (std::numeric_limits<int32_t>::min)();
    int32_t maximumInteger = (std::numeric_limits<int32_t>::max)();
    ScriptVector3 defaultVector3{};
    std::string defaultString;
    ScriptInputActionKind inputActionKind = ScriptInputActionKind::Any;
};

class BehaviorRegistry {
public:
    using Factory = std::function<std::unique_ptr<Behavior>()>;

    bool Register(std::string type, Factory factory,
                  BehaviorRequirements requirements = {},
                  std::string sourceAsset = {},
                  std::vector<ScriptPropertyDefinition> properties = {});
    [[nodiscard]] std::unique_ptr<Behavior> Create(std::string_view type) const;
    [[nodiscard]] std::vector<std::string_view> Types() const;
    [[nodiscard]] const BehaviorRequirements* Requirements(std::string_view type) const;
    [[nodiscard]] std::string_view TypeFromSourceAsset(
        std::string_view sourceAsset) const;
    [[nodiscard]] std::string_view SourceAsset(std::string_view type) const;
    [[nodiscard]] const std::vector<ScriptPropertyDefinition>* Properties(
        std::string_view type) const;
    [[nodiscard]] bool Configure(std::string_view type,
                                 const BehaviorComponent& component,
                                 Behavior& behavior) const;
    [[nodiscard]] bool ValidateRequirements(std::string_view type,
                                            const WorldEntity& entity,
                                            std::string* error = nullptr) const;
    [[nodiscard]] bool EnsureRequirements(std::string_view type,
                                          WorldEntity& entity) const;

private:
    struct Entry {
        std::string type;
        Factory factory;
        BehaviorRequirements requirements{};
        std::string sourceAsset;
        std::vector<ScriptPropertyDefinition> properties;
    };

    std::vector<Entry> entries_;
};
