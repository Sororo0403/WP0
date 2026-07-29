#include "EditorScene.h"

#include "imgui.h"
#include "internal/EditorSceneAssetUtils.h"

#include <algorithm>
#include <string>

using namespace EditorSceneAssetUtils;

void EditorScene::DrawConsolePanel() {
    DrawConsoleToolbar();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ConsoleSearch", "Search messages...", consoleSearch_.data(),
                             consoleSearch_.size());
    ImGui::Separator();
    DrawConsoleMessages();
}

void EditorScene::DrawConsoleToolbar() {
    if (ImGui::Button("Clear")) {
        ClearConsoleEntries();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        CopyConsoleEntriesToClipboard();
    }
    ImGui::SameLine();
    DrawConsoleSeverityFilters();
}

void EditorScene::ClearConsoleEntries() {
    consoleEntries_.clear();
    lastCapturedStatus_ = status_;
}

void EditorScene::CopyConsoleEntriesToClipboard() const {
    std::string text;
    for (const ConsoleEntry& entry : consoleEntries_) {
        text += '[';
        text += ConsoleSeverityLabel(entry.severity);
        text += "] ";
        text += entry.message;
        text += '\n';
    }
    ImGui::SetClipboardText(text.c_str());
}

void EditorScene::DrawConsoleSeverityFilters() {
    const std::string infoLabel =
        "Info (" + std::to_string(CountConsoleEntries(ConsoleSeverity::Info)) + ")";
    const std::string warningLabel =
        "Warnings (" + std::to_string(CountConsoleEntries(ConsoleSeverity::Warning)) + ")";
    const std::string errorLabel =
        "Errors (" + std::to_string(CountConsoleEntries(ConsoleSeverity::Error)) + ")";
    ImGui::Checkbox(infoLabel.c_str(), &showConsoleInfo_);
    ImGui::SameLine();
    ImGui::Checkbox(warningLabel.c_str(), &showConsoleWarnings_);
    ImGui::SameLine();
    ImGui::Checkbox(errorLabel.c_str(), &showConsoleErrors_);
}

size_t EditorScene::CountConsoleEntries(const ConsoleSeverity severity) const {
    return static_cast<size_t>(
        std::ranges::count(consoleEntries_, severity, &ConsoleEntry::severity));
}

const char* EditorScene::ConsoleSeverityLabel(const ConsoleSeverity severity) {
    switch (severity) {
    case ConsoleSeverity::Info:
        return "Info";
    case ConsoleSeverity::Warning:
        return "Warning";
    case ConsoleSeverity::Error:
        return "Error";
    }
    return "Info";
}

void EditorScene::DrawConsoleMessages() {
    if (ImGui::BeginChild("ConsoleMessages", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const std::string query(consoleSearch_.data());
        for (size_t index = 0; index < consoleEntries_.size(); ++index) {
            const ConsoleEntry& entry = consoleEntries_[index];
            if (IsConsoleEntryVisible(entry, query)) {
                DrawConsoleEntry(entry, index);
            }
        }
        if (consoleScrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            consoleScrollToBottom_ = false;
        }
    }
    ImGui::EndChild();
}

bool EditorScene::IsConsoleEntryVisible(const ConsoleEntry& entry,
                                        const std::string& query) const {
    return IsConsoleSeverityVisible(entry.severity) &&
           (query.empty() || ContainsCaseInsensitive(entry.message, query));
}

bool EditorScene::IsConsoleSeverityVisible(const ConsoleSeverity severity) const {
    switch (severity) {
    case ConsoleSeverity::Info:
        return showConsoleInfo_;
    case ConsoleSeverity::Warning:
        return showConsoleWarnings_;
    case ConsoleSeverity::Error:
        return showConsoleErrors_;
    }
    return false;
}

void EditorScene::DrawConsoleEntry(const ConsoleEntry& entry, const size_t index) {
    const ImVec4 color = entry.severity == ConsoleSeverity::Error
                             ? ImVec4{1.0f, 0.35f, 0.35f, 1.0f}
                         : entry.severity == ConsoleSeverity::Warning
                             ? ImVec4{1.0f, 0.75f, 0.25f, 1.0f}
                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginGroup();
    ImGui::TextDisabled("[%7.2f]", entry.timestampSeconds);
    ImGui::SameLine();
    ImGui::TextColored(color, "[%s]", ConsoleSeverityLabel(entry.severity));
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.message.c_str());
    ImGui::EndGroup();
    DrawConsoleSourceInteraction(entry);
    DrawConsoleEntryContextMenu(entry);
    ImGui::PopID();
}

void EditorScene::DrawConsoleSourceInteraction(const ConsoleEntry& entry) {
    const bool hasSource = !entry.sourcePath.empty() && entry.sourceLine > 0u;
    if (!hasSource || !ImGui::IsItemHovered()) {
        return;
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const std::string filename = entry.sourcePath.filename().string();
    if (entry.sourceColumn > 0u) {
        ImGui::SetTooltip("Double-click to open %s:%u:%u", filename.c_str(),
                          entry.sourceLine, entry.sourceColumn);
    } else {
        ImGui::SetTooltip("Double-click to open %s:%u", filename.c_str(),
                          entry.sourceLine);
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        OpenConsoleSource(entry.sourcePath, entry.sourceLine);
    }
}

void EditorScene::DrawConsoleEntryContextMenu(const ConsoleEntry& entry) {
    if (!ImGui::BeginPopupContextItem("MessageContext")) {
        return;
    }
    const bool hasSource = !entry.sourcePath.empty() && entry.sourceLine > 0u;
    if (hasSource && ImGui::MenuItem("Open Source")) {
        OpenConsoleSource(entry.sourcePath, entry.sourceLine);
    }
    if (hasSource && ImGui::MenuItem("Copy Source Location")) {
        CopyConsoleSourceLocation(entry);
    }
    if (ImGui::MenuItem("Copy Message")) {
        ImGui::SetClipboardText(entry.message.c_str());
    }
    ImGui::EndPopup();
}

void EditorScene::CopyConsoleSourceLocation(const ConsoleEntry& entry) {
    std::string location =
        entry.sourcePath.generic_string() + ":" + std::to_string(entry.sourceLine);
    if (entry.sourceColumn > 0u) {
        location += ":" + std::to_string(entry.sourceColumn);
    }
    ImGui::SetClipboardText(location.c_str());
}
