#include "EditorScene.h"

#include "world/WorldSerializer.h"

void EditorScene::CreateUiEntity(const UiEntityPreset preset, const EntityId parent) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    if (preset == UiEntityPreset::Canvas) {
        CreateUiCanvasEntity(parent, before, selectionBefore);
        return;
    }
    EntityId createdCanvas{};
    const EntityId uiParent = ResolveUiEntityParent(parent, createdCanvas);
    if (!uiParent.IsValid()) {
        return;
    }
    const EntityId createdEventSystem = EnsureUiEventSystem();
    const EntityId entityId = CreateUiPresetEntity(preset, uiParent);
    if (!entityId.IsValid()) {
        RollbackUiSupportEntities(createdCanvas, createdEventSystem);
        status_ = "Could not create the UI element.";
        return;
    }
    selection_ = entityId;
    RecordImmediateEdit(GetUiPresetHistoryLabel(preset), before, selectionBefore);
    status_ = std::string("Created a UI ") + GetUiPresetName(preset) + ".";
}

bool EditorScene::CreateUiCanvasEntity(const EntityId parent, const std::string& before,
                                       const EntityId selectionBefore) {
    const EntityId canvasId = world_.CreateEntity("Canvas");
    WorldEntity* canvas = world_.Find(canvasId);
    if (canvas == nullptr) {
        status_ = "Could not create a Canvas.";
        return false;
    }
    canvas->canvas = CanvasComponent{};
    if (parent.IsValid() && !world_.SetParent(canvasId, parent)) {
        world_.DestroyEntity(canvasId);
        status_ = "Could not parent the new Canvas.";
        return false;
    }
    (void)EnsureUiEventSystem();
    selection_ = canvasId;
    RecordImmediateEdit("Create UI Canvas", before, selectionBefore);
    status_ = "Created a UI Canvas.";
    return true;
}

EntityId EditorScene::EnsureUiEventSystem() {
    for (const WorldEntity& entity : world_.Entities()) {
        if (entity.eventSystem) {
            return {};
        }
    }
    const EntityId eventSystemId = world_.CreateEntity("Event System");
    WorldEntity* eventSystem = world_.Find(eventSystemId);
    if (eventSystem == nullptr) {
        return {};
    }
    eventSystem->eventSystem = EventSystemComponent{};
    return eventSystemId;
}

EntityId EditorScene::FindUiCanvasParent(const EntityId requestedParent) const {
    for (EntityId current = requestedParent; current.IsValid();) {
        const WorldEntity* entity = world_.Find(current);
        if (entity == nullptr) {
            break;
        }
        if (entity->canvas) {
            return requestedParent;
        }
        current = entity->parent;
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (entity.canvas) {
            return entity.id;
        }
    }
    return {};
}

EntityId EditorScene::ResolveUiEntityParent(const EntityId requestedParent,
                                            EntityId& createdCanvas) {
    const EntityId existingParent = FindUiCanvasParent(requestedParent);
    if (existingParent.IsValid()) {
        return existingParent;
    }
    createdCanvas = world_.CreateEntity("Canvas");
    WorldEntity* canvas = world_.Find(createdCanvas);
    if (canvas == nullptr) {
        status_ = "Could not create a Canvas for the UI element.";
        return {};
    }
    canvas->canvas = CanvasComponent{};
    return createdCanvas;
}

EntityId EditorScene::CreateUiPresetEntity(const UiEntityPreset preset,
                                           const EntityId parent) {
    const EntityId entityId = world_.CreateEntity(GetUiPresetName(preset));
    WorldEntity* entity = world_.Find(entityId);
    if (entity == nullptr || !world_.SetParent(entityId, parent)) {
        world_.DestroyEntity(entityId);
        return {};
    }
    ConfigureUiPreset(*entity, preset);
    return entityId;
}

void EditorScene::RollbackUiSupportEntities(const EntityId createdCanvas,
                                            const EntityId createdEventSystem) {
    if (createdCanvas.IsValid()) {
        world_.DestroyEntity(createdCanvas);
    }
    if (createdEventSystem.IsValid()) {
        world_.DestroyEntity(createdEventSystem);
    }
}

const char* EditorScene::GetUiPresetName(const UiEntityPreset preset) const {
    switch (preset) {
        case UiEntityPreset::Canvas:
            return "Canvas";
        case UiEntityPreset::Text:
            return "Text";
        case UiEntityPreset::Image:
            return "Image";
        case UiEntityPreset::Button:
            return "Button";
        case UiEntityPreset::Toggle:
            return "Toggle";
        case UiEntityPreset::Slider:
            return "Slider";
        case UiEntityPreset::Dropdown:
            return "Dropdown";
        case UiEntityPreset::InputField:
            return "Input Field";
    }
    return "UI Element";
}

