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

void EditorScene::CaptureConsoleStatus() {
    if (status_.empty() || status_ == lastCapturedStatus_) {
        return;
    }
    lastCapturedStatus_ = status_;
    std::string normalized = status_;
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    ConsoleSeverity severity = ConsoleSeverity::Info;
    if (normalized.find("failed") != std::string::npos ||
        normalized.find("failure") != std::string::npos ||
        normalized.find("error") != std::string::npos) {
        severity = ConsoleSeverity::Error;
    } else if (normalized.find("could not") != std::string::npos ||
               normalized.find("cannot") != std::string::npos ||
               normalized.find("invalid") != std::string::npos ||
               normalized.find("rejected") != std::string::npos ||
               normalized.find("warning") != std::string::npos ||
               normalized.find("unavailable") != std::string::npos) {
        severity = ConsoleSeverity::Warning;
    }
    AddConsoleEntry(status_, severity);
}

void EditorScene::AddConsoleEntry(std::string message, ConsoleSeverity severity,
                                  std::filesystem::path sourcePath,
                                  uint32_t sourceLine, uint32_t sourceColumn) {
    if (message.empty()) {
        return;
    }
    consoleEntries_.push_back({std::move(message), ImGui::GetTime(), severity,
                               std::move(sourcePath), sourceLine, sourceColumn});
    constexpr size_t kMaxConsoleEntries = 512u;
    if (consoleEntries_.size() > kMaxConsoleEntries) {
        consoleEntries_.erase(consoleEntries_.begin(),
                              consoleEntries_.begin() +
                                  static_cast<ptrdiff_t>(consoleEntries_.size() -
                                                         kMaxConsoleEntries));
    }
    consoleScrollToBottom_ = true;
}

