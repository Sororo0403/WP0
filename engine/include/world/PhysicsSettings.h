#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct PhysicsSettings {
    static constexpr size_t kLayerCount = 32u;

    std::array<std::string, kLayerCount> layerNames{};
    std::array<uint32_t, kLayerCount> collisionMasks{};

    [[nodiscard]] static PhysicsSettings Defaults() {
        PhysicsSettings settings{};
        settings.layerNames[0] = "Default";
        settings.collisionMasks.fill(0xffffffffu);
        return settings;
    }

    [[nodiscard]] bool LayersCollide(size_t first, size_t second) const {
        return first < kLayerCount && second < kLayerCount &&
               (collisionMasks[first] & (uint32_t{1u} << second)) != 0u;
    }

    void SetLayersCollide(size_t first, size_t second, bool collide) {
        if (first >= kLayerCount || second >= kLayerCount) {
            return;
        }
        const uint32_t secondBit = uint32_t{1u} << second;
        const uint32_t firstBit = uint32_t{1u} << first;
        if (collide) {
            collisionMasks[first] |= secondBit;
            collisionMasks[second] |= firstBit;
        } else {
            collisionMasks[first] &= ~secondBit;
            collisionMasks[second] &= ~firstBit;
        }
    }
};
