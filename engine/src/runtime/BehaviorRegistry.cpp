#include "runtime/BehaviorRegistry.h"

#include <algorithm>
#include <utility>

bool BehaviorRegistry::Register(std::string type, Factory factory) {
    if (type.empty() || type.size() > 128u || type.find('\0') != std::string::npos ||
        !factory ||
        std::ranges::any_of(entries_, [&type](const Entry& entry) {
            return entry.type == type;
        })) {
        return false;
    }
    entries_.push_back({std::move(type), std::move(factory)});
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
