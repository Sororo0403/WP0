#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct EntityId {
    uint64_t high = 0;
    uint64_t low = 0;

    static EntityId New();
    static bool TryParse(std::string_view text, EntityId& result);

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::string ToString() const;

    friend bool operator==(EntityId lhs, EntityId rhs) noexcept = default;
};

struct EntityIdHash {
    [[nodiscard]] size_t operator()(EntityId id) const noexcept;
};
