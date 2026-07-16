#pragma once

#include "scene/BaseScene.h"
#include "world/World.h"

#include <filesystem>
#include <functional>
#include <string>

class EditorScene final : public BaseScene {
public:
    explicit EditorScene(std::function<void()> requestClose);

    void Update() override;
    void Draw() override;
    void DrawPostProcessOverlay() override;

private:
    void DrawMainMenu();
    void DrawPanels();
    void DrawHierarchyPanel();
    void DrawEntityNode(EntityId id);
    void DrawInspectorPanel();
    void NewScene();
    void SaveScene();
    void LoadScene();
    static void BeginFixedPanel(const char* name, float x, float y, float width, float height);

    std::function<void()> requestClose_;
    World world_;
    EntityId selection_{};
    std::filesystem::path scenePath_ = L"Assets/Scenes/Untitled.wp0scene";
    std::string status_ = "Editor session started.";
    bool dirty_ = false;
};
