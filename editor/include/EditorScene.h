#pragma once

#include "scene/BaseScene.h"

#include <functional>

class EditorScene final : public BaseScene {
public:
    explicit EditorScene(std::function<void()> requestClose);

    void Update() override;
    void Draw() override;
    void DrawPostProcessOverlay() override;

private:
    void DrawMainMenu();
    void DrawPanels();
    static void BeginFixedPanel(const char* name, float x, float y, float width, float height);

    std::function<void()> requestClose_;
};
