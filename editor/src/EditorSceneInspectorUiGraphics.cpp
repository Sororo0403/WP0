#include "AssetImportPlanner.h"
#include "EditorScene.h"
#include "PlayerPackageBuilder.h"
#include "PlayerProjectValidator.h"
#include "ProjectDescriptor.h"
#include "RuntimeSceneLoader.h"
#include "ScriptAsset.h"
#include "ScriptBuildService.h"
#include "core/AssetManager.h"
#include "core/MathUtils.h"
#include "core/WinApp.h"
#include "font/TextRenderer.h"
#include "graphics/DirectXCommon.h"
#include "graphics/LightingScene.h"
#include "graphics/RenderScene.h"
#include "graphics/ShaderPaths.h"
#include "graphics/SrvManager.h"
#include "imgui.h"
#include "imgui/ImguiManager.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "model/MeshRenderer.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldCollision.h"
#include "world/WorldSerializer.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

#include "internal/EditorSceneHierarchyUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace EditorSceneHierarchyUtils;

void EditorScene::DrawTextInspector(WorldEntity* entity) {
    if (entity->text) {
        ImGui::SeparatorText("Text");
        if (ImGui::Button("Remove Text")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->text.reset();
            RecordImmediateEdit("Remove Text", before, selectionBefore);
            status_ = "Removed Text.";
        } else {
            TextComponent& text = *entity->text;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Text", &text.enabled)) {
                RecordImmediateEdit("Toggle Text", std::move(before), selectionBefore);
            }
            const UiAnchorChoice& textAnchor = GetUiAnchorChoice(text.anchor);
            if (ImGui::BeginCombo("Anchor##Text", textAnchor.label)) {
                for (const UiAnchorChoice& choice : kUiAnchorChoices) {
                    if (ImGui::Selectable(choice.label, text.anchor == choice.value)) {
                        const std::string anchorBefore = WorldSerializer::Serialize(world_);
                        text.anchor = choice.value;
                        RecordImmediateEdit("Modify Text Anchor", anchorBefore, selectionBefore);
                        status_ = "Modified Text anchor.";
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Position is an offset from the selected anchor.");

            std::array<char, 4097> textBuffer{};
            std::memcpy(textBuffer.data(), text.text.data(),
                        (std::min)(text.text.size(), textBuffer.size() - 1u));
            if (ImGui::InputTextMultiline("Content##Text", textBuffer.data(), textBuffer.size(),
                                          {-FLT_MIN, 80.0f})) {
                text.text = textBuffer.data();
                RefreshDirty();
                status_ = "Modified Text content.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat2("Position##Text", &text.position.x, 1.0f, -1000000.0f, 1000000.0f,
                                  "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text position.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            const std::string fontLabel = text.fontPath.empty() ? "Default" : text.fontPath;
            if (ImGui::Button((fontLabel + "##TextFont").c_str(), {-FLT_MIN, 0.0f})) {
                ImGui::OpenPopup("TextFontPicker");
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kFontAssetDragPayload);
                    payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
                    AssignTextFont(selection_, static_cast<const char*>(payload->Data));
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("TextFontPicker")) {
                if (ImGui::MenuItem("Default", nullptr, text.fontPath.empty())) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    text.fontPath.clear();
                    RecordImmediateEdit("Clear Text Font", clearBefore, selectionBefore);
                    status_ = "Reset Text to the default font.";
                }
                ImGui::Separator();
                for (const std::filesystem::path& fontAsset : fontAssets_) {
                    const std::string reference =
                        "asset://" + fontAsset.lexically_relative("assets").generic_string();
                    const std::string label = fontAsset.filename().generic_string() + "##TextFont" +
                                              fontAsset.generic_string();
                    if (ImGui::MenuItem(label.c_str(), nullptr, text.fontPath == reference)) {
                        AssignTextFont(selection_, fontAsset);
                    }
                }
                ImGui::EndPopup();
            }
            if (!text.fontPath.empty()) {
                const std::optional<std::filesystem::path> resolvedFont =
                    ResolveProjectAssetPath(text.fontPath);
                if (!resolvedFont || !AssetImport::IsFontFile(*resolvedFont)) {
                    ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f},
                                       "The assigned font asset is missing or invalid.");
                } else if (const auto loadedFont = loadedFonts_.find(text.fontPath);
                           loadedFont != loadedFonts_.end() && !loadedFont->second.IsValid()) {
                    ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                       "The font could not be loaded; using the default.");
                }
            }
            if (ImGui::DragFloat("Font Size##Text", &text.fontSize, 0.5f, 1.0f, 512.0f, "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text font size.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Line Spacing##Text", &text.lineSpacing, 0.5f, 0.0f, 512.0f,
                                 "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text line spacing.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text Line Spacing");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            if (ImGui::DragFloat("Wrap Width##Text", &text.wrapWidth, 1.0f, 0.0f, 16384.0f, "%.1f",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Text wrap width.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text Wrap Width");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            ImGui::TextDisabled("0 disables automatic wrapping.");
            if (ImGui::ColorEdit4("Color##Text", &text.color.x)) {
                RefreshDirty();
                status_ = "Modified Text color.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Text");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }

            const char* alignment = text.alignment == TextAlignment::Left     ? "Left"
                                    : text.alignment == TextAlignment::Center ? "Center"
                                                                              : "Right";
            if (ImGui::BeginCombo("Alignment##Text", alignment)) {
                const auto selectAlignment = [&](TextAlignment value, const char* label) {
                    if (ImGui::Selectable(label, text.alignment == value)) {
                        const std::string alignmentBefore = WorldSerializer::Serialize(world_);
                        text.alignment = value;
                        RecordImmediateEdit("Modify Text Alignment", alignmentBefore,
                                            selectionBefore);
                        status_ = "Modified Text alignment.";
                    }
                };
                selectAlignment(TextAlignment::Left, "Left");
                selectAlignment(TextAlignment::Center, "Center");
                selectAlignment(TextAlignment::Right, "Right");
                ImGui::EndCombo();
            }

            const WorldEntity* ancestor = entity;
            while (ancestor != nullptr && !ancestor->canvas) {
                ancestor = ancestor->parent.IsValid() ? world_.Find(ancestor->parent) : nullptr;
            }
            if (ancestor == nullptr) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Text requires a Canvas on this Entity or an ancestor.");
            }
        }
    }
}

