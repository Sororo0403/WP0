#include "EditorScene.h"

#include "AssetImportPlanner.h"
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
#include "imgui/ImguiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "input/Input.h"
#include "model/Model.h"
#include "model/ModelManager.h"
#include "model/MeshRenderer.h"
#include "sound/ISoundService.h"
#include "sprite/SpriteRenderer.h"
#include "texture/TextureManager.h"
#include "world/WorldSerializer.h"
#include "world/WorldCollision.h"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef DrawText
#undef DrawText
#endif

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

#include "internal/EditorSceneAssetUtils.h"

using namespace EditorSceneAssetUtils;

void EditorScene::DrawProjectPanel() {
    const ImVec2 panelPosition = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetWindowSize();
    projectPanelMinX_ = panelPosition.x;
    projectPanelMinY_ = panelPosition.y;
    projectPanelMaxX_ = panelPosition.x + panelSize.x;
    projectPanelMaxY_ = panelPosition.y + panelSize.y;
    if (pendingAssetDirectory_) {
        currentAssetDirectory_ = std::move(*pendingAssetDirectory_);
        pendingAssetDirectory_.reset();
        selectedAsset_.clear();
        RefreshAssetBrowser();
    }
    if (ImGui::Button("New")) {
        ImGui::OpenPopup("AssetCreateMenu");
    }
    if (ImGui::BeginPopup("AssetCreateMenu")) {
        if (ImGui::MenuItem("Folder")) {
            RequestCreateAssetFolder();
        }
        if (ImGui::MenuItem("Prefab from Selection...", nullptr, false,
                            selection_.IsValid() && !IsInPlayMode())) {
            SaveSelectionAsPrefab();
        }
        if (ImGui::MenuItem("Import Assets...")) {
            ImportAssetFiles();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RefreshAssetBrowser();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "%zu model(s), %zu texture(s), %zu audio(s), %zu font(s), %zu script(s), "
        "%zu prefab(s)",
        modelAssets_.size(), textureAssets_.size(), audioAssets_.size(),
        fontAssets_.size(), scriptAssets_.size(), prefabAssets_.size());
    ImGui::Separator();
    if (!currentAssetDirectory_.empty()) {
        if (ImGui::Button("< Back")) {
            NavigateAssetBrowser(currentAssetDirectory_.parent_path());
        }
        ImGui::SameLine();
    }
    DrawAssetBrowserBreadcrumbs();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetSearch", "Search assets...", assetSearch_.data(),
                             assetSearch_.size());

    constexpr const char* formatLabels[] = {
        "All formats", "Prefab", "C++ Script", "glTF", "GLB", "OBJ", "FBX", "DAE",
        "3DS", "PLY", "PNG", "JPG", "JPEG", "TGA", "BMP", "DDS", "HDR", "EXR",
        "WAV", "MP3", "AAC", "M4A", "WMA", "TTF", "OTF"};
    constexpr const char* formatExtensions[] = {
        "", ".likeprefab", ".cpp", ".gltf", ".glb", ".obj", ".fbx", ".dae", ".3ds",
        ".ply", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr",
        ".wav", ".mp3", ".aac", ".m4a", ".wma", ".ttf", ".otf"};
    constexpr const char* sortLabels[] = {"Name", "Type", "Size"};
    ImGui::SetNextItemWidth(105.0f);
    ImGui::Combo("##AssetFormat", &assetFormatFilter_, formatLabels,
                 static_cast<int>(std::size(formatLabels)));
    ImGui::SameLine();
    int sortMode = static_cast<int>(assetSortMode_);
    ImGui::SetNextItemWidth(75.0f);
    if (ImGui::Combo("##AssetSort", &sortMode, sortLabels,
                     static_cast<int>(std::size(sortLabels)))) {
        assetSortMode_ = static_cast<AssetSortMode>(sortMode);
    }
    ImGui::SameLine();
    if (ImGui::Button(assetSortAscending_ ? "Asc" : "Desc")) {
        assetSortAscending_ = !assetSortAscending_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle sort direction");
    }
    ImGui::Separator();

    const auto matchesFormat = [&](const std::filesystem::path& relativePath) {
        if (assetFormatFilter_ <= 0 ||
            assetFormatFilter_ >= static_cast<int>(std::size(formatExtensions))) {
            return true;
        }
        return LowercaseAscii(relativePath.extension().string()) ==
               formatExtensions[assetFormatFilter_];
    };
    const auto fileSize = [&](const std::filesystem::path& relativePath) {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(assetRoot_ / relativePath, error);
        return error ? uintmax_t{0} : size;
    };
    const auto comparePaths = [&](const std::filesystem::path& left,
                                  const std::filesystem::path& right) {
        int result = 0;
        if (assetSortMode_ == AssetSortMode::Type) {
            result = LowercaseAscii(left.extension().string()).compare(
                LowercaseAscii(right.extension().string()));
        } else if (assetSortMode_ == AssetSortMode::Size) {
            const uintmax_t leftSize = fileSize(left);
            const uintmax_t rightSize = fileSize(right);
            result = leftSize < rightSize ? -1 : (leftSize > rightSize ? 1 : 0);
        }
        if (result == 0) {
            result = LowercaseAscii(left.filename().string())
                         .compare(LowercaseAscii(right.filename().string()));
        }
        return assetSortAscending_ ? result < 0 : result > 0;
    };

    const float detailsHeight = selectedAsset_.empty() ? 0.0f : 190.0f;
    if (ImGui::BeginChild("AssetBrowserEntries", {0.0f, -detailsHeight},
                          ImGuiChildFlags_None)) {
        const std::string search(assetSearch_.data());
        if (!search.empty()) {
            std::vector<std::filesystem::path> matches;
            auto appendMatches = [&](const auto& assets) {
                for (const std::filesystem::path& logicalPath : assets) {
                    const std::filesystem::path relativePath =
                        logicalPath.lexically_relative("assets");
                    if (ContainsCaseInsensitive(logicalPath.generic_string(), search) &&
                        matchesFormat(relativePath)) {
                        matches.push_back(relativePath);
                    }
                }
            };
            appendMatches(modelAssets_);
            appendMatches(textureAssets_);
            appendMatches(audioAssets_);
            appendMatches(fontAssets_);
            appendMatches(scriptAssets_);
            appendMatches(prefabAssets_);
            std::ranges::sort(matches, comparePaths);
            for (const std::filesystem::path& relativePath : matches) {
                DrawAssetBrowserEntry(relativePath, false);
            }
            if (matches.empty()) {
                ImGui::TextDisabled("No matching assets.");
            }
        } else {
            std::vector<AssetBrowserEntry> visibleEntries;
            std::ranges::copy_if(assetBrowserEntries_, std::back_inserter(visibleEntries),
                                 [&](const AssetBrowserEntry& entry) {
                                     return entry.directory || matchesFormat(entry.relativePath);
                                 });
            std::ranges::sort(visibleEntries, [&](const AssetBrowserEntry& left,
                                                  const AssetBrowserEntry& right) {
                if (left.directory != right.directory) {
                    return left.directory;
                }
                return comparePaths(left.relativePath, right.relativePath);
            });
            for (const AssetBrowserEntry& entry : visibleEntries) {
                DrawAssetBrowserEntry(entry.relativePath, entry.directory);
            }
            if (visibleEntries.empty()) {
                ImGui::TextDisabled("This folder contains no matching assets or folders.");
            }
        }
    }
    ImGui::EndChild();
    if (!selectedAsset_.empty()) {
        DrawSelectedAssetDetails();
    }
}

