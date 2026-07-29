#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"

using namespace EditorSceneHierarchyUtils;

bool EditorScene::DrawImageRemoval(WorldEntity* entity) {
    if (!ImGui::Button("Remove Image")) {
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    const std::string previousPath = entity->image->texturePath;
    entity->image.reset();
    loadedTextures_.erase(previousPath);
    RecordImmediateEdit("Remove Image", before, selectionBefore);
    status_ = "Removed Image.";
    return true;
}

void EditorScene::DrawImageGeneralSettings(ImageComponent& image, EntityId selectionBefore) {
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Enabled##Image", &image.enabled)) {
        RecordImmediateEdit("Toggle Image", std::move(before), selectionBefore);
    }

    const char* imageType = image.type == ImageType::Filled ? "Filled" : "Simple";
    if (ImGui::BeginCombo("Type##Image", imageType)) {
        if (ImGui::Selectable("Simple", image.type == ImageType::Simple)) {
            const std::string typeBefore = WorldSerializer::Serialize(world_);
            image.type = ImageType::Simple;
            RecordImmediateEdit("Modify Image Type", typeBefore, selectionBefore);
            status_ = "Modified Image type.";
        }
        if (ImGui::Selectable("Filled", image.type == ImageType::Filled)) {
            const std::string typeBefore = WorldSerializer::Serialize(world_);
            image.type = ImageType::Filled;
            RecordImmediateEdit("Modify Image Type", typeBefore, selectionBefore);
            status_ = "Modified Image type.";
        }
        ImGui::EndCombo();
    }

    const std::string preserveAspectBefore = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Preserve Aspect##Image", &image.preserveAspect)) {
        RecordImmediateEdit("Toggle Image Preserve Aspect", preserveAspectBefore, selectionBefore);
        status_ = "Modified Image aspect preservation.";
    }
}

void EditorScene::DrawImageFillSettings(ImageComponent& image, EntityId selectionBefore) {
    if (image.type != ImageType::Filled) {
        return;
    }
    const char* fillMethod =
        image.fillMethod == ImageFillMethod::Vertical ? "Vertical" : "Horizontal";
    if (ImGui::BeginCombo("Fill Method##Image", fillMethod)) {
        if (ImGui::Selectable("Horizontal",
                              image.fillMethod == ImageFillMethod::Horizontal)) {
            const std::string methodBefore = WorldSerializer::Serialize(world_);
            image.fillMethod = ImageFillMethod::Horizontal;
            RecordImmediateEdit("Modify Image Fill Method", methodBefore, selectionBefore);
            status_ = "Modified Image fill method.";
        }
        if (ImGui::Selectable("Vertical", image.fillMethod == ImageFillMethod::Vertical)) {
            const std::string methodBefore = WorldSerializer::Serialize(world_);
            image.fillMethod = ImageFillMethod::Vertical;
            RecordImmediateEdit("Modify Image Fill Method", methodBefore, selectionBefore);
            status_ = "Modified Image fill method.";
        }
        ImGui::EndCombo();
    }
    if (ImGui::DragFloat("Fill Amount##Image", &image.fillAmount, 0.01f, 0.0f, 1.0f, "%.2f",
                         ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Image fill amount.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Image Fill Amount");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    const std::string reverseBefore = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Reverse Fill##Image", &image.fillReverse)) {
        RecordImmediateEdit("Toggle Image Reverse Fill", reverseBefore, selectionBefore);
        status_ = "Modified Image fill direction.";
    }
    ImGui::TextDisabled(image.fillMethod == ImageFillMethod::Horizontal
                            ? "Fills left-to-right; Reverse fills right-to-left."
                            : "Fills bottom-to-top; Reverse fills top-to-bottom.");
}

