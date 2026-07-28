#include "EditorScene.h"

#include "imgui.h"
#include "world/WorldSerializer.h"

void EditorScene::DrawEntityRenameDialog() {
    PrepareEntityRenamePopup();
    if (!ImGui::BeginPopupModal("Rename Entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    WorldEntity* entity = ResolveEntityRenameTarget();
    if (entity != nullptr) {
        bool renameRequested = false;
        bool cancelRequested = false;
        DrawEntityRenameInput(renameRequested, cancelRequested);
        if (renameRequested) {
            CommitEntityRename(*entity);
        } else if (cancelRequested) {
            CancelEntityRename();
        }
    }
    ImGui::EndPopup();
}

void EditorScene::PrepareEntityRenamePopup() {
    if (!showEntityRenameDialog_) {
        return;
    }
    ImGui::OpenPopup("Rename Entity");
    showEntityRenameDialog_ = false;
    focusEntityRenameInput_ = true;
}

WorldEntity* EditorScene::ResolveEntityRenameTarget() {
    if (IsInPlayMode()) {
        CancelEntityRename();
        return nullptr;
    }
    WorldEntity* entity = world_.Find(renameEntity_);
    if (entity == nullptr) {
        CancelEntityRename();
    }
    return entity;
}

void EditorScene::DrawEntityRenameInput(bool& renameRequested, bool& cancelRequested) {
    ImGui::TextDisabled("ID: %s", renameEntity_.ToString().c_str());
    if (focusEntityRenameInput_) {
        ImGui::SetKeyboardFocusHere();
        focusEntityRenameInput_ = false;
    }
    ImGui::SetNextItemWidth(320.0f);
    const bool submitted =
        ImGui::InputText("##EntityName", renameBuffer_.data(), renameBuffer_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    renameRequested = submitted || ImGui::Button("Rename", ImVec2(100.0f, 0.0f));
    if (!renameRequested) {
        ImGui::SameLine();
        cancelRequested = ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                          ImGui::Button("Cancel", ImVec2(100.0f, 0.0f));
    }
}

void EditorScene::CommitEntityRename(WorldEntity& entity) {
    const std::string before = WorldSerializer::Serialize(world_);
    const EntityId selectionBefore = selection_;
    entity.name = renameBuffer_.data();
    if (entity.name.empty()) {
        entity.name = "Entity";
    }
    selection_ = renameEntity_;
    renameEntity_ = {};
    RecordImmediateEdit("Rename Entity", before, selectionBefore);
    status_ = "Renamed the entity.";
    ImGui::CloseCurrentPopup();
}

void EditorScene::CancelEntityRename() {
    renameEntity_ = {};
    ImGui::CloseCurrentPopup();
}
