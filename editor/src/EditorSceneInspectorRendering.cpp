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

void EditorScene::DrawCameraInspector(WorldEntity* entity) {
    if (entity->camera) {
        ImGui::SeparatorText("Camera");
        if (ImGui::Button("Remove Camera")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->camera.reset();
            RecordImmediateEdit("Remove Camera", before, selectionBefore);
            status_ = "Removed Camera.";
        } else {
            CameraComponent& camera = *entity->camera;
            if (ImGui::Button("Align to Scene View")) {
                AlignSelectedCameraToSceneView();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move and rotate this Camera to match the Scene View.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Move View to Camera")) {
                AlignSceneViewToSelectedCamera();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Move the Scene View to this Camera's position and rotation.");
            }
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Camera", &camera.enabled)) {
                RecordImmediateEdit("Toggle Camera", std::move(before), selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Primary##Camera", &camera.primary)) {
                if (camera.primary) {
                    world_.SetPrimaryCamera(entity->id);
                }
                RecordImmediateEdit("Change Primary Camera", std::move(before), selectionBefore);
            }
            int projection = static_cast<int>(camera.projection);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Projection##Camera", &projection, "Perspective\0Orthographic\0")) {
                camera.projection = static_cast<CameraProjection>(projection);
                RecordImmediateEdit("Change Camera Projection", std::move(before), selectionBefore);
            }
            auto drawCameraFloat = [&](const char* label, float& value, float speed, float minimum,
                                       float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Camera.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Camera");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            if (camera.projection == CameraProjection::Perspective) {
                drawCameraFloat("Field of View", camera.fieldOfViewDegrees, 0.25f, 1.0f, 179.0f,
                                "%.1f deg");
            } else {
                drawCameraFloat("Orthographic Height", camera.orthographicHeight, 0.05f, 0.001f,
                                1000000.0f, "%.3f");
            }
            drawCameraFloat("Near Clip", camera.nearClip, 0.005f, 0.001f,
                            (std::max)(0.001f, camera.farClip - 0.001f), "%.3f");
            drawCameraFloat("Far Clip", camera.farClip, 0.5f, camera.nearClip + 0.001f,
                            1000000000.0f, "%.1f");
        }
    }
}

void EditorScene::DrawLightInspector(WorldEntity* entity) {
    if (entity->light) {
        ImGui::SeparatorText("Light");
        if (ImGui::Button("Remove Light")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->light.reset();
            RecordImmediateEdit("Remove Light", before, selectionBefore);
            status_ = "Removed Light.";
        } else {
            LightComponent& light = *entity->light;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##Light", &light.enabled)) {
                RecordImmediateEdit("Toggle Light", std::move(before), selectionBefore);
            }
            int type = static_cast<int>(light.type);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Type##Light", &type, "Directional\0Point\0Spot\0")) {
                light.type = static_cast<LightType>(type);
                RecordImmediateEdit("Change Light Type", std::move(before), selectionBefore);
            }
            if (ImGui::ColorEdit3("Color##Light", &light.color.x, ImGuiColorEditFlags_Float)) {
                RefreshDirty();
                status_ = "Modified Light.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Light Color");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            auto drawLightFloat = [&](const char* label, float& value, float speed, float minimum,
                                      float maximum, const char* format) {
                if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format,
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Light.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Light");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawLightFloat("Intensity##Light", light.intensity, 0.02f, 0.0f, 1000000.0f, "%.2f");
            if (light.type != LightType::Directional) {
                drawLightFloat("Range##Light", light.range, 0.05f, 0.001f, 1000000.0f, "%.2f");
            }
            if (light.type == LightType::Spot) {
                drawLightFloat("Inner Angle##Light", light.innerAngleDegrees, 0.25f, 0.0f,
                               (std::max)(0.0f, light.outerAngleDegrees - 0.1f), "%.1f deg");
                drawLightFloat("Outer Angle##Light", light.outerAngleDegrees, 0.25f,
                               light.innerAngleDegrees + 0.1f, 179.0f, "%.1f deg");
            }
        }
    }
}

