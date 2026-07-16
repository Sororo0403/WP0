#include "world/EntityId.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

namespace {
constexpr std::array<size_t, 4> kHyphenPositions{8u, 13u, 18u, 23u};

int HexValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

char HexDigit(uint8_t value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return kDigits[value & 0x0Fu];
}
} // namespace

EntityId EntityId::New() {
    GUID guid{};
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        static_assert(sizeof(guid) == sizeof(EntityId));
        EntityId result{};
        std::memcpy(&result, &guid, sizeof(result));
        return result;
    }

    static std::atomic<uint64_t> fallbackCounter{1u};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return {static_cast<uint64_t>(now), fallbackCounter.fetch_add(1u)};
}

bool EntityId::TryParse(std::string_view text, EntityId& result) {
    if (text.size() != 36u) {
        return false;
    }
    for (size_t position : kHyphenPositions) {
        if (text[position] != '-') {
            return false;
        }
    }

    EntityId parsed{};
    size_t nibble = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '-') {
            continue;
        }
        const int value = HexValue(text[index]);
        if (value < 0) {
            return false;
        }
        uint64_t& target = nibble < 16u ? parsed.high : parsed.low;
        target = (target << 4u) | static_cast<uint64_t>(value);
        ++nibble;
    }
    if (nibble != 32u || !parsed.IsValid()) {
        return false;
    }
    result = parsed;
    return true;
}

bool EntityId::IsValid() const noexcept {
    return high != 0u || low != 0u;
}

std::string EntityId::ToString() const {
    std::string result(36u, '-');
    size_t outputIndex = 0;
    for (size_t nibble = 0; nibble < 32u; ++nibble) {
        if (outputIndex == 8u || outputIndex == 13u || outputIndex == 18u ||
            outputIndex == 23u) {
            ++outputIndex;
        }
        const uint64_t source = nibble < 16u ? high : low;
        const size_t localNibble = nibble < 16u ? nibble : nibble - 16u;
        const auto shift = static_cast<unsigned int>((15u - localNibble) * 4u);
        result[outputIndex++] = HexDigit(static_cast<uint8_t>((source >> shift) & 0x0Fu));
    }
    return result;
}

size_t EntityIdHash::operator()(EntityId id) const noexcept {
    const uint64_t mixed = id.high ^ (id.low + 0x9e3779b97f4a7c15ull + (id.high << 6u) +
                                      (id.high >> 2u));
    return static_cast<size_t>(mixed);
}
