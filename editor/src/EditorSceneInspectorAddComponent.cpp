#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"
#include "world/WorldSerializer.h"

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawAddComponentScriptDropTarget() {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptAssetDragPayload);
        payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
        static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
        AssignScriptAsset(selection_, static_cast<const char*>(payload->Data));
    }
    ImGui::EndDragDropTarget();
}

void EditorScene::AddInspectorComponent(
    const char* historyLabel, const char* status,
    const std::function<void()>& addComponent) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    addComponent();
    RecordImmediateEdit(historyLabel, before, selectionBefore);
    status_ = status;
}

void EditorScene::DrawAddRenderingComponents(WorldEntity& entity) {
    if (!entity.meshRenderer && ImGui::MenuItem("Mesh Renderer")) {
        AddInspectorComponent("Add MeshRenderer", "Added MeshRenderer.",
                              [&] { entity.meshRenderer = MeshRendererComponent{}; });
    }
    if (!entity.materialOverride && ImGui::MenuItem("Material Override")) {
        AddInspectorComponent(
            "Add Material Override", "Added Material Override.",
            [&] { entity.materialOverride = MaterialOverrideComponent{}; });
    }
    if (!entity.camera && ImGui::MenuItem("Camera")) {
        AddInspectorComponent("Add Camera", "Added Camera.",
                              [&] { entity.camera = CameraComponent{}; });
    }
    if (!entity.light && ImGui::MenuItem("Light")) {
        AddInspectorComponent("Add Light", "Added Light.",
                              [&] { entity.light = LightComponent{}; });
    }
}

void EditorScene::DrawAddAudioAnimationComponents(WorldEntity& entity) {
    if (!entity.audioSource && ImGui::MenuItem("Audio Source")) {
        AddInspectorComponent("Add AudioSource", "Added AudioSource.",
                              [&] { entity.audioSource = AudioSourceComponent{}; });
    }
    if (!entity.audioListener && ImGui::MenuItem("Audio Listener")) {
        AddInspectorComponent("Add AudioListener", "Added AudioListener.",
                              [&] { entity.audioListener = AudioListenerComponent{}; });
    }
    if (!entity.animator && ImGui::MenuItem("Animator")) {
        AddInspectorComponent("Add Animator", "Added Animator.",
                              [&] { entity.animator = AnimatorComponent{}; });
    }
}

void EditorScene::DrawAddUiFoundationComponents(WorldEntity& entity) {
    if (!entity.canvas && ImGui::MenuItem("Canvas")) {
        AddInspectorComponent("Add Canvas", "Added Canvas.",
                              [&] { entity.canvas = CanvasComponent{}; });
    }
    if (!entity.canvasGroup && ImGui::MenuItem("Canvas Group")) {
        AddInspectorComponent("Add CanvasGroup", "Added Canvas Group.",
                              [&] { entity.canvasGroup = CanvasGroupComponent{}; });
    }
    if (!entity.eventSystem && ImGui::MenuItem("Event System")) {
        AddInspectorComponent("Add EventSystem", "Added Event System.",
                              [&] { entity.eventSystem = EventSystemComponent{}; });
    }
    if (!entity.text && ImGui::MenuItem("Text")) {
        AddInspectorComponent("Add Text", "Added Text.",
                              [&] { entity.text = TextComponent{}; });
    }
    if (!entity.image && ImGui::MenuItem("Image")) {
        AddInspectorComponent("Add Image", "Added Image.",
                              [&] { entity.image = ImageComponent{}; });
    }
}

void EditorScene::DrawAddUiControlComponents(WorldEntity& entity) {
    if (!entity.button && ImGui::MenuItem("Button")) {
        AddInspectorComponent("Add Button", "Added Button.",
                              [&] { entity.button = ButtonComponent{}; });
    }
    if (!entity.toggle && ImGui::MenuItem("Toggle")) {
        AddInspectorComponent("Add Toggle", "Added Toggle.",
                              [&] { entity.toggle = ToggleComponent{}; });
    }
    if (!entity.slider && ImGui::MenuItem("Slider")) {
        AddInspectorComponent("Add Slider", "Added Slider.",
                              [&] { entity.slider = SliderComponent{}; });
    }
    if (!entity.dropdown && ImGui::MenuItem("Dropdown")) {
        AddInspectorComponent("Add Dropdown", "Added Dropdown.",
                              [&] { entity.dropdown = DropdownComponent{}; });
    }
    if (!entity.inputField && ImGui::MenuItem("Input Field")) {
        AddInspectorComponent("Add InputField", "Added Input Field.",
                              [&] { entity.inputField = InputFieldComponent{}; });
    }
}

void EditorScene::DrawAddPhysicsComponents(WorldEntity& entity) {
    if (!entity.boxCollider && ImGui::MenuItem("Box Collider")) {
        AddInspectorComponent("Add BoxCollider", "Added BoxCollider.",
                              [&] { entity.boxCollider = BoxColliderComponent{}; });
    }
    if (!entity.characterController && ImGui::MenuItem("Character Controller")) {
        AddInspectorComponent(
            "Add CharacterController", "Added CharacterController.",
            [&] { entity.characterController = CharacterControllerComponent{}; });
    }
}

void EditorScene::DrawAddScriptComponent(WorldEntity& entity) {
    if (ImGui::MenuItem("Script")) {
        AddInspectorComponent("Add Script", "Added an empty Script component.",
                              [&] { entity.scripts.emplace_back(); });
    }
}

void EditorScene::DrawAddComponentInspector(WorldEntity* entity) {
    ImGui::Separator();
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentMenu");
    }
    DrawAddComponentScriptDropTarget();
    if (!ImGui::BeginPopup("AddComponentMenu")) {
        return;
    }
    DrawAddRenderingComponents(*entity);
    DrawAddAudioAnimationComponents(*entity);
    DrawAddUiFoundationComponents(*entity);
    DrawAddUiControlComponents(*entity);
    DrawAddPhysicsComponents(*entity);
    DrawAddScriptComponent(*entity);
    ImGui::EndPopup();
}