void EditorScene::DrawMaterialOverrideInspector(WorldEntity* entity) {
    if (entity->materialOverride) {
        ImGui::SeparatorText("Material Override");
        if (ImGui::Button("Remove Material Override")) {
            const std::string before = WorldSerializer::Serialize(world_);
            const EntityId selectionBefore = selection_;
            entity->materialOverride.reset();
            RecordImmediateEdit("Remove Material Override", before, selectionBefore);
            status_ = "Removed Material Override.";
        } else {
            MaterialOverrideComponent& material = *entity->materialOverride;
            const EntityId selectionBefore = selection_;
            std::string before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Enabled##MaterialOverride", &material.enabled)) {
                RecordImmediateEdit("Toggle Material Override", std::move(before), selectionBefore);
            }
            int blendMode = static_cast<int>(material.blendMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Blend Mode##MaterialOverride", &blendMode,
                             "Opaque\0Cutout\0Transparent\0")) {
                material.blendMode = static_cast<MaterialSurfaceBlendMode>(blendMode);
                RecordImmediateEdit("Change Material Blend Mode", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Cutout) {
                if (ImGui::DragFloat("Alpha Cutoff##MaterialOverride", &material.alphaCutoff, 0.01f,
                                     0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Alpha Cutoff");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            }
            int cullMode = static_cast<int>(material.cullMode);
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("Cull Mode##MaterialOverride", &cullMode,
                             "None (Double-Sided)\0Front\0Back\0")) {
                material.cullMode = static_cast<MaterialSurfaceCullMode>(cullMode);
                RecordImmediateEdit("Change Material Cull Mode", std::move(before),
                                    selectionBefore);
            }
            before = WorldSerializer::Serialize(world_);
            if (ImGui::Checkbox("Depth Write##MaterialOverride", &material.depthWrite)) {
                RecordImmediateEdit("Toggle Material Depth Write", std::move(before),
                                    selectionBefore);
            }
            if (material.blendMode == MaterialSurfaceBlendMode::Transparent &&
                ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Transparent materials always disable depth writes at draw time.");
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
            auto drawMaterialFloat = [&](const char* label, float& value) {
                if (ImGui::DragFloat(label, &value, 0.01f, 0.0f, 1.0f, "%.3f",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    RefreshDirty();
                    status_ = "Modified Material Override.";
                }
                if (ImGui::IsItemActivated()) {
                    BeginHistoryEdit("Modify Material Override");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    CommitHistoryEdit();
                }
            };
            drawMaterialFloat("Metallic##MaterialOverride", material.metallic);
            drawMaterialFloat("Roughness##MaterialOverride", material.roughness);
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
            if (!material.baseColorTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##BaseColorTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.baseColorTexturePath;
                    material.baseColorTexturePath.clear();
                    loadedTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Base Color Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Base Color texture.";
                }
                const TextureHandle texture = ResolveBaseColorTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            }
            if (ImGui::DragFloat("Normal Strength##MaterialOverride", &material.normalStrength,
                                 0.01f, 0.0f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
                RefreshDirty();
                status_ = "Modified Material Override.";
            }
            if (ImGui::IsItemActivated()) {
                BeginHistoryEdit("Modify Normal Strength");
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitHistoryEdit();
            }
            std::array<char, 512> normalPathBuffer{};
            strncpy_s(normalPathBuffer.data(), normalPathBuffer.size(),
                      material.normalTexturePath.c_str(), _TRUNCATE);
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
            if (!material.normalTexturePath.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##NormalTexture")) {
                    const std::string clearBefore = WorldSerializer::Serialize(world_);
                    const std::string previousPath = material.normalTexturePath;
                    material.normalTexturePath.clear();
                    loadedLinearTextures_.erase(previousPath);
                    RecordImmediateEdit("Clear Normal Texture", clearBefore, selectionBefore);
                    status_ = "Cleared Normal texture.";
                }
                const TextureHandle texture = ResolveNormalTexture(material);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Normal texture is loading or unavailable.");
                }
            }
            int packing = static_cast<int>(material.pbrTexturePacking);
            const std::string packingBefore = WorldSerializer::Serialize(world_);
            if (ImGui::Combo("PBR Texture Packing", &packing,
                             "Separate\0ORM (R=AO, G=Roughness, B=Metallic)\0Metallic-Roughness "
                             "(G=Roughness, B=Metallic)\0")) {
                material.pbrTexturePacking = static_cast<MaterialPbrTexturePacking>(packing);
                RecordImmediateEdit("Change PBR Texture Packing", packingBefore, selectionBefore);
            }
            using TextureAssignFunction =
                void (EditorScene::*)(EntityId, const std::filesystem::path&);
            auto drawLinearTextureSlot = [&](const char* label, const char* id, std::string& path,
                                             TextureAssignFunction assignTexture) {
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
                        RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                            selectionBefore);
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
                    RecordImmediateEdit(std::string("Clear ") + label, clearBefore,
                                        selectionBefore);
                    status_ = std::string("Cleared ") + label + ".";
                    return;
                }
                const TextureHandle texture = ResolveLinearTexture(path);
                if (texture.IsValid() && ctx_ != nullptr && ctx_->rendering.texture != nullptr &&
                    ctx_->rendering.texture->IsValidTexture(texture)) {
                    const D3D12_GPU_DESCRIPTOR_HANDLE handle =
                        ctx_->rendering.texture->GetGpuHandle(texture);
                    ImGui::Image(static_cast<ImTextureID>(handle.ptr), {64.0f, 64.0f});
                } else {
                    ImGui::TextDisabled("Texture is loading or unavailable.");
                }
            };
            drawLinearTextureSlot("Roughness Texture", "RoughnessTexture",
                                  material.roughnessTexturePath,
                                  &EditorScene::AssignRoughnessTexture);
            drawLinearTextureSlot("Metallic Texture", "MetallicTexture",
                                  material.metallicTexturePath,
                                  &EditorScene::AssignMetallicTexture);
            if (material.pbrTexturePacking != MaterialPbrTexturePacking::Separate &&
                (material.roughnessTexturePath.empty() ||
                 material.roughnessTexturePath != material.metallicTexturePath)) {
                ImGui::TextColored({1.0f, 0.7f, 0.25f, 1.0f},
                                   "Packed PBR textures must use the same asset in both slots.");
            }
            if (!entity->meshRenderer) {
                ImGui::TextDisabled("Add a Mesh Renderer to display this material.");
            }
        }
    }
}
