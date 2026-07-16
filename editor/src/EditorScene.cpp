#include "EditorScene.h"

#include "imgui.h"
#include "world/WorldSerializer.h"

#include <array>
#include <cstring>
#include <utility>

namespace {
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoCollapse;
}

EditorScene::EditorScene(std::function<void()> requestClose)
    : requestClose_(std::move(requestClose)) {
    NewScene();
    dirty_ = false;
}

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
        if (ImGui::MenuItem("New Scene", "Ctrl+N")) {
            NewScene();
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            SaveScene();
        }
        if (ImGui::MenuItem("Reload Scene")) {
            LoadScene();
        }
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
    ImGui::TextUnformatted(dirty_ ? "WP0 Editor *" : "WP0 Editor");
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
    DrawHierarchyPanel();
    ImGui::End();

    BeginFixedPanel("Project", origin.x, origin.y + upperHeight, leftWidth, bottomHeight);
    ImGui::TextUnformatted("Current Scene");
    ImGui::Separator();
    ImGui::TextWrapped("%s", scenePath_.string().c_str());
    ImGui::End();

    BeginFixedPanel("Scene", origin.x + leftWidth, origin.y, centerWidth, upperHeight);
    ImGui::TextUnformatted("Scene View");
    ImGui::Separator();
    ImGui::TextDisabled("RenderSurface will be connected here.");
    ImGui::End();

    BeginFixedPanel("Console", origin.x + leftWidth, origin.y + upperHeight, centerWidth,
                    bottomHeight);
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();

    BeginFixedPanel("Inspector", origin.x + leftWidth + centerWidth, origin.y, rightWidth,
                    size.y);
    DrawInspectorPanel();
    ImGui::End();
}

void EditorScene::DrawHierarchyPanel() {
    if (ImGui::Button("Create Empty")) {
        selection_ = world_.CreateEntity();
        dirty_ = true;
        status_ = "Created a new entity.";
    }
    ImGui::SameLine();
    const bool canDelete = world_.Contains(selection_);
    if (!canDelete) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Delete")) {
        world_.DestroyEntity(selection_);
        selection_ = {};
        dirty_ = true;
        status_ = "Deleted the selected entity hierarchy.";
    }
    if (!canDelete) {
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    for (EntityId id : world_.GetRootEntities()) {
        DrawEntityNode(id);
    }
}

void EditorScene::DrawEntityNode(EntityId id) {
    const WorldEntity* entity = world_.Find(id);
    if (entity == nullptr) {
        return;
    }
    const std::vector<EntityId> children = world_.GetChildren(id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selection_ == id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string idText = id.ToString();
    ImGui::PushID(idText.c_str());
    const bool open = ImGui::TreeNodeEx(entity->name.c_str(), flags);
    if (ImGui::IsItemClicked()) {
        selection_ = id;
    }
    if (open && !children.empty()) {
        for (EntityId child : children) {
            DrawEntityNode(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorScene::DrawInspectorPanel() {
    WorldEntity* entity = world_.Find(selection_);
    if (entity == nullptr) {
        ImGui::TextDisabled("Nothing selected.");
        return;
    }

    std::array<char, 256> nameBuffer{};
    strncpy_s(nameBuffer.data(), nameBuffer.size(), entity->name.c_str(), _TRUNCATE);
    if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size())) {
        entity->name = nameBuffer.data();
        if (entity->name.empty()) {
            entity->name = "Entity";
        }
        dirty_ = true;
    }
    ImGui::TextDisabled("ID: %s", entity->id.ToString().c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Transform");
    bool transformChanged = false;
    transformChanged |=
        ImGui::DragFloat3("Position", &entity->transform.position.x, 0.05f);
    transformChanged |=
        ImGui::DragFloat3("Rotation", &entity->transform.rotationDegrees.x, 0.25f);
    transformChanged |= ImGui::DragFloat3("Scale", &entity->transform.scale.x, 0.02f);
    if (transformChanged) {
        dirty_ = true;
        status_ = "Modified Transform.";
    }
}

void EditorScene::NewScene() {
    world_.Clear();
    const EntityId camera = world_.CreateEntity("Main Camera");
    if (WorldEntity* cameraEntity = world_.Find(camera)) {
        cameraEntity->transform.position = {0.0f, 2.0f, -5.0f};
    }
    selection_ = world_.CreateEntity("Cube");
    dirty_ = true;
    status_ = "Created a new scene.";
}

void EditorScene::SaveScene() {
    std::string error;
    if (!WorldSerializer::Save(world_, scenePath_, &error)) {
        status_ = "Save failed: " + error;
        return;
    }
    dirty_ = false;
    status_ = "Saved scene: " + scenePath_.string();
}

void EditorScene::LoadScene() {
    World loaded;
    std::string error;
    if (!WorldSerializer::Load(scenePath_, loaded, &error)) {
        status_ = "Load failed: " + error;
        return;
    }
    world_ = std::move(loaded);
    selection_ = world_.Empty() ? EntityId{} : world_.Entities().front().id;
    dirty_ = false;
    status_ = "Loaded scene: " + scenePath_.string();
}
