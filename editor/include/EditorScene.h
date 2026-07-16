#pragma once

#include "camera/Camera.h"
#include "graphics/PostProcessSystem.h"
#include "graphics/RenderSurface.h"
#include "graphics/SceneRenderer.h"
#include "graphics/RenderScene.h"
#include "scene/BaseScene.h"
#include "world/World.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

class EditorScene final : public BaseScene {
public:
    explicit EditorScene(std::function<void()> requestClose);

    void Initialize(const SceneContext& ctx) override;
    void Update() override;
    void Draw() override;
    void DrawPostProcessOverlay() override;

private:
    void DrawMainMenu();
    void DrawPanels();
    void DrawHierarchyPanel();
    void DrawEntityNode(EntityId id);
    void DrawInspectorPanel();
    void BuildRenderScene();
    void ResolveMeshResources();
    ModelHandle ResolveModel(const MeshRendererComponent& component) const;
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
    RenderSurface sceneViewSurface_{};
    PostProcessSystem sceneViewPostProcess_{};
    SceneRenderer sceneRenderer_{};
    RenderScene renderScene_{};
    Camera sceneViewCamera_{};
    ModelHandle primitiveModels_[4]{};
    std::unordered_map<std::string, ModelHandle> loadedModels_;
    int requestedSceneWidth_ = 960;
    int requestedSceneHeight_ = 540;
    bool postProcessInitializationAttempted_ = false;
};
