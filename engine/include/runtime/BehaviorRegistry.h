#pragma once

#include "runtime/Behavior.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct WorldEntity;

struct BehaviorRequirements {
    bool characterController = false;
};

class BehaviorRegistry {
public:
    using Factory = std::function<std::unique_ptr<Behavior>()>;

    bool Register(std::string type, Factory factory,
                  BehaviorRequirements requirements = {},
                  std::string sourceAsset = {});
    [[nodiscard]] std::unique_ptr<Behavior> Create(std::string_view type) const;
    [[nodiscard]] std::vector<std::string_view> Types() const;
    [[nodiscard]] const BehaviorRequirements* Requirements(std::string_view type) const;
    [[nodiscard]] std::string_view TypeFromSourceAsset(
        std::string_view sourceAsset) const;
    [[nodiscard]] std::string_view SourceAsset(std::string_view type) const;
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
    };

    std::vector<Entry> entries_;
};
