#pragma once

#include <filesystem>

struct EditorLayoutSettings {
    float leftWidthRatio = 0.22f;
    float rightWidthRatio = 0.24f;
    float bottomHeightRatio = 0.28f;
};

class EditorLayoutStore {
public:
    explicit EditorLayoutStore(std::filesystem::path settingsPath);

    [[nodiscard]] EditorLayoutSettings Load() const;
    [[nodiscard]] bool Save(const EditorLayoutSettings& settings) const;

    [[nodiscard]] static EditorLayoutSettings Normalize(EditorLayoutSettings settings);

private:
    std::filesystem::path settingsPath_;
};
