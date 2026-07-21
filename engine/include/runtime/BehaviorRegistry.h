#pragma once

#include "runtime/Behavior.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class BehaviorRegistry {
public:
    using Factory = std::function<std::unique_ptr<Behavior>()>;

    bool Register(std::string type, Factory factory);
    [[nodiscard]] std::unique_ptr<Behavior> Create(std::string_view type) const;
    [[nodiscard]] std::vector<std::string_view> Types() const;

private:
    struct Entry {
        std::string type;
        Factory factory;
    };

    std::vector<Entry> entries_;
};