const char* EditorScene::GetUiPresetHistoryLabel(const UiEntityPreset preset) const {
    switch (preset) {
        case UiEntityPreset::Text:
            return "Create UI Text";
        case UiEntityPreset::Image:
            return "Create UI Image";
        case UiEntityPreset::Toggle:
            return "Create UI Toggle";
        case UiEntityPreset::Slider:
            return "Create UI Slider";
        case UiEntityPreset::Dropdown:
            return "Create UI Dropdown";
        case UiEntityPreset::InputField:
            return "Create UI Input Field";
        case UiEntityPreset::Canvas:
            return "Create UI Canvas";
        case UiEntityPreset::Button:
            return "Create UI Button";
    }
    return "Create UI Element";
}

void EditorScene::ConfigureUiPreset(WorldEntity& entity, const UiEntityPreset preset) {
    switch (preset) {
        case UiEntityPreset::Text:
            ConfigureUiTextPreset(entity);
            break;
        case UiEntityPreset::Image:
            ConfigureUiImagePreset(entity);
            break;
        case UiEntityPreset::Button:
            ConfigureUiButtonPreset(entity);
            break;
        case UiEntityPreset::Toggle:
            ConfigureUiTogglePreset(entity);
            break;
        case UiEntityPreset::Slider:
            ConfigureUiSliderPreset(entity);
            break;
        case UiEntityPreset::Dropdown:
            ConfigureUiDropdownPreset(entity);
            break;
        case UiEntityPreset::InputField:
            ConfigureUiInputFieldPreset(entity);
            break;
        case UiEntityPreset::Canvas:
            entity.canvas = CanvasComponent{};
            break;
    }
}

void EditorScene::ConfigureUiTextPreset(WorldEntity& entity) {
    entity.text = TextComponent{};
    entity.text->anchor = UiAnchor::Center;
    entity.text->alignment = TextAlignment::Center;
}

void EditorScene::ConfigureUiImagePreset(WorldEntity& entity) {
    entity.image = ImageComponent{};
    entity.image->anchor = UiAnchor::Center;
    entity.image->pivot = {0.5f, 0.5f};
    entity.image->size = {200.0f, 100.0f};
}

void EditorScene::ConfigureUiButtonPreset(WorldEntity& entity) {
    InitializeUiImage(entity, {240.0f, 64.0f}, {0.22f, 0.38f, 0.65f, 1.0f});
    entity.button = ButtonComponent{};
    InitializeUiText(entity, "Button", 28.0f);
}

void EditorScene::ConfigureUiTogglePreset(WorldEntity& entity) {
    InitializeUiImage(entity, {48.0f, 48.0f}, {0.18f, 0.22f, 0.28f, 1.0f});
    entity.button = ButtonComponent{};
    entity.toggle = ToggleComponent{};
}

void EditorScene::ConfigureUiSliderPreset(WorldEntity& entity) {
    InitializeUiImage(entity, {240.0f, 24.0f}, {0.18f, 0.22f, 0.28f, 1.0f});
    entity.slider = SliderComponent{};
}

void EditorScene::ConfigureUiDropdownPreset(WorldEntity& entity) {
    InitializeUiImage(entity, {280.0f, 56.0f}, {0.18f, 0.22f, 0.28f, 1.0f});
    entity.button = ButtonComponent{};
    entity.dropdown = DropdownComponent{};
    InitializeUiText(entity, entity.dropdown->options.front(), 26.0f);
}

void EditorScene::ConfigureUiInputFieldPreset(WorldEntity& entity) {
    InitializeUiImage(entity, {360.0f, 56.0f}, {0.12f, 0.15f, 0.2f, 1.0f});
    entity.button = ButtonComponent{};
    entity.inputField = InputFieldComponent{};
    InitializeUiText(entity, "", 26.0f);
}

void EditorScene::InitializeUiImage(WorldEntity& entity, const DirectX::XMFLOAT2& size,
                                    const DirectX::XMFLOAT4& color) {
    entity.image = ImageComponent{};
    entity.image->anchor = UiAnchor::Center;
    entity.image->pivot = {0.5f, 0.5f};
    entity.image->size = size;
    entity.image->color = color;
}

void EditorScene::InitializeUiText(WorldEntity& entity, const std::string& text,
                                   const float fontSize) {
    entity.text = TextComponent{};
    entity.text->text = text;
    entity.text->anchor = UiAnchor::Center;
    entity.text->alignment = TextAlignment::Center;
    entity.text->fontSize = fontSize;
}
