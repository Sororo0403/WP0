#pragma once

#include "input/Input.h"

#include <filesystem>
#include <string>

class InputSettingsStore {
public:
    explicit InputSettingsStore(std::filesystem::path path);

    bool Load(Input& input, std::string& error) const;
    bool Save(const Input& input, std::string& error) const;

    [[nodiscard]] const std::filesystem::path& Path() const;

private:
    std::filesystem::path path_;
};
