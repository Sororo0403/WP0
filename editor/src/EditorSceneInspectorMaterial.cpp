#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneHierarchyUtils.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"

#include <array>
#include <cstring>
#include <utility>

using namespace EditorSceneHierarchyUtils;

bool EditorScene::DrawMaterialOverrideHeader(WorldEntity* entity) {
    ImGui::SeparatorText("Material Override");
    if (!ImGui::Button("Remove Material Override")) {
        return false;
    }
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity->materialOverride.reset();
    RecordImmediateEdit("Remove Material Override", before, selectionBefore);
    status_ = "Removed Material Override.";
    return true;
}

void EditorScene::DrawMaterialFloat(const char* label, float& value, float minimum, float maximum,
                                    const char* historyLabel) {
    if (ImGui::DragFloat(label, &value, 0.01f, minimum, maximum, "%.3f",
                         ImGuiSliderFlags_AlwaysClamp)) {
        RefreshDirty();
        status_ = "Modified Material Override.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit(historyLabel);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
}

void EditorScene::DrawMaterialSurfaceSettings(MaterialOverrideComponent& material,
                                              EntityId selectionBefore) {
    std::string before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Enabled##MaterialOverride", &material.enabled)) {
        RecordImmediateEdit("Toggle Material Override", std::move(before), selectionBefore);
    }
    int blendMode = static_cast<int>(material.blendMode);
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Combo("Blend Mode##MaterialOverride", &blendMode,
                     "Opaque\0Cutout\0Transparent\0")) {
        material.blendMode = static_cast<MaterialSurfaceBlendMode>(blendMode);
        RecordImmediateEdit("Change Material Blend Mode", std::move(before), selectionBefore);
    }
    if (material.blendMode == MaterialSurfaceBlendMode::Cutout) {
        DrawMaterialFloat("Alpha Cutoff##MaterialOverride", material.alphaCutoff, 0.0f, 1.0f,
                          "Modify Alpha Cutoff");
    }
    int cullMode = static_cast<int>(material.cullMode);
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Combo("Cull Mode##MaterialOverride", &cullMode,
                     "None (Double-Sided)\0Front\0Back\0")) {
        material.cullMode = static_cast<MaterialSurfaceCullMode>(cullMode);
        RecordImmediateEdit("Change Material Cull Mode", std::move(before), selectionBefore);
    }
    before = WorldSerializer::Serialize(world_);
    if (ImGui::Checkbox("Depth Write##MaterialOverride", &material.depthWrite)) {
        RecordImmediateEdit("Toggle Material Depth Write", std::move(before), selectionBefore);
    }
    if (material.blendMode == MaterialSurfaceBlendMode::Transparent &&
        ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Transparent materials always disable depth writes at draw time.");
    }
    if (ImGui::ColorEdit4("Base Color##MaterialOverride", &material.baseColor.x,
                          ImGuiColorEditFlags_Float)) {
        RefreshDirty();
        status_ = "Modified Material Override.";
    }
    if (ImGui::IsItemActivated()) {
        BeginHistoryEdit("Modify Material Base Color");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitHistoryEdit();
    }
    DrawMaterialFloat("Metallic##MaterialOverride", material.metallic, 0.0f, 1.0f);
    DrawMaterialFloat("Roughness##MaterialOverride", material.roughness, 0.0f, 1.0f);
}

void EditorScene::DrawMaterialTexturePreview(TextureHandle texture,
                                             const char* unavailableText) const {
    if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
        ctx_->rendering.texture->IsValidTexture(texture)) {
        const D3D12_GPU_DESCRIPTOR_HANDLE handle =
            ctx_->rendering.texture->GetGpuHandle(texture);
        ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
    } else {
        ImGui::TextDisabled(unavailableText);
    }
}

