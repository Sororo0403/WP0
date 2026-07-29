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
    if (!entity->materialOverride || DrawMaterialOverrideHeader(entity)) {
        return;
    }
    MaterialOverrideComponent& material = *entity->materialOverride;
    const EntityId selectionBefore = selection_;
    DrawMaterialSurfaceSettings(material, selectionBefore);
    DrawBaseColorTextureSlot(material, selectionBefore);
    DrawNormalTextureSettings(material, selectionBefore);
    DrawMaterialPbrTextureSettings(material, selectionBefore);
    if (!entity->meshRenderer) {
        ImGui::TextDisabled("Add a Mesh Renderer to display this material.");
    }
}
