#include "runtime/BehaviorRegistry.h"

#include "world/World.h"

#include <algorithm>
#include <utility>

bool BehaviorRegistry::Register(std::string type, Factory factory,
                                BehaviorRequirements requirements) {
    if (type.empty() || type.size() > 128u || type.find('\0') != std::string::npos ||
        !factory ||
        std::ranges::any_of(entries_, [&type](const Entry& entry) {
            return entry.type == type;
        })) {
        return false;
    }
    entries_.push_back({std::move(type), std::move(factory), requirements});
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
