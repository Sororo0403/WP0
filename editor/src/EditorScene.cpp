#include "EditorScene.h"

#include "imgui.h"

#include <utility>

namespace {
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoCollapse;
}

EditorScene::EditorScene(std::function<void()> requestClose)
    : requestClose_(std::move(requestClose)) {}

void EditorScene::Update() {}

void EditorScene::Draw() {}

void EditorScene::DrawPostProcessOverlay() {
    DrawMainMenu();
    DrawPanels();
}

void EditorScene::DrawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("New Project", nullptr, false, false);
        ImGui::MenuItem("Open Project", nullptr, false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit") && requestClose_) {
            requestClose_();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::EndMenu();
    }
    ImGui::TextUnformatted("WP0 Editor");
    ImGui::EndMainMenuBar();
}

void EditorScene::BeginFixedPanel(const char* name, float x, float y, float width,
                                  float height) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::Begin(name, nullptr, kPanelFlags);
}

void EditorScene::DrawPanels() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;
    const float leftWidth = size.x * 0.22f;
    const float rightWidth = size.x * 0.24f;
    const float centerWidth = size.x - leftWidth - rightWidth;
    const float bottomHeight = size.y * 0.28f;
    const float upperHeight = size.y - bottomHeight;

    BeginFixedPanel("Hierarchy", origin.x, origin.y, leftWidth, upperHeight);
    ImGui::TextDisabled("World document is the next milestone.");
    ImGui::End();

    BeginFixedPanel("Project", origin.x, origin.y + upperHeight, leftWidth, bottomHeight);
    ImGui::TextDisabled("No project is open.");
    ImGui::End();

    BeginFixedPanel("Scene", origin.x + leftWidth, origin.y, centerWidth, upperHeight);
    ImGui::TextUnformatted("Scene View");
    ImGui::Separator();
    ImGui::TextDisabled("RenderSurface will be connected here.");
    ImGui::End();

    BeginFixedPanel("Console", origin.x + leftWidth, origin.y + upperHeight, centerWidth,
                    bottomHeight);
    ImGui::TextDisabled("Editor session started.");
    ImGui::End();

    BeginFixedPanel("Inspector", origin.x + leftWidth + centerWidth, origin.y, rightWidth,
                    size.y);
    ImGui::TextDisabled("Nothing selected.");
    ImGui::End();
}