void EditorScene::DrawImageAnchorAndPivotSettings(ImageComponent& image,
                                                   EntityId selectionBefore) {
    const UiAnchorChoice& imageAnchor = GetUiAnchorChoice(image.anchor);
    if (ImGui::BeginCombo("Anchor##Image", imageAnchor.label)) {
        for (const UiAnchorChoice& choice : kUiAnchorChoices) {
            if (ImGui::Selectable(choice.label, image.anchor == choice.value)) {
                const std::string anchorBefore = WorldSerializer::Serialize(world_);
                image.anchor = choice.value;
                RecordImmediateEdit("Modify Image Anchor", anchorBefore, selectionBefore);
                status_ = "Modified Image anchor.";
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Position is an offset from the selected anchor.");
    if (ImGui::DragFloat2("Pivot##Image", &image.pivot.x, 0.01f, 0.0f, 1.0f, "%.2f",
                          ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Image pivot.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Image Pivot");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    ImGui::TextDisabled("Pivot (0,0) is top-left; (1,1) is bottom-right.");
}

void EditorScene::DrawImageTextureSettings(ImageComponent& image, EntityId selectionBefore) {
    const std::string textureLabel =
        image.texturePath.empty() ? "None (solid color)" : image.texturePath;
    if (ImGui::Button((textureLabel + "##ImageTexture").c_str(), {-FLT_MIN, 0.0f})) {
        ImGui::OpenPopup("ImageTexturePicker");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignImageTexture(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (!ImGui::BeginPopup("ImageTexturePicker")) {
        return;
    }
    if (ImGui::MenuItem("None (solid color)", nullptr, image.texturePath.empty())) {
        const std::string clearBefore = WorldSerializer::Serialize(world_);
        const std::string previousPath = image.texturePath;
        image.texturePath.clear();
        loadedTextures_.erase(previousPath);
        RecordImmediateEdit("Clear Image Texture", clearBefore, selectionBefore);
        status_ = "Cleared Image texture.";
    }
    ImGui::Separator();
    for (const std::filesystem::path& textureAsset : textureAssets_) {
        const std::string reference =
            "asset://" + textureAsset.lexically_relative("assets").generic_string();
        const std::string label =
            textureAsset.filename().generic_string() + "##Image" + textureAsset.generic_string();
        if (ImGui::MenuItem(label.c_str(), nullptr, image.texturePath == reference)) {
            AssignImageTexture(selection_, textureAsset);
        }
    }
    ImGui::EndPopup();
}

void EditorScene::DrawImageLayoutAndColorSettings(ImageComponent& image) {
    if (ImGui::DragFloat2("Position##Image", &image.position.x, 1.0f, -1000000.0f, 1000000.0f,
                          "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Image position.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Image");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    if (ImGui::DragFloat2("Size##Image", &image.size.x, 1.0f, 0.0f, 1000000.0f, "%.1f",
                          ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Image size.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Image");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    if (ImGui::ColorEdit4("Color##Image", &image.color.x)) {
        RefreshDirty();
        status_ = "Modified Image color.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Image");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}

void EditorScene::DrawImageTexturePreview(ImageComponent& image, EntityId selectionBefore) {
    const TextureHandle texture = image.texturePath.empty() ? TextureHandle{}
                                  : loadedTextures_.contains(image.texturePath)
                                      ? loadedTextures_.at(image.texturePath)
                                      : TextureHandle{};
    const bool textureAvailable =
        texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
        ctx_->rendering.texture->IsValidTexture(texture);
    ImGui::BeginDisabled(!textureAvailable);
    if (ImGui::Button("Set Native Size##Image")) {
        const std::string nativeSizeBefore = WorldSerializer::Serialize(world_);
        image.size = {
            static_cast<float>(ctx_->rendering.texture->GetWidth(texture)),
            static_cast<float>(ctx_->rendering.texture->GetHeight(texture)),
        };
        RecordImmediateEdit("Set Image Native Size", nativeSizeBefore, selectionBefore);
        status_ = "Set Image to its texture's native size.";
    }
    ImGui::EndDisabled();
    if (textureAvailable) {
        const D3D12_GPU_DESCRIPTOR_HANDLE handle =
            ctx_->rendering.texture->GetGpuHandle(texture);
        ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
    } else if (!image.texturePath.empty()) {
        ImGui::TextDisabled("Texture is loading or unavailable.");
    }
}

void EditorScene::DrawImageCanvasRequirement(const WorldEntity* entity) {
    const WorldEntity* ancestor = entity;
    while (ancestor != nullptr && !ancestor->canvas) {
        ancestor = ancestor->parent.IsValid() ? world_.Find(ancestor->parent) : nullptr;
    }
    if (ancestor == nullptr) {
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                           "Image requires a Canvas on this Entity or an ancestor.");
    }
}

void EditorScene::DrawImageInspector(WorldEntity* entity) {
    if (!entity->image) {
        return;
    }
    ImGui::SeparatorText("Image");
    if (DrawImageRemoval(entity)) {
        return;
    }
    ImageComponent& image = *entity->image;
    const EntityId selectionBefore = selection_;
    DrawImageGeneralSettings(image, selectionBefore);
    DrawImageFillSettings(image, selectionBefore);
    DrawImageAnchorAndPivotSettings(image, selectionBefore);
    DrawImageTextureSettings(image, selectionBefore);
    DrawImageLayoutAndColorSettings(image);
    DrawImageTexturePreview(image, selectionBefore);
    DrawImageCanvasRequirement(entity);
}