bool EditorScene::OpenConsoleSource(const std::filesystem::path& sourcePath,
                                    uint32_t sourceLine) {
    std::filesystem::path physical = sourcePath;
    if (physical.is_relative()) {
        physical = projectRoot_ / physical;
    }
    std::error_code error;
    physical = std::filesystem::weakly_canonical(physical, error);
    if (error || !std::filesystem::is_regular_file(physical, error) || error) {
        status_ = "Could not open compiler source because the file no longer exists.";
        return false;
    }
    if (reinterpret_cast<intptr_t>(ShellExecuteW(
            nullptr, L"open", physical.c_str(), nullptr,
            physical.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
        status_ = "Could not open compiler source: " + physical.generic_string();
        return false;
    }
    status_ = "Opened compiler source: " + physical.filename().string();
    if (sourceLine > 0u) {
        status_ += " (line " + std::to_string(sourceLine) + ").";
    }
    return true;
}

void EditorScene::InitializeScriptMonitoring() {
    lastScriptScanTime_ = std::chrono::steady_clock::now();
    lastScriptChangeTime_ = lastScriptScanTime_;
    std::string error;
    if (ScriptBuildService::GetSourceFingerprint(projectRoot_, scriptSourceFingerprint_,
                                                 error)) {
        scriptFingerprintInitialized_ = true;
    } else {
        status_ = "Warning: Project Script monitoring could not start: " + error;
    }
}

void EditorScene::UpdateScriptCompilation() {
    constexpr auto scanInterval = std::chrono::milliseconds(500);
    constexpr auto buildDebounce = std::chrono::milliseconds(750);
    const auto now = std::chrono::steady_clock::now();

    if (scriptBuildInProgress_ &&
        scriptBuildFuture_.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        FinishScriptCompilation();
    }

    if (now - lastScriptScanTime_ >= scanInterval) {
        lastScriptScanTime_ = now;
        uint64_t fingerprint = 0u;
        std::string error;
        if (ScriptBuildService::GetSourceFingerprint(projectRoot_, fingerprint, error)) {
            if (!scriptFingerprintInitialized_) {
                scriptSourceFingerprint_ = fingerprint;
                scriptFingerprintInitialized_ = true;
            } else if (fingerprint != scriptSourceFingerprint_) {
                scriptSourceFingerprint_ = fingerprint;
                scriptBuildPending_ = true;
                lastScriptChangeTime_ = now;
                if (IsInPlayMode()) {
                    if (!scriptChangesDeferredMessageShown_) {
                        status_ = "Project Script changes detected. Compilation is deferred "
                                  "until Play Mode stops.";
                        scriptChangesDeferredMessageShown_ = true;
                    }
                } else {
                    status_ = "Project Script changes detected.";
                }
            }
        } else {
            status_ = "Warning: Project Script change detection failed: " + error;
        }
    }

    if (!scriptBuildInProgress_ && scriptBuildPending_ && !IsInPlayMode() &&
        now - lastScriptChangeTime_ >= buildDebounce) {
        StartScriptCompilation();
    }
}

void EditorScene::StartScriptCompilation() {
    if (scriptBuildInProgress_ || IsInPlayMode()) {
        return;
    }
    scriptBuildPending_ = false;
    scriptBuildInProgress_ = true;
    scriptChangesDeferredMessageShown_ = false;
    status_ = "Compiling Project Scripts...";
    const std::filesystem::path projectRoot = projectRoot_;
    try {
        scriptBuildFuture_ = std::async(std::launch::async, [projectRoot] {
            ScriptBuildCompletion completion{};
            completion.succeeded = ScriptBuildService::Build(
                projectRoot, completion.error, &completion.output);
            return completion;
        });
    } catch (const std::exception& exception) {
        scriptBuildInProgress_ = false;
        status_ = "Error: Could not start Project Script compilation: " +
                  std::string(exception.what());
    }
}

void EditorScene::FinishScriptCompilation() {
    ScriptBuildCompletion completion = scriptBuildFuture_.get();
    scriptBuildInProgress_ = false;
    if (!completion.output.empty()) {
        constexpr size_t maximumOutputLength = 48u * 1024u;
        if (completion.output.size() > maximumOutputLength) {
            completion.output.erase(0u,
                                    completion.output.size() - maximumOutputLength);
            completion.output.insert(0u, "... compiler output truncated ...\n");
        }
        AddConsoleEntry("Project Script compiler output:", ConsoleSeverity::Info);
        std::istringstream stream(completion.output);
        std::string outputLine;
        while (std::getline(stream, outputLine)) {
            if (!outputLine.empty() && outputLine.back() == '\r') {
                outputLine.pop_back();
            }
            if (outputLine.empty()) {
                continue;
            }
            std::string normalized = LowercaseAscii(outputLine);
            ConsoleSeverity severity = ConsoleSeverity::Info;
            if (normalized.find("error") != std::string::npos ||
                normalized.find("failed") != std::string::npos) {
                severity = ConsoleSeverity::Error;
            } else if (normalized.find("warning") != std::string::npos) {
                severity = ConsoleSeverity::Warning;
            }
            std::filesystem::path sourcePath;
            uint32_t sourceLine = 0u;
            uint32_t sourceColumn = 0u;
            ScriptBuildService::ParseDiagnosticLocation(
                outputLine, sourcePath, sourceLine, sourceColumn);
            AddConsoleEntry(std::move(outputLine), severity, std::move(sourcePath),
                            sourceLine, sourceColumn);
        }
    }
    if (!completion.succeeded) {
        status_ = "Error: " +
                  (completion.error.empty() ?
                       std::string("Project Script compilation failed.") :
                       completion.error);
        return;
    }
    if (IsInPlayMode()) {
        scriptBuildPending_ = true;
        lastScriptChangeTime_ = std::chrono::steady_clock::now();
        scriptChangesDeferredMessageShown_ = true;
        status_ = "Project Script compilation finished during Play Mode. Reload is "
                  "deferred until Play Mode stops.";
        return;
    }
    if (scriptBuildPending_) {
        status_ = "Project Scripts changed again during compilation. Rebuilding...";
        return;
    }

    std::string reloadError;
    if (!ReloadProjectScripts(reloadError)) {
        status_ = "Error: Project Script reload failed: " + reloadError;
        return;
    }
    std::string requirementError;
    if (!ValidateWorldBehaviorRequirements(&requirementError)) {
        status_ = "Warning: Project Scripts reloaded, but the scene contains an invalid "
                  "Behavior: " + requirementError;
    } else {
        status_ = "Project Scripts compiled and reloaded successfully (" +
                  std::to_string(behaviorRegistry_.Types().size()) + " type(s)).";
    }
}

bool EditorScene::ReloadProjectScripts(std::string& error) {
    if (IsInPlayMode() || ctx_ == nullptr) {
        error = "Project Scripts can only be reloaded in Edit Mode.";
        return false;
    }
    ProjectScriptLibrary newLibrary;
    BehaviorRegistry newRegistry;
    if (!newLibrary.Load(projectRoot_, ctx_->systems.input, newRegistry, error)) {
        return false;
    }

    // Destroy factories that point into the old DLL before unloading that DLL.
    behaviorRegistry_ = std::move(newRegistry);
    projectScripts_ = std::move(newLibrary);
    (void)UpgradeInputActionReferences();
    RefreshAssetBrowser();
    error.clear();
    return true;
}

void EditorScene::DrawConsolePanel() {
    size_t infoCount = 0;
    size_t warningCount = 0;
    size_t errorCount = 0;
    for (const ConsoleEntry& entry : consoleEntries_) {
        switch (entry.severity) {
        case ConsoleSeverity::Info:
            ++infoCount;
            break;
        case ConsoleSeverity::Warning:
            ++warningCount;
            break;
        case ConsoleSeverity::Error:
            ++errorCount;
            break;
        }
    }
    if (ImGui::Button("Clear")) {
        consoleEntries_.clear();
        lastCapturedStatus_ = status_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string text;
        for (const ConsoleEntry& entry : consoleEntries_) {
            const char* severity = entry.severity == ConsoleSeverity::Error
                                       ? "Error"
                                       : entry.severity == ConsoleSeverity::Warning ? "Warning"
                                                                                    : "Info";
            text += '[';
            text += severity;
            text += "] ";
            text += entry.message;
            text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    std::string infoLabel = "Info (" + std::to_string(infoCount) + ")";
    std::string warningLabel = "Warnings (" + std::to_string(warningCount) + ")";
    std::string errorLabel = "Errors (" + std::to_string(errorCount) + ")";
    ImGui::Checkbox(infoLabel.c_str(), &showConsoleInfo_);
    ImGui::SameLine();
    ImGui::Checkbox(warningLabel.c_str(), &showConsoleWarnings_);
    ImGui::SameLine();
    ImGui::Checkbox(errorLabel.c_str(), &showConsoleErrors_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ConsoleSearch", "Search messages...", consoleSearch_.data(),
                             consoleSearch_.size());
    ImGui::Separator();

    if (ImGui::BeginChild("ConsoleMessages", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const std::string query(consoleSearch_.data());
        for (size_t index = 0; index < consoleEntries_.size(); ++index) {
            const ConsoleEntry& entry = consoleEntries_[index];
            const bool severityVisible =
                (entry.severity == ConsoleSeverity::Info && showConsoleInfo_) ||
                (entry.severity == ConsoleSeverity::Warning && showConsoleWarnings_) ||
                (entry.severity == ConsoleSeverity::Error && showConsoleErrors_);
            if (!severityVisible || (!query.empty() &&
                                     !ContainsCaseInsensitive(entry.message, query))) {
                continue;
            }
            const char* label = entry.severity == ConsoleSeverity::Error
                                    ? "Error"
                                    : entry.severity == ConsoleSeverity::Warning ? "Warning"
                                                                                 : "Info";
            const ImVec4 color = entry.severity == ConsoleSeverity::Error
                                     ? ImVec4{1.0f, 0.35f, 0.35f, 1.0f}
                                     : entry.severity == ConsoleSeverity::Warning
                                           ? ImVec4{1.0f, 0.75f, 0.25f, 1.0f}
                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushID(static_cast<int>(index));
            ImGui::BeginGroup();
            ImGui::TextDisabled("[%7.2f]", entry.timestampSeconds);
            ImGui::SameLine();
            ImGui::TextColored(color, "[%s]", label);
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.message.c_str());
            ImGui::EndGroup();
            const bool hasSource = !entry.sourcePath.empty() && entry.sourceLine > 0u;
            if (hasSource && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (entry.sourceColumn > 0u) {
                    ImGui::SetTooltip("Double-click to open %s:%u:%u",
                                      entry.sourcePath.filename().string().c_str(),
                                      entry.sourceLine, entry.sourceColumn);
                } else {
                    ImGui::SetTooltip("Double-click to open %s:%u",
                                      entry.sourcePath.filename().string().c_str(),
                                      entry.sourceLine);
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    OpenConsoleSource(entry.sourcePath, entry.sourceLine);
                }
            }
            if (ImGui::BeginPopupContextItem("MessageContext")) {
                if (hasSource && ImGui::MenuItem("Open Source")) {
                    OpenConsoleSource(entry.sourcePath, entry.sourceLine);
                }
                if (hasSource && ImGui::MenuItem("Copy Source Location")) {
                    std::string location = entry.sourcePath.generic_string() + ":" +
                                           std::to_string(entry.sourceLine);
                    if (entry.sourceColumn > 0u) {
                        location += ":" + std::to_string(entry.sourceColumn);
                    }
                    ImGui::SetClipboardText(location.c_str());
                }
                if (ImGui::MenuItem("Copy Message")) {
                    ImGui::SetClipboardText(entry.message.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (consoleScrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            consoleScrollToBottom_ = false;
        }
    }
    ImGui::EndChild();
}