void EditorScene::DrawBaseColorTextureSlot(MaterialOverrideComponent& material,
                                           EntityId selectionBefore) {
    std::array<char, 512> texturePathBuffer{};
    strncpy_s(texturePathBuffer.data(), texturePathBuffer.size(),
              material.baseColorTexturePath.c_str(), _TRUNCATE);
    if (ImGui::InputText("Base Color Texture", texturePathBuffer.data(),
                         texturePathBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (texturePathBuffer[0] == '\0') {
            const std::string clearBefore = WorldSerializer::Serialize(world_);
            const std::string previousPath = material.baseColorTexturePath;
            material.baseColorTexturePath.clear();
            loadedTextures_.erase(previousPath);
            RecordImmediateEdit("Clear Base Color Texture", clearBefore, selectionBefore);
            status_ = "Cleared Base Color texture.";
        } else {
            AssignBaseColorTexture(selection_, texturePathBuffer.data());
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignBaseColorTexture(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (material.baseColorTexturePath.empty()) {
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##BaseColorTexture")) {
        const std::string clearBefore = WorldSerializer::Serialize(world_);
        const std::string previousPath = material.baseColorTexturePath;
        material.baseColorTexturePath.clear();
        loadedTextures_.erase(previousPath);
        RecordImmediateEdit("Clear Base Color Texture", clearBefore, selectionBefore);
        status_ = "Cleared Base Color texture.";
    }
    DrawMaterialTexturePreview(ResolveBaseColorTexture(material),
                               "Texture is loading or unavailable.");
}

void EditorScene::DrawNormalTextureSettings(MaterialOverrideComponent& material,
                                            EntityId selectionBefore) {
    DrawMaterialFloat("Normal Strength##MaterialOverride", material.normalStrength, 0.0f, 4.0f,
                      "Modify Normal Strength");
    std::array<char, 512> normalPathBuffer{};
    strncpy_s(normalPathBuffer.data(), normalPathBuffer.size(), material.normalTexturePath.c_str(),
              _TRUNCATE);
    if (ImGui::InputText("Normal Texture", normalPathBuffer.data(), normalPathBuffer.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (normalPathBuffer[0] == '\0') {
            const std::string clearBefore = WorldSerializer::Serialize(world_);
            const std::string previousPath = material.normalTexturePath;
            material.normalTexturePath.clear();
            loadedLinearTextures_.erase(previousPath);
            RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
            status_ = "Cleared Normal texture.";
        } else {
            AssignNormalTexture(selection_, normalPathBuffer.data());
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            AssignNormalTexture(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (material.normalTexturePath.empty()) {
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##NormalTexture")) {
        const std::string clearBefore = WorldSerializer::Serialize(world_);
        const std::string previousPath = material.normalTexturePath;
        material.normalTexturePath.clear();
        loadedLinearTextures_.erase(previousPath);
        RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
        status_ = "Cleared Normal texture.";
    }
    DrawMaterialTexturePreview(ResolveNormalTexture(material),
                               "Normal texture is loading or unavailable.");
}

void EditorScene::DrawMaterialLinearTextureSlot(const char* label, const char* id,
                                                std::string& path, EntityId selectionBefore,
                                                MaterialTextureAssignFunction assignTexture) {
    std::array<char, 512> pathBuffer{};
    strncpy_s(pathBuffer.data(), pathBuffer.size(), path.c_str(), _TRUNCATE);
    const std::string inputLabel = std::string(label) + "##" + id;
    if (ImGui::InputText(inputLabel.c_str(), pathBuffer.data(), pathBuffer.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (pathBuffer[0] == '\0') {
            const std::string clearBefore = WorldSerializer::Serialize(world_);
            const std::string previousPath = path;
            path.clear();
            loadedLinearTextures_.erase(previousPath);
            RecordImmediateEdit(std::string("Clear ") + label, clearBefore, selectionBefore);
            status_ = std::string("Cleared ") + label + ".";
        } else {
            (this->*assignTexture)(selection_, pathBuffer.data());
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kTextureAssetDragPayload);
            payload != nullptr && payload->IsDelivery() && payload->DataSize > 1 &&
            static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            (this->*assignTexture)(selection_, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (path.empty()) {
        return;
    }
    ImGui::SameLine();
    const std::string clearId = std::string("Clear##") + id;
    if (ImGui::SmallButton(clearId.c_str())) {
        const std::string clearBefore = WorldSerializer::Serialize(world_);
        const std::string previousPath = path;
        path.clear();
        loadedLinearTextures_.erase(previousPath);
        RecordImmediateEdit(std::string("Clear ") + label, clearBefore, selectionBefore);
        status_ = std::string("Cleared ") + label + ".";
        return;
    }
    DrawMaterialTexturePreview(ResolveLinearTexture(path), "Texture is loading or unavailable.");
}

void EditorScene::DrawMaterialPbrTextureSettings(MaterialOverrideComponent& material,
                                                 EntityId selectionBefore) {
    int packing = static_cast<int>(material.pbrTexturePacking);
    const std::string packingBefore = WorldSerializer::Serialize(world_);
    if (ImGui::Combo("PBR Texture Packing", &packing,
                     "Separate\0ORM (R=AO, G=Roughness, B=Metallic)\0Metallic-Roughness "
                     "(G=Roughness, B=Metallic)\0")) {
        material.pbrTexturePacking = static_cast<MaterialPbrTexturePacking>(packing);
        RecordImmediateEdit("Change PBR Texture Packing", packingBefore, selectionBefore);
    }
    DrawMaterialLinearTextureSlot("Roughness Texture", "RoughnessTexture",
                                  material.roughnessTexturePath, selectionBefore,
                                  &EditorScene::AssignRoughnessTexture);
    DrawMaterialLinearTextureSlot("Metallic Texture", "MetallicTexture",
                                  material.metallicTexturePath, selectionBefore,
                                  &EditorScene::AssignMetallicTexture);
    if (material.pbrTexturePacking != MaterialPbrTexturePacking::Separate &&
        (material.roughnessTexturePath.empty() ||
         material.roughnessTexturePath != material.metallicTexturePath)) {
        ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f},
                           "Packed PBR textures must use the same asset in both slots.");
    }
}
