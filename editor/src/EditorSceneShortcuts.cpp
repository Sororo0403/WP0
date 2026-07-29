#include "EditorScene.h"

#include "imgui.h"

void EditorScene::HandleEditorShortcuts() {
    if (pendingSceneAction_ != PendingSceneAction::None) {
        return;
    }
    if (HandleGameInputReleaseShortcut() || HandleRuntimeShortcut()) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (ShouldIgnoreEditorCommandShortcuts(io)) {
        return;
    }
    if (io.KeyCtrl) {
        HandleEditorControlShortcut(io);
    } else {
        HandleEditorDirectShortcut(io);
    }
}

bool EditorScene::HandleGameInputReleaseShortcut() {
    if (!gameInputCaptured_ || !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        return false;
    }
    ReleaseGameInputCapture();
    status_ = "Released Game input.";
    return true;
}

bool EditorScene::HandleRuntimeShortcut() {
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        IsInPlayMode() ? StopPlayMode() : EnterPlayMode();
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
        if (IsInPlayMode()) {
            TogglePlayPause();
        }
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
        StepRuntimeWorld();
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        LaunchPlayerPreview();
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        BuildAndRunPlayerPackage();
        return true;
    }
    return false;
}

bool EditorScene::ShouldIgnoreEditorCommandShortcuts(const ImGuiIO& io) const {
    return io.WantTextInput || sceneCameraNavigating_ || sceneCameraPanning_ ||
           IsInPlayMode();
}

void EditorScene::HandleEditorDirectShortcut(const ImGuiIO& io) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ClearHierarchySelection();
    } else if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
        MoveEntityInHierarchy(selection_, -1);
    } else if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) {
        MoveEntityInHierarchy(selection_, 1);
    } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        RequestEntityRename(selection_);
    } else if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        gizmoOperation_ = GizmoOperation::Translate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        gizmoOperation_ = GizmoOperation::Rotate;
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        gizmoOperation_ = GizmoOperation::Scale;
    } else if (!gizmoWasUsing_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        DeleteSelection();
    }
}

void EditorScene::HandleEditorControlShortcut(const ImGuiIO& io) {
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        SelectAllHierarchyEntities();
    } else if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        RequestSceneAction(PendingSceneAction::NewScene);
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        RequestSceneAction(PendingSceneAction::OpenScene);
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        HandleSceneSaveShortcut(io.KeyShift);
    } else if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        CopySelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        CutSelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        PasteEntityClipboard();
    } else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        DuplicateSelection();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        HandleHistoryShortcut(io.KeyShift);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        Redo();
    }
}

void EditorScene::HandleSceneSaveShortcut(const bool saveAs) {
    saveAs ? SaveSceneAs() : SaveScene();
}

void EditorScene::HandleHistoryShortcut(const bool redo) {
    redo ? Redo() : Undo();
}