void EditorScene::DrawImageInspector(WorldEntity* entity) {
    if (entity->image) {
        ImGui::SeparatorText("Image");
        if (ImGui::Button("Remove Image")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            const std::string previousPath = entity->image->texturePath;
            entity->image.reset();
            loadedTextures_.erase(previousPath);
            RecordImmediateEdit("Remove Image", before, selectionBefore);
            status_ = "Removed Image.";
        } else {
            ImageComponent& image = *entity->image;
            const EntityId selectionBefore = selection_;
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
                RecordImmediateEdit("Toggle Image Preserve Aspect", preserveAspectBefore,
                                    selectionBefore);
                status_ = "Modified Image aspect preservation.";
            }
            if (image.type == ImageType::Filled) {
                const char* fillMethod =
                    image.fillMethod == ImageFillMethod::Vertical ? "Vertical" : "Horizontal";
                if (ImGui::BeginCombo("Fill Method##Image", fillMethod)) {
                    if (ImGui::Selectable("Horizontal",
                                          image.fillMethod == ImageFillMethod::Horizontal)) {
                        const std::string methodBefore = WorldSerializer::Serialize(world_);
                        image.fillMethod = ImageFillMethod::Horizontal;
                        RecordImmediateEdit("Modify Image Fill Method", methodBefore,
                                            selectionBefore);
                        status_ = "Modified Image fill method.";
                    }
                    if (ImGui::Selectable("Vertical",
                                          image.fillMethod == ImageFillMethod::Vertical)) {
                        const std::string methodBefore = WorldSerializer::Serialize(world_);
                        image.fillMethod = ImageFillMethod::Vertical;
                        RecordImmediateEdit("Modify Image Fill Method", methodBefore,
                                            selectionBefore);
                        status_ = "Modified Image fill method.";
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::DragFloat("Fill Amount##Image", &image.fillAmount, 0.01f, 0.0f, 1.0f,
                                     "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
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
                    RecordImmediateEdit("Toggle Image Reverse Fill", reverseBefore,
                                        selectionBefore);
                    status_ = "Modified Image fill direction.";
                }
                ImGui::TextDisabled(image.fillMethod == ImageFillMethod::Horizontal
                                        ? "Fills left-to-right; Reverse fills right-to-left."
                                        : "Fills bottom-to-top; Reverse fills top-to-bottom.");
            }
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
            if (ImGui::BeginPopup("ImageTexturePicker")) {
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
                    const std::string label = textureAsset.filename().generic_string() + "##Image" +
                                              textureAsset.generic_string();
                    if (ImGui::MenuItem(label.c_str(), nullptr, image.texturePath == reference)) {
                        AssignImageTexture(selection_, textureAsset);
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::DragFloat2("Position##Image", &image.position.x, 1.0f, -1000000.0f,
                                  1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp)) {
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

            const TextureHandle texture = image.texturePath.empty() ? TextureHandle{}
                                          : loadedTextures_.contains(image.texturePath)
                                              ? loadedTextures_.at(image.texturePath)
                                              : TextureHandle{};
            const bool textureAvailable = texture.IsValid() && ctx_ != nullptr &&
                                          ctx_->rendering.texture != nullptr &&
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

            const WorldEntity* ancestor = entity;
            while (ancestor != nullptr && !ancestor->canvas) {
                ancestor = ancestor->parent.IsValid() ? world_.Find(ancestor->parent) : nullptr;
            }
            if (ancestor == nullptr) {
                ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f},
                                   "Image requires a Canvas on this Entity or an ancestor.");
            }
        }
    }
}