void EditorScene::DrawAssetBrowserBreadcrumbs() {
    if (ImGui::SmallButton("assets")) {
        NavigateAssetBrowser({});
    }
    std::filesystem::path accumulated;
    for (const std::filesystem::path& component : currentAssetDirectory_) {
        if (component == L".") {
            continue;
        }
        accumulated /= component;
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextUnformatted(">");
        ImGui::SameLine(0.0f, 3.0f);
        const std::string label = component.string();
        const std::string id = accumulated.generic_string();
        ImGui::PushID(id.c_str());
        if (ImGui::SmallButton(label.c_str())) {
            NavigateAssetBrowser(accumulated);
        }
        ImGui::PopID();
    }
}

void EditorScene::DrawAssetBrowserEntry(const std::filesystem::path& relativePath,
                                        bool directory) {
    const std::filesystem::path logicalPath =
        (std::filesystem::path("assets") / relativePath).lexically_normal();
    const std::string id = logicalPath.generic_string();
    const bool texture = !directory && AssetImport::IsTextureFile(relativePath);
    const bool audio = !directory && AssetImport::IsAudioFile(relativePath);
    const bool font = !directory && AssetImport::IsFontFile(relativePath);
    const bool script = !directory && ScriptAssets::IsScriptFile(relativePath);
    const bool scriptSource =
        !directory && ScriptAssets::IsScriptSourceFile(relativePath);
    const bool scriptHeader = scriptSource && !script;
    const bool prefab = !directory && IsPrefabAsset(relativePath);
    const std::string label = std::string(directory ? "[Folder] "
                                                     : prefab ? "[Prefab] "
                                                     : texture ? "[Texture] "
                                                     : audio ? "[Audio] "
                                                     : font ? "[Font] "
                                                     : script ? "[Script] "
                                                     : scriptSource ? "[C++ Script] "
                                                                    : "[Model] ") +
                              relativePath.filename().string();
    ImGui::PushID(id.c_str());
    const bool selected = selectedAsset_ == relativePath;
    if (ImGui::Selectable(label.c_str(), selected,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
        selectedAsset_ = relativePath;
        if (directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            NavigateAssetBrowser(relativePath);
        } else if (prefab && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            InstantiatePrefabAsset(logicalPath);
        } else if (scriptSource &&
                   ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const std::filesystem::path physical = assetRoot_ / relativePath;
            if (reinterpret_cast<intptr_t>(ShellExecuteW(
                    nullptr, L"open", physical.c_str(), nullptr,
                    physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
                status_ = "Could not open Script source: " + id;
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", id.c_str());
    }
    if (!directory && !scriptHeader && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(prefab ? kPrefabAssetDragPayload
                                          : texture ? kTextureAssetDragPayload
                                          : audio ? kAudioAssetDragPayload
                                          : font ? kFontAssetDragPayload
                                          : script ? kScriptAssetDragPayload
                                                   : kModelAssetDragPayload,
                                  id.c_str(), id.size() + 1u);
        ImGui::TextUnformatted(id.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("AssetContext")) {
        selectedAsset_ = relativePath;
        if (directory) {
            if (ImGui::MenuItem("Open")) {
                NavigateAssetBrowser(relativePath);
            }
        } else if (scriptHeader) {
            if (ImGui::MenuItem("Open")) {
                const std::filesystem::path physical = assetRoot_ / relativePath;
                if (reinterpret_cast<intptr_t>(ShellExecuteW(
                        nullptr, L"open", physical.c_str(), nullptr,
                        physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
                    status_ = "Could not open Script source: " + id;
                }
            }
        } else if (prefab && ImGui::MenuItem("Instantiate")) {
            InstantiatePrefabAsset(logicalPath);
        } else if (script && ImGui::MenuItem("Attach to Selected Entity", nullptr, false,
                                             selection_.IsValid())) {
            AssignScriptAsset(selection_, logicalPath);
        } else if (font) {
            if (ImGui::MenuItem("Assign to Selected Text", nullptr, false,
                                selection_.IsValid())) {
                AssignTextFont(selection_, logicalPath);
            }
        } else if (!texture && ImGui::MenuItem("Create Entity")) {
            CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
        } else if (texture && ImGui::BeginMenu("Assign to Selected Material",
                                               selection_.IsValid())) {
            if (ImGui::MenuItem("Base Color")) {
                AssignBaseColorTexture(selection_, logicalPath);
            }
            if (ImGui::MenuItem("Normal Map")) {
                AssignNormalTexture(selection_, logicalPath);
            }
            if (ImGui::MenuItem("Roughness")) {
                AssignRoughnessTexture(selection_, logicalPath);
            }
            if (ImGui::MenuItem("Metallic")) {
                AssignMetallicTexture(selection_, logicalPath);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename")) {
            RequestAssetRename(relativePath, directory);
        }
        if (!directory && ImGui::MenuItem("Duplicate")) {
            DuplicateAsset(relativePath);
        }
        if (ImGui::MenuItem("Delete")) {
            RequestAssetDelete(relativePath, directory);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer")) {
            RevealAssetInExplorer(relativePath);
        }
        const size_t references = CountAssetReferences(relativePath, directory);
        if (ImGui::MenuItem("Select Referencing Entities", nullptr, false,
                            references != 0u)) {
            SelectAssetReferences(relativePath, directory);
        }
        if (!directory) {
            ImGui::Separator();
            const std::string uri =
                "asset://" + relativePath.lexically_normal().generic_string();
            if (ImGui::MenuItem("Copy Asset URI")) {
                ImGui::SetClipboardText(uri.c_str());
                status_ = "Copied asset URI: " + uri;
            }
            if (ImGui::MenuItem("Copy Project Path")) {
                ImGui::SetClipboardText(id.c_str());
                status_ = "Copied project asset path: " + id;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void EditorScene::DrawSelectedAssetDetails() {
    ImGui::SeparatorText("Selected Asset");
    const std::filesystem::path relative = selectedAsset_.lexically_normal();
    const std::filesystem::path physical = assetRoot_ / relative;
    const std::string logicalPath =
        (std::filesystem::path("assets") / relative).lexically_normal().generic_string();
    ImGui::TextWrapped("%s", logicalPath.c_str());

    std::error_code error;
    const bool directory = std::filesystem::is_directory(physical, error) && !error;
    error.clear();
    const bool regularFile = std::filesystem::is_regular_file(physical, error) && !error;
    if (!directory && !regularFile) {
        ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Asset no longer exists.");
        return;
    }
    const std::string extension = physical.extension().string();
    std::string typeLabel = directory
                                ? "Folder"
                                : IsPrefabAsset(physical)
                                      ? "Prefab"
                                : AssetImport::IsTextureFile(physical)
                                      ? "Texture"
                                : AssetImport::IsAudioFile(physical)
                                      ? "Audio"
                                : AssetImport::IsFontFile(physical)
                                      ? "Font"
                                      : ScriptAssets::IsScriptFile(physical)
                                            ? "Script"
                                            : ScriptAssets::IsScriptSourceFile(physical)
                                                  ? "C++ Script Source"
                                                  : "Model";
    if (regularFile && !extension.empty()) {
        typeLabel += " (" + extension + ")";
    }
    ImGui::TextDisabled("Type: %s", typeLabel.c_str());
    if (regularFile) {
        const uintmax_t bytes = std::filesystem::file_size(physical, error);
        if (!error) {
            constexpr double kilobyte = 1024.0;
            constexpr double megabyte = kilobyte * 1024.0;
            if (bytes >= static_cast<uintmax_t>(megabyte)) {
                ImGui::SameLine();
                ImGui::TextDisabled("Size: %.2f MB", static_cast<double>(bytes) / megabyte);
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("Size: %.1f KB", static_cast<double>(bytes) / kilobyte);
            }
        }
    }
    const size_t references = CountAssetReferences(relative, directory);
    ImGui::TextDisabled("Scene references: %zu", references);
    if (ImGui::SmallButton("Show in Explorer")) {
        RevealAssetInExplorer(relative);
    }
    ImGui::SameLine();
    if (references == 0u) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("Select References")) {
        SelectAssetReferences(relative, directory);
    }
    if (references == 0u) {
        ImGui::EndDisabled();
    }
    if (regularFile && AssetImport::IsAudioFile(physical)) {
        DrawAudioAssetPreview(physical);
        return;
    }
    if (!regularFile || !AssetImport::IsModelFile(physical)) {
        return;
    }

    if (assetPreviewAsset_ != relative) {
        ImGui::TextDisabled("Dependencies: Analyzing...");
        return;
    }
    if (!assetPreviewError_.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "Dependencies: Invalid");
        ImGui::SameLine();
        if (ImGui::SmallButton("Details##AssetDependencyError")) {
            status_ = assetPreviewError_;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", assetPreviewError_.c_str());
        }
        return;
    }
    const size_t dependencyCount =
        assetPreviewPlan_.empty() ? 0u : assetPreviewPlan_.size() - 1u;
    ImGui::TextDisabled("Dependencies: %zu", dependencyCount);
    ImGui::SameLine();
    if (ImGui::SmallButton("Create Entity##SelectedAsset")) {
        CreateModelEntityFromAsset(logicalPath, {0.0f, 0.0f, 0.0f});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a model entity at the scene origin");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Preview##SelectedAsset")) {
        ImGui::OpenPopup("Model Preview");
    }
    if (dependencyCount != 0u) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show##AssetDependencies")) {
            ImGui::OpenPopup("AssetDependencies");
        }
    }
    if (ImGui::BeginPopup("AssetDependencies")) {
        for (size_t index = 1; index < assetPreviewPlan_.size(); ++index) {
            const std::string dependency =
                (std::filesystem::path("assets") / relative.parent_path() /
                 assetPreviewPlan_[index].relativeDestination)
                    .lexically_normal()
                    .generic_string();
            ImGui::BulletText("%s", dependency.c_str());
        }
        ImGui::EndPopup();
    }
    DrawAssetPreviewPopup();
}

void EditorScene::DrawAssetPreviewPopup() {
    ImGui::SetNextWindowSize({360.0f, 460.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopup("Model Preview")) {
        return;
    }
    const std::string filename = assetPreviewAsset_.filename().string();
    ImGui::TextUnformatted(filename.c_str());
    ImGui::Separator();
    if (!assetPreviewModel_.IsValid() || !assetPreviewSurface_.IsReady() ||
        !assetPreviewPostProcess_.IsReady() || ctx_ == nullptr ||
        ctx_->rendering.dxCommon == nullptr || ctx_->rendering.model == nullptr) {
        ImGui::TextDisabled("Model preview is not ready.");
        ImGui::EndPopup();
        return;
    }

    ModelManager* modelManager = ctx_->rendering.model;
    Model* model = modelManager->GetModel(assetPreviewModel_);
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    std::unordered_set<uint32_t> materials;
    if (model != nullptr) {
        for (const ModelSubMesh& subMesh : model->subMeshes) {
            vertexCount += subMesh.vertexCount;
            if (IsValidResourceId(subMesh.meshId)) {
                triangleCount += ctx_->rendering.model->GetMesh(subMesh.meshId).indexCount / 3u;
            }
            if (IsValidResourceId(subMesh.materialId)) {
                materials.insert(subMesh.materialId);
            }
        }
        if (model->subMeshes.empty() && IsValidResourceId(model->meshId)) {
            const Mesh& mesh = ctx_->rendering.model->GetMesh(model->meshId);
            vertexCount = mesh.vertexStride == 0u ? 0u : mesh.vertexBytes / mesh.vertexStride;
            triangleCount = mesh.indexCount / 3u;
            if (IsValidResourceId(model->materialId)) {
                materials.insert(model->materialId);
            }
        }
        ImGui::TextDisabled("Meshes: %zu   Vertices: %llu   Triangles: %llu",
                            model->subMeshes.empty() ? size_t{1} : model->subMeshes.size(),
                            static_cast<unsigned long long>(vertexCount),
                            static_cast<unsigned long long>(triangleCount));
        ImGui::TextDisabled("Materials: %zu   Animations: %zu   Bones: %zu", materials.size(),
                            model->animations.size(), model->bones.size());
        if (!model->animations.empty()) {
            std::vector<std::string> animationNames;
            animationNames.reserve(model->animations.size());
            for (const auto& [name, clip] : model->animations) {
                (void)clip;
                animationNames.push_back(name);
            }
            std::ranges::sort(animationNames);
            if (assetPreviewAnimation_.empty() ||
                !model->animations.contains(assetPreviewAnimation_)) {
                assetPreviewAnimation_ = model->animations.contains(model->currentAnimation)
                                             ? model->currentAnimation
                                             : animationNames.front();
                modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                            assetPreviewAnimationLoop_);
            }
            if (ImGui::BeginCombo("Animation##ModelPreview",
                                  assetPreviewAnimation_.c_str())) {
                for (const std::string& name : animationNames) {
                    const bool selected = name == assetPreviewAnimation_;
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        assetPreviewAnimation_ = name;
                        modelManager->PlayAnimation(assetPreviewModel_, name,
                                                    assetPreviewAnimationLoop_);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (model->isPlaying) {
                if (ImGui::Button("Pause##ModelPreviewAnimation")) {
                    model->isPlaying = false;
                }
            } else if (ImGui::Button("Play##ModelPreviewAnimation")) {
                if (model->animationFinished) {
                    modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                                assetPreviewAnimationLoop_);
                } else {
                    model->isPlaying = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart##ModelPreviewAnimation")) {
                modelManager->PlayAnimation(assetPreviewModel_, assetPreviewAnimation_,
                                            assetPreviewAnimationLoop_);
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Loop##ModelPreviewAnimation", &assetPreviewAnimationLoop_)) {
                model->isLoop = assetPreviewAnimationLoop_;
            }
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Speed##ModelPreviewAnimation", &assetPreviewAnimationSpeed_, 0.01f,
                             0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            const AnimationClip& clip = model->animations.at(assetPreviewAnimation_);
            const float animationDuration = (std::max)(clip.duration, 0.0f);
            const auto seekAnimation = [&](const float time) {
                model->animationTime = std::clamp(time, 0.0f, animationDuration);
                model->isPlaying = false;
                model->animationFinished =
                    animationDuration > 0.0f && model->animationTime >= animationDuration;
                modelManager->UpdateAnimation(assetPreviewModel_, 0.0f);
            };
            constexpr float kAnimationPreviewStepSeconds = 1.0f / 30.0f;
            if (ImGui::SmallButton("|<##ModelPreviewAnimation")) {
                seekAnimation(0.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("<##ModelPreviewAnimation")) {
                seekAnimation(model->animationTime - kAnimationPreviewStepSeconds);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Step backward 1/30 second");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">##ModelPreviewAnimation")) {
                seekAnimation(model->animationTime + kAnimationPreviewStepSeconds);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Step forward 1/30 second");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">|##ModelPreviewAnimation")) {
                seekAnimation(animationDuration);
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            float animationTime = model->animationTime;
            if (ImGui::SliderFloat("##ModelPreviewAnimationTimeline", &animationTime, 0.0f,
                                   animationDuration, "%.2f s",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                seekAnimation(animationTime);
            }
            ImGui::TextDisabled("%.2f / %.2f s", model->animationTime, animationDuration);
            if (model->isPlaying) {
                const float deltaTime =
                    std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f) *
                    assetPreviewAnimationSpeed_;
                modelManager->UpdateAnimation(assetPreviewModel_, deltaTime);
            }
        }
    }

    if (ImGui::SmallButton("Reset View##ModelPreview")) {
        assetPreviewRotationDegrees_ = {0.0f, 180.0f};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drag the preview to rotate");
    DirectX::XMStoreFloat4(
        &assetPreviewTransform_.rotation,
        DirectX::XMQuaternionRotationRollPitchYaw(
            DirectX::XMConvertToRadians(assetPreviewRotationDegrees_.x),
            DirectX::XMConvertToRadians(assetPreviewRotationDegrees_.y), 0.0f));
    assetPreviewSurface_.BeginScenePass({0.035f, 0.045f, 0.065f, 1.0f});
    ModelRenderer* previewRenderer = modelManager->GetRenderer();
    previewRenderer->PreDraw();
    modelManager->Draw(assetPreviewModel_, assetPreviewTransform_, assetPreviewCamera_);
    ModelRenderer::PostDraw();
    assetPreviewSurface_.EndScenePass();
    assetPreviewSurface_.TransitionDepthToShaderResource();
    assetPreviewSurface_.BeginOutputPass({0.0f, 0.0f, 0.0f, 1.0f});
    const PostProcessOutputTarget target{
        assetPreviewSurface_.GetOutputRtvHandle(),
        static_cast<uint32_t>(assetPreviewSurface_.GetWidth()),
        static_cast<uint32_t>(assetPreviewSurface_.GetHeight()),
        DirectXCommon::kBackBufferFormat,
    };
    assetPreviewPostProcess_.DrawToTarget(assetPreviewSurface_.GetSceneColorGpuHandle(),
                                          assetPreviewSurface_.GetDepthGpuHandle(), target);
    assetPreviewSurface_.EndOutputPass();
    assetPreviewSurface_.TransitionDepthToWrite();
    ctx_->rendering.dxCommon->SetBackBufferRenderTarget(false, false);
    const D3D12_GPU_DESCRIPTOR_HANDLE output = assetPreviewSurface_.GetOutputGpuHandle();
    ImGui::Image(static_cast<ImTextureID>(output.ptr), {320.0f, 320.0f});
    if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        assetPreviewRotationDegrees_.x =
            std::clamp(assetPreviewRotationDegrees_.x + delta.y * 0.4f, -89.0f, 89.0f);
        assetPreviewRotationDegrees_.y += delta.x * 0.4f;
    }
    ImGui::EndPopup();
}

void EditorScene::RequestAssetRename(const std::filesystem::path& relativePath,
                                     bool directory) {
    pendingAssetOperationPath_ = relativePath.lexically_normal();
    pendingAssetOperationIsDirectory_ = directory;
    assetRenameBuffer_.fill('\0');
    const std::string filename = pendingAssetOperationPath_.filename().string();
    strncpy_s(assetRenameBuffer_.data(), assetRenameBuffer_.size(), filename.c_str(), _TRUNCATE);
    showAssetRenameDialog_ = true;
    focusAssetRenameInput_ = true;
}

void EditorScene::RequestAssetDelete(const std::filesystem::path& relativePath,
                                     bool directory) {
    pendingAssetOperationPath_ = relativePath.lexically_normal();
    pendingAssetOperationIsDirectory_ = directory;
    showAssetDeleteDialog_ = true;
}

void EditorScene::RequestCreateAssetFolder() {
    assetFolderNameBuffer_.fill('\0');
    strncpy_s(assetFolderNameBuffer_.data(), assetFolderNameBuffer_.size(), "New Folder",
              _TRUNCATE);
    showCreateAssetFolderDialog_ = true;
    focusAssetFolderNameInput_ = true;
}

void EditorScene::DrawAssetOperationDialogs() {
    if (showAssetRenameDialog_) {
        ImGui::OpenPopup("Rename Asset");
        showAssetRenameDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("assets/%s", pendingAssetOperationPath_.generic_string().c_str());
        if (focusAssetRenameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusAssetRenameInput_ = false;
        }
        ImGui::SetNextItemWidth(360.0f);
        const bool submitted = ImGui::InputText(
            "##AssetName", assetRenameBuffer_.data(), assetRenameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (submitted || ImGui::Button("Rename", {100.0f, 0.0f})) {
            if (RenamePendingAsset()) {
                pendingAssetOperationPath_.clear();
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::SameLine();
            if (cancel || ImGui::Button("Cancel", {100.0f, 0.0f})) {
                pendingAssetOperationPath_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (showAssetDeleteDialog_) {
        ImGui::OpenPopup("Delete Asset");
        showAssetDeleteDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(pendingAssetOperationIsDirectory_
                                   ? "Delete this asset folder and all of its contents?"
                                   : "Delete this asset file?");
        ImGui::TextDisabled("assets/%s", pendingAssetOperationPath_.generic_string().c_str());
        const bool referenced =
            IsAssetReferenced(pendingAssetOperationPath_, pendingAssetOperationIsDirectory_);
        if (referenced) {
            ImGui::TextColored({1.0f, 0.45f, 0.3f, 1.0f},
                               "Cannot delete: the current scene references this asset.");
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Delete", {100.0f, 0.0f}) && DeletePendingAsset()) {
            pendingAssetOperationPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        if (referenced) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100.0f, 0.0f}) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pendingAssetOperationPath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showCreateAssetFolderDialog_) {
        ImGui::OpenPopup("Create Asset Folder");
        showCreateAssetFolderDialog_ = false;
    }
    if (ImGui::BeginPopupModal("Create Asset Folder", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::filesystem::path parent =
            (std::filesystem::path("assets") / currentAssetDirectory_).lexically_normal();
        ImGui::TextDisabled("In %s", parent.generic_string().c_str());
        if (focusAssetFolderNameInput_) {
            ImGui::SetKeyboardFocusHere();
            focusAssetFolderNameInput_ = false;
        }
        ImGui::SetNextItemWidth(360.0f);
        const bool submitted = ImGui::InputText(
            "##AssetFolderName", assetFolderNameBuffer_.data(),
            assetFolderNameBuffer_.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (submitted || ImGui::Button("Create", {100.0f, 0.0f})) {
            if (CreatePendingAssetFolder()) {
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::SameLine();
            if (cancel || ImGui::Button("Cancel", {100.0f, 0.0f})) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

bool EditorScene::RenamePendingAsset() {
    const std::filesystem::path oldRelative = pendingAssetOperationPath_.lexically_normal();
    const std::string filename(assetRenameBuffer_.data());
    if (oldRelative.empty() || oldRelative.is_absolute() || HasParentTraversal(oldRelative) ||
        !IsValidAssetFilename(filename)) {
        status_ = "Asset rename rejected an invalid name.";
        return false;
    }
    const std::filesystem::path filenamePath(filename);
    std::string newExtension = filenamePath.extension().string();
    std::string oldExtension = oldRelative.extension().string();
    std::ranges::transform(newExtension, newExtension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::ranges::transform(oldExtension, oldExtension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (!pendingAssetOperationIsDirectory_ && newExtension != oldExtension) {
        status_ = "Asset rename cannot change a file extension.";
        return false;
    }
    const std::filesystem::path newRelative =
        (oldRelative.parent_path() / filenamePath).lexically_normal();
    if (newRelative == oldRelative) {
        status_ = "The asset already has that name.";
        return false;
    }
    const std::filesystem::path source = assetRoot_ / oldRelative;
    const std::filesystem::path destination = assetRoot_ / newRelative;
    std::error_code error;
    const bool sourceTypeMatches =
        pendingAssetOperationIsDirectory_ ? std::filesystem::is_directory(source, error)
                                          : std::filesystem::is_regular_file(source, error);
    if (error || !sourceTypeMatches || !IsPathWithinRoot(assetRoot_, source)) {
        status_ = "Asset rename failed because the source no longer exists.";
        return false;
    }
    error.clear();
    if (!IsPathAtOrWithinRoot(assetRoot_, source.parent_path()) ||
        std::filesystem::exists(destination, error) || error) {
        status_ = "Asset rename failed because the destination is invalid or already exists.";
        return false;
    }
    std::filesystem::rename(source, destination, error);
    if (error) {
        status_ = "Asset rename failed: " + error.message();
        return false;
    }
    const size_t updatedReferences =
        UpdateAssetReferences(oldRelative, newRelative, pendingAssetOperationIsDirectory_);
    selectedAsset_ = newRelative;
    loadedModels_.clear();
    animatorModels_.clear();
    RefreshAssetBrowser();
    RefreshDirty();
    status_ = "Renamed asset to assets/" + newRelative.generic_string();
    if (updatedReferences != 0u) {
        status_ += " and updated " + std::to_string(updatedReferences) + " scene reference(s).";
    }
    return true;
}

bool EditorScene::DeletePendingAsset() {
    const std::filesystem::path relative = pendingAssetOperationPath_.lexically_normal();
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        IsAssetReferenced(relative, pendingAssetOperationIsDirectory_)) {
        status_ = "Asset deletion rejected an invalid or referenced path.";
        return false;
    }
    const std::filesystem::path physical = assetRoot_ / relative;
    if (!IsPathWithinRoot(assetRoot_, physical)) {
        status_ = "Asset deletion rejected a path outside the project assets directory.";
        return false;
    }
    std::error_code error;
    const uintmax_t removed = std::filesystem::remove_all(physical, error);
    if (error || removed == 0u) {
        status_ = "Asset deletion failed" +
                  (error ? std::string(": ") + error.message() : std::string("."));
        return false;
    }
    selectedAsset_.clear();
    loadedModels_.clear();
    animatorModels_.clear();
    RefreshAssetBrowser();
    status_ = "Deleted asset: assets/" + relative.generic_string();
    return true;
}

bool EditorScene::DuplicateAsset(const std::filesystem::path& relativePath) {
    const std::filesystem::path relative = relativePath.lexically_normal();
    const std::filesystem::path source = assetRoot_ / relative;
    std::error_code error;
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        !std::filesystem::is_regular_file(source, error) || error ||
        !IsPathWithinRoot(assetRoot_, source)) {
        status_ = "Asset duplication rejected an invalid source.";
        return false;
    }
    const std::string stem = relative.stem().string();
    const std::string extension = relative.extension().string();
    std::filesystem::path duplicateRelative;
    for (size_t copyIndex = 1; copyIndex <= 100u; ++copyIndex) {
        const std::string suffix = copyIndex == 1u ? " Copy" : " Copy (" +
                                                                   std::to_string(copyIndex) + ")";
        duplicateRelative = relative.parent_path() / (stem + suffix + extension);
        if (!std::filesystem::exists(assetRoot_ / duplicateRelative, error) && !error) {
            break;
        }
        duplicateRelative.clear();
        error.clear();
    }
    if (duplicateRelative.empty()) {
        status_ = "Asset duplication could not find an available filename.";
        return false;
    }
    std::filesystem::copy_file(source, assetRoot_ / duplicateRelative,
                               std::filesystem::copy_options::none, error);
    if (error) {
        status_ = "Asset duplication failed: " + error.message();
        return false;
    }
    selectedAsset_ = duplicateRelative;
    RefreshAssetBrowser();
    status_ = "Duplicated asset: assets/" + duplicateRelative.generic_string();
    return true;
}

bool EditorScene::CreatePendingAssetFolder() {
    const std::string folderName(assetFolderNameBuffer_.data());
    if (!IsValidAssetFilename(folderName)) {
        status_ = "Asset folder creation rejected an invalid name.";
        return false;
    }
    const std::filesystem::path parent = assetRoot_ / currentAssetDirectory_;
    const std::filesystem::path destination = parent / std::filesystem::path(folderName);
    if (!IsPathAtOrWithinRoot(assetRoot_, parent)) {
        status_ = "Asset folder creation rejected a path outside the assets directory.";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(destination, error) || error) {
        status_ = "Asset folder creation failed because that name already exists.";
        return false;
    }
    if (!std::filesystem::create_directory(destination, error) || error) {
        status_ = "Asset folder creation failed" +
                  (error ? std::string(": ") + error.message() : std::string("."));
        return false;
    }
    selectedAsset_ = (currentAssetDirectory_ / folderName).lexically_normal();
    RefreshAssetBrowser();
    status_ = "Created asset folder: assets/" + selectedAsset_.generic_string();
    return true;
}

bool EditorScene::ImportAssetFiles() {
    const std::vector<std::filesystem::path> selectedFiles = ShowImportAssetDialog();
    if (selectedFiles.empty()) {
        return false;
    }
    return ImportAssetFiles(selectedFiles);
}

bool EditorScene::ImportAssetFiles(
    const std::vector<std::filesystem::path>& selectedFiles) {
    const std::filesystem::path destinationDirectory =
        assetRoot_ / currentAssetDirectory_;
    if (!IsPathAtOrWithinRoot(assetRoot_, destinationDirectory)) {
        status_ = "Asset import rejected an invalid destination.";
        return false;
    }

    std::vector<AssetImport::File> importFiles;
    std::string importError;
    if (!AssetImport::BuildPlan(selectedFiles, importFiles, importError)) {
        status_ = "Asset import stopped: " + importError;
        return false;
    }

    size_t alreadyPresent = 0;
    std::error_code error;
    for (const AssetImport::File& file : importFiles) {
        const std::filesystem::path destination =
            destinationDirectory / file.relativeDestination;
        if (!IsPathAtOrWithinRoot(assetRoot_, destination.parent_path())) {
            status_ = "Asset import rejected a dependency destination outside assets/.";
            return false;
        }
        error.clear();
        if (!std::filesystem::exists(destination, error) && !error) {
            continue;
        }
        if (error || !std::filesystem::is_regular_file(destination, error) || error ||
            !AssetImport::HaveEqualContents(file.source, destination)) {
            status_ = "Asset import stopped because assets/" +
                      (currentAssetDirectory_ / file.relativeDestination).generic_string() +
                      " already exists with different contents.";
            return false;
        }
        ++alreadyPresent;
    }

    std::vector<std::filesystem::path> copiedFiles;
    std::vector<std::filesystem::path> createdDirectories;
    copiedFiles.reserve(importFiles.size());
    for (const AssetImport::File& file : importFiles) {
        const std::filesystem::path destination =
            destinationDirectory / file.relativeDestination;
        error.clear();
        if (std::filesystem::exists(destination, error) && !error) {
            continue;
        }
        const std::filesystem::path parent = destination.parent_path();
        std::vector<std::filesystem::path> missingDirectories;
        for (std::filesystem::path directory = parent;
             directory != destinationDirectory && !directory.empty() &&
             !std::filesystem::exists(directory, error);
             directory = directory.parent_path()) {
            if (error) {
                break;
            }
            missingDirectories.push_back(directory);
        }
        for (auto directory = missingDirectories.rbegin();
             !error && directory != missingDirectories.rend(); ++directory) {
            if (std::filesystem::create_directory(*directory, error)) {
                createdDirectories.push_back(*directory);
            }
        }
        if (error) {
            for (const std::filesystem::path& copied : copiedFiles) {
                std::error_code rollbackError;
                std::filesystem::remove(copied, rollbackError);
            }
            for (auto directory = createdDirectories.rbegin();
                 directory != createdDirectories.rend(); ++directory) {
                std::error_code rollbackError;
                std::filesystem::remove(*directory, rollbackError);
            }
            status_ = "Asset import failed and was rolled back: " + error.message();
            RefreshAssetBrowser();
            return false;
        }
        error.clear();
        std::filesystem::copy_file(file.source, destination, std::filesystem::copy_options::none,
                                   error);
        if (error) {
            for (const std::filesystem::path& copied : copiedFiles) {
                std::error_code rollbackError;
                std::filesystem::remove(copied, rollbackError);
            }
            for (auto directory = createdDirectories.rbegin();
                 directory != createdDirectories.rend(); ++directory) {
                std::error_code rollbackError;
                std::filesystem::remove(*directory, rollbackError);
            }
            status_ = "Asset import failed and was rolled back: " + error.message();
            RefreshAssetBrowser();
            return false;
        }
        copiedFiles.push_back(destination);
    }

    selectedAsset_ =
        (currentAssetDirectory_ / selectedFiles.front().filename()).lexically_normal();
    RefreshAssetBrowser();
    status_ = "Imported " + std::to_string(copiedFiles.size()) +
              " new asset file(s) into assets/" + currentAssetDirectory_.generic_string();
    if (alreadyPresent != 0u) {
        status_ += " (kept " + std::to_string(alreadyPresent) + " identical file(s)).";
    }
    return true;
}

bool EditorScene::RevealAssetInExplorer(const std::filesystem::path& relativePath) {
    const std::filesystem::path relative = relativePath.lexically_normal();
    const std::filesystem::path physical = assetRoot_ / relative;
    std::error_code error;
    if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative) ||
        !std::filesystem::exists(physical, error) || error ||
        !IsPathWithinRoot(assetRoot_, physical)) {
        status_ = "Could not reveal an invalid or missing asset path.";
        return false;
    }
    HINSTANCE result = nullptr;
    if (std::filesystem::is_directory(physical, error) && !error) {
        result = ShellExecuteW(nullptr, L"open", physical.c_str(), nullptr, nullptr,
                               SW_SHOWNORMAL);
    } else {
        const std::wstring arguments = L"/select,\"" + physical.wstring() + L"\"";
        result = ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                               SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        status_ = "Could not open the asset location in Explorer.";
        return false;
    }
    status_ = "Opened asset location: assets/" + relative.generic_string();
    return true;
}

void EditorScene::SelectAssetReferences(const std::filesystem::path& relativePath,
                                        bool directory) {
    hierarchySelection_.clear();
    selection_ = {};
    for (const WorldEntity& entity : world_.Entities()) {
        const std::optional<std::filesystem::path> modelReference =
            entity.meshRenderer && entity.meshRenderer->sourceType == MeshSourceType::Model
                ? AssetRelativeFromReference(entity.meshRenderer->modelPath)
                : std::nullopt;
        const std::optional<std::filesystem::path> textureReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->baseColorTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> normalReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->normalTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> roughnessReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->roughnessTexturePath)
                : std::nullopt;
        const std::optional<std::filesystem::path> metallicReference =
            entity.materialOverride
                ? AssetRelativeFromReference(entity.materialOverride->metallicTexturePath)
                : std::nullopt;
        const bool matchesModel =
            modelReference && AssetPathMatches(*modelReference, relativePath, directory);
        const bool matchesTexture =
            textureReference && AssetPathMatches(*textureReference, relativePath, directory);
        const bool matchesNormal =
            normalReference && AssetPathMatches(*normalReference, relativePath, directory);
        const bool matchesRoughness =
            roughnessReference && AssetPathMatches(*roughnessReference, relativePath, directory);
        const bool matchesMetallic =
            metallicReference && AssetPathMatches(*metallicReference, relativePath, directory);
        const bool matchesScript = std::ranges::any_of(
            entity.scripts, [&](const BehaviorComponent& script) {
                const std::optional<std::filesystem::path> referenced =
                    AssetRelativeFromReference(script.scriptAssetPath);
                return referenced && AssetPathMatches(*referenced, relativePath, directory);
            });
        const std::optional<std::filesystem::path> audioReference =
            entity.audioSource
                ? AssetRelativeFromReference(entity.audioSource->clipPath)
                : std::nullopt;
        const bool matchesAudio =
            audioReference && AssetPathMatches(*audioReference, relativePath, directory);
        const std::optional<std::filesystem::path> imageReference =
            entity.image
                ? AssetRelativeFromReference(entity.image->texturePath)
                : std::nullopt;
        const bool matchesImage =
            imageReference &&
            AssetPathMatches(*imageReference, relativePath, directory);
        const std::optional<std::filesystem::path> fontReference =
            entity.text
                ? AssetRelativeFromReference(entity.text->fontPath)
                : std::nullopt;
        const bool matchesFont =
            fontReference &&
            AssetPathMatches(*fontReference, relativePath, directory);
        if (!matchesModel && !matchesTexture && !matchesNormal && !matchesRoughness &&
            !matchesMetallic && !matchesScript && !matchesAudio &&
            !matchesImage && !matchesFont) {
            continue;
        }
        hierarchySelection_.insert(entity.id);
        if (!selection_.IsValid()) {
            selection_ = entity.id;
        }
    }
    hierarchySelectionAnchor_ = selection_;
    showHierarchyPanel_ = true;
    status_ = hierarchySelection_.empty()
                  ? "No scene entities reference the selected asset."
                  : "Selected " + std::to_string(hierarchySelection_.size()) +
                        " entity reference(s) to assets/" +
                        relativePath.lexically_normal().generic_string();
}

bool EditorScene::IsAssetReferenced(const std::filesystem::path& relativePath,
                                    bool directory) const {
    return CountAssetReferences(relativePath, directory) != 0u;
}

size_t EditorScene::CountAssetReferences(const std::filesystem::path& relativePath,
                                         bool directory) const {
    size_t references = 0;
    for (const WorldEntity& entity : world_.Entities()) {
        bool referencedByEntity = false;
        if (entity.meshRenderer && entity.meshRenderer->sourceType == MeshSourceType::Model) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.meshRenderer->modelPath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->baseColorTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->roughnessTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->metallicTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.materialOverride) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.materialOverride->normalTexturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity) {
            referencedByEntity = std::ranges::any_of(
                entity.scripts, [&](const BehaviorComponent& script) {
                    const std::optional<std::filesystem::path> referenced =
                        AssetRelativeFromReference(script.scriptAssetPath);
                    return referenced &&
                           AssetPathMatches(*referenced, relativePath, directory);
                });
        }
        if (!referencedByEntity && entity.audioSource) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.audioSource->clipPath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.image) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.image->texturePath);
            referencedByEntity =
                referenced && AssetPathMatches(*referenced, relativePath, directory);
        }
        if (!referencedByEntity && entity.text) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(entity.text->fontPath);
            referencedByEntity =
                referenced &&
                AssetPathMatches(*referenced, relativePath, directory);
        }
        if (referencedByEntity) {
            ++references;
        }
    }
    return references;
}

size_t EditorScene::UpdateAssetReferences(const std::filesystem::path& oldRelativePath,
                                          const std::filesystem::path& newRelativePath,
                                          bool directory) {
    size_t updated = 0;
    for (const WorldEntity& candidate : world_.Entities()) {
        WorldEntity* entity = world_.Find(candidate.id);
        if (entity == nullptr) {
            continue;
        }
        auto updateReference = [&](std::string& reference) {
            const std::optional<std::filesystem::path> referenced =
                AssetRelativeFromReference(reference);
            if (!referenced || !AssetPathMatches(*referenced, oldRelativePath, directory)) {
                return;
            }
            const std::filesystem::path suffix = referenced->lexically_relative(oldRelativePath);
            const std::filesystem::path replacement =
                suffix.empty() || suffix == L"." ? newRelativePath : newRelativePath / suffix;
            reference = "asset://" + replacement.lexically_normal().generic_string();
            ++updated;
        };
        if (entity->meshRenderer && entity->meshRenderer->sourceType == MeshSourceType::Model) {
            updateReference(entity->meshRenderer->modelPath);
        }
        if (entity->materialOverride) {
            updateReference(entity->materialOverride->baseColorTexturePath);
            updateReference(entity->materialOverride->normalTexturePath);
            updateReference(entity->materialOverride->roughnessTexturePath);
            updateReference(entity->materialOverride->metallicTexturePath);
        }
        if (entity->audioSource) {
            updateReference(entity->audioSource->clipPath);
        }
        if (entity->image) {
            updateReference(entity->image->texturePath);
        }
        if (entity->text) {
            updateReference(entity->text->fontPath);
        }
        for (BehaviorComponent& script : entity->scripts) {
            updateReference(script.scriptAssetPath);
        }
    }
    return updated;
}

