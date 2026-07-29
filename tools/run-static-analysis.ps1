param(
    [string]$CppcheckPath = "C:\Program Files\Cppcheck\cppcheck.exe"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$reportDirectory = Join-Path $repositoryRoot "reports\static-analysis"
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null

$sourcePaths = @(
    "engine/src",
    "engine/include",
    "engine/public",
    "editor/src",
    "editor/include",
    "tests",
    "projects/test/assets/Scripts"
)

# These limits preserve the current legacy complexity ceiling while making any
# increase fail the check. The full HTML report keeps every function visible.
$lizardComplexityLimit = 939
$lizardLengthLimit = 3396
$lizardTextReport = Join-Path $reportDirectory "lizard-final.txt"
$lizardHtmlReport = Join-Path $reportDirectory "lizard-final.html"

& lizard @sourcePaths -l cpp -t 1 -C $lizardComplexityLimit `
    -L $lizardLengthLimit -w -i 0 -o $lizardTextReport
if ($LASTEXITCODE -ne 0) {
    throw "Lizard reported one or more warnings."
}
& lizard @sourcePaths -l cpp -t 1 -C $lizardComplexityLimit `
    -L $lizardLengthLimit -H -o $lizardHtmlReport
if ($LASTEXITCODE -ne 0) {
    throw "Lizard HTML report generation failed."
}

$serializerSources = @(
    "engine/src/world/WorldSerializer.cpp",
    "engine/src/world/WorldSerializerDeserialize.cpp",
    "engine/src/world/WorldSerializerPhysicsDecoders.cpp",
    "engine/src/world/WorldSerializerRenderingDecoders.cpp",
    "engine/src/world/WorldSerializerRuntimeDecoders.cpp",
    "engine/src/world/WorldSerializerScriptDecoders.cpp",
    "engine/src/world/WorldSerializerSerialize.cpp",
    "engine/src/world/WorldSerializerUiDecoders.cpp"
)
$serializerLizardReport =
    Join-Path $reportDirectory "lizard-world-serializer-final.txt"
& lizard @serializerSources -l cpp -t 1 -C 74 -L 410 -w -i 0 `
    -o $serializerLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "WorldSerializer exceeded its refactored complexity ceiling."
}

$inputSettingsLizardReport =
    Join-Path $reportDirectory "lizard-input-settings-final.txt"
& lizard "editor/src/InputSettingsStore.cpp" -l cpp -t 1 -C 12 -L 55 -w -i 0 `
    -o $inputSettingsLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "InputSettingsStore exceeded its refactored complexity ceiling."
}

$projectScriptLibraryLizardReport =
    Join-Path $reportDirectory "lizard-project-script-library-final.txt"
& lizard "editor/src/ProjectScriptLibrary.cpp" -l cpp -t 1 -C 7 -L 34 -w -i 0 `
    -o $projectScriptLibraryLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "ProjectScriptLibrary exceeded its refactored complexity ceiling."
}

$projectScriptValidationLizardReport =
    Join-Path $reportDirectory "lizard-project-script-library-validation-final.txt"
& lizard "editor/src/ProjectScriptLibraryValidation.cpp" -l cpp -t 1 -C 35 -L 44 -w -i 0 `
    -o $projectScriptValidationLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "ProjectScriptLibrary validation exceeded its refactored complexity ceiling."
}

$inspectorSources = @(
    "editor/src/EditorSceneInspector.cpp",
    "editor/src/EditorSceneInspectorAnimator.cpp",
    "editor/src/EditorSceneInspectorImage.cpp",
    "editor/src/EditorSceneInspectorMaterial.cpp",
    "editor/src/EditorSceneInspectorMedia.cpp",
    "editor/src/EditorSceneInspectorPhysics.cpp",
    "editor/src/EditorSceneInspectorRendering.cpp",
    "editor/src/EditorSceneInspectorScriptAssets.cpp",
    "editor/src/EditorSceneInspectorUi.cpp",
    "editor/src/EditorSceneInspectorUiControls.cpp",
    "editor/src/EditorSceneInspectorUiGraphics.cpp"
)
$inspectorLizardReport =
    Join-Path $reportDirectory "lizard-editor-inspector-final.txt"
& lizard @inspectorSources -l cpp -t 1 -C 66 -L 272 -w -i 0 `
    -o $inspectorLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Inspector exceeded its refactored complexity ceiling."
}

$materialInspectorLizardReport =
    Join-Path $reportDirectory "lizard-editor-inspector-material-final.txt"
& lizard "editor/src/EditorSceneInspectorMaterial.cpp" -l cpp -t 1 -C 11 -L 46 -w -i 0 `
    -o $materialInspectorLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Material Inspector exceeded its refactored complexity ceiling."
}

$animatorInspectorLizardReport =
    Join-Path $reportDirectory "lizard-editor-inspector-animator-final.txt"
& lizard "editor/src/EditorSceneInspectorAnimator.cpp" -l cpp -t 1 -C 26 -L 52 -w -i 0 `
    -o $animatorInspectorLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Animator Inspector exceeded its refactored complexity ceiling."
}

$imageInspectorLizardReport =
    Join-Path $reportDirectory "lizard-editor-inspector-image-final.txt"
& lizard "editor/src/EditorSceneInspectorImage.cpp" -l cpp -t 1 -C 12 -L 42 -w -i 0 `
    -o $imageInspectorLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Image Inspector exceeded its refactored complexity ceiling."
}

$scriptAssetInspectorLizardReport =
    Join-Path $reportDirectory "lizard-editor-inspector-script-assets-final.txt"
& lizard "editor/src/EditorSceneInspectorScriptAssets.cpp" -l cpp -t 1 -C 16 -L 41 -w -i 0 `
    -o $scriptAssetInspectorLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Script Asset Inspector exceeded its refactored complexity ceiling."
}

$gameUiLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-final.txt"
& lizard "editor/src/EditorSceneGameUi.cpp" -l cpp -t 1 -C 43 -L 116 -w -i 0 `
    -o $gameUiLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Game UI exceeded its refactored complexity ceiling."
}

$gameUiVisualsLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-visuals-final.txt"
& lizard "editor/src/EditorSceneGameUiVisuals.cpp" -l cpp -t 1 -C 20 -L 61 -w -i 0 `
    -o $gameUiVisualsLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Game UI visuals exceeded their refactored complexity ceiling."
}

$gameUiInteractionLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-interaction-final.txt"
& lizard "editor/src/EditorSceneGameUiInteraction.cpp" -l cpp -t 1 -C 32 -L 70 -w -i 0 `
    -o $gameUiInteractionLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Game UI interaction exceeded its refactored complexity ceiling."
}

$gameUiEditingLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-editing-final.txt"
& lizard "editor/src/EditorSceneGameUiEditing.cpp" -l cpp -t 1 -C 25 -L 90 -w -i 0 `
    -o $gameUiEditingLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Game UI editing exceeded its refactored complexity ceiling."
}

$gizmoLizardReport =
    Join-Path $reportDirectory "lizard-editor-gizmos-final.txt"
& lizard "editor/src/EditorSceneGizmos.cpp" -l cpp -t 1 -C 48 -L 148 -w -i 0 `
    -o $gizmoLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Gizmos exceeded their refactored complexity ceiling."
}

$hierarchyLizardReport =
    Join-Path $reportDirectory "lizard-editor-hierarchy-final.txt"
& lizard "editor/src/EditorSceneHierarchy.cpp" -l cpp -t 1 -C 39 -L 187 -w -i 0 `
    -o $hierarchyLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Hierarchy exceeded its refactored complexity ceiling."
}

$hierarchyNodeLizardReport =
    Join-Path $reportDirectory "lizard-editor-hierarchy-node-final.txt"
& lizard "editor/src/EditorSceneHierarchyNode.cpp" -l cpp -t 1 -C 27 -L 71 -w -i 0 `
    -o $hierarchyNodeLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Hierarchy node exceeded its refactored complexity ceiling."
}

$projectSettingsLizardReport =
    Join-Path $reportDirectory "lizard-editor-project-settings-final.txt"
& lizard "editor/src/EditorSceneProjectSettings.cpp" -l cpp -t 1 -C 41 -L 173 -w -i 0 `
    -o $projectSettingsLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Project Settings exceeded their refactored complexity ceiling."
}

$panelsLizardReport =
    Join-Path $reportDirectory "lizard-editor-panels-final.txt"
& lizard "editor/src/EditorScenePanels.cpp" -l cpp -t 1 -C 32 -L 131 -w -i 0 `
    -o $panelsLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Panels exceeded their refactored complexity ceiling."
}

$mainMenuLizardReport =
    Join-Path $reportDirectory "lizard-editor-main-menu-final.txt"
& lizard "editor/src/EditorSceneMainMenu.cpp" -l cpp -t 1 -C 15 -L 46 -w -i 0 `
    -o $mainMenuLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene main menu exceeded its refactored complexity ceiling."
}

$updateLizardReport =
    Join-Path $reportDirectory "lizard-editor-update-final.txt"
& lizard "editor/src/EditorSceneUpdate.cpp" -l cpp -t 1 -C 17 -L 27 -w -i 0 `
    -o $updateLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Update exceeded its refactored complexity ceiling."
}

$renderingLizardReport =
    Join-Path $reportDirectory "lizard-editor-rendering-final.txt"
& lizard "editor/src/EditorSceneRendering.cpp" -l cpp -t 1 -C 21 -L 45 -w -i 0 `
    -o $renderingLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Rendering exceeded its refactored complexity ceiling."
}

$assetDiscoveryLizardReport =
    Join-Path $reportDirectory "lizard-editor-asset-discovery-final.txt"
& lizard "editor/src/EditorSceneAssetDiscovery.cpp" -l cpp -t 1 -C 18 -L 33 -w -i 0 `
    -o $assetDiscoveryLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene asset discovery exceeded its refactored complexity ceiling."
}

$assetBrowserEntryLizardReport =
    Join-Path $reportDirectory "lizard-editor-asset-browser-entry-final.txt"
& lizard "editor/src/EditorSceneAssetBrowserEntry.cpp" -l cpp -t 1 -C 24 -L 63 -w -i 0 `
    -o $assetBrowserEntryLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene asset browser entry exceeded its refactored complexity ceiling."
}

$viewportNavigationLizardReport =
    Join-Path $reportDirectory "lizard-editor-viewport-navigation-final.txt"
& lizard "editor/src/EditorSceneViewportNavigation.cpp" -l cpp -t 1 -C 11 -L 50 -w -i 0 `
    -o $viewportNavigationLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene viewport navigation exceeded its refactored complexity ceiling."
}

$viewportSelectionLizardReport =
    Join-Path $reportDirectory "lizard-editor-viewport-selection-final.txt"
& lizard "editor/src/EditorSceneViewportSelection.cpp" -l cpp -t 1 -C 13 -L 42 -w -i 0 `
    -o $viewportSelectionLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene viewport selection exceeded its refactored complexity ceiling."
}

$initializationLizardReport =
    Join-Path $reportDirectory "lizard-editor-initialization-final.txt"
& lizard "editor/src/EditorSceneInitialization.cpp" -l cpp -t 1 -C 7 -L 22 -w -i 0 `
    -o $initializationLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene initialization exceeded its refactored complexity ceiling."
}

$prefabSavingLizardReport =
    Join-Path $reportDirectory "lizard-editor-prefab-saving-final.txt"
& lizard "editor/src/EditorScenePrefabSaving.cpp" -l cpp -t 1 -C 6 -L 26 -w -i 0 `
    -o $prefabSavingLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Prefab saving exceeded its refactored complexity ceiling."
}

$playerBuildLizardReport =
    Join-Path $reportDirectory "lizard-editor-player-build-final.txt"
& lizard "editor/src/EditorScenePlayerBuild.cpp" -l cpp -t 1 -C 8 -L 24 -w -i 0 `
    -o $playerBuildLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Player build exceeded its refactored complexity ceiling."
}

$assetPreviewLizardReport =
    Join-Path $reportDirectory "lizard-editor-asset-preview-final.txt"
& lizard "editor/src/EditorSceneAssetPreview.cpp" -l cpp -t 1 -C 6 -L 26 -w -i 0 `
    -o $assetPreviewLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Asset Preview exceeded its refactored complexity ceiling."
}

$lightingLizardReport =
    Join-Path $reportDirectory "lizard-editor-lighting-final.txt"
& lizard "editor/src/EditorSceneLighting.cpp" -l cpp -t 1 -C 7 -L 26 -w -i 0 `
    -o $lightingLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Lighting exceeded its refactored complexity ceiling."
}

$playerPreviewLizardReport =
    Join-Path $reportDirectory "lizard-editor-player-preview-final.txt"
& lizard "editor/src/EditorScenePlayerPreview.cpp" -l cpp -t 1 -C 8 -L 19 -w -i 0 `
    -o $playerPreviewLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Player Preview exceeded its refactored complexity ceiling."
}

$audioPreviewLizardReport =
    Join-Path $reportDirectory "lizard-editor-audio-preview-final.txt"
& lizard "editor/src/EditorSceneAudioPreview.cpp" -l cpp -t 1 -C 5 -L 18 -w -i 0 `
    -o $audioPreviewLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Audio Preview exceeded its refactored complexity ceiling."
}

$prefabInstantiationLizardReport =
    Join-Path $reportDirectory "lizard-editor-prefab-instantiation-final.txt"
& lizard "editor/src/EditorScenePrefabInstantiation.cpp" -l cpp -t 1 -C 6 -L 23 -w -i 0 `
    -o $prefabInstantiationLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Prefab instantiation exceeded its refactored complexity ceiling."
}

$entityRenameLizardReport =
    Join-Path $reportDirectory "lizard-editor-entity-rename-final.txt"
& lizard "editor/src/EditorSceneEntityRename.cpp" -l cpp -t 1 -C 5 -L 18 -w -i 0 `
    -o $entityRenameLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Entity Rename exceeded its refactored complexity ceiling."
}

$cameraPreviewLizardReport =
    Join-Path $reportDirectory "lizard-editor-camera-preview-final.txt"
& lizard "editor/src/EditorSceneCameraPreview.cpp" -l cpp -t 1 -C 9 -L 18 -w -i 0 `
    -o $cameraPreviewLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Camera Preview exceeded its refactored complexity ceiling."
}

if (-not (Test-Path -LiteralPath $CppcheckPath)) {
    throw "Cppcheck was not found at '$CppcheckPath'."
}

$cppcheckReport = Join-Path $reportDirectory "cppcheck-final.txt"
$cppcheckSuppressions = Join-Path $repositoryRoot "cppcheck-suppressions.txt"
$cppcheckArguments = @(
    "engine/src",
    "editor/src",
    "tests",
    "projects/test/assets/Scripts",
    "-I", "engine/include",
    "-I", "engine/public",
    "-I", "engine/src",
    "-I", "editor/include",
    "-D_WIN32",
    "-D_DEBUG",
    "-D_LIB",
    "-DENGINE_WITH_IMGUI",
    "--enable=warning,style,performance,portability",
    "--inconclusive",
    "--check-level=normal",
    "--inline-suppr",
    "--std=c++20",
    "--platform=win64",
    "--error-exitcode=1",
    "--template=vs",
    "--suppressions-list=$cppcheckSuppressions",
    "--output-file=$cppcheckReport",
    "-i", "engine/externals",
    "-i", "editor/externals",
    "-i", "projects/test/build",
    "-j", "1",
    "--quiet"
)

Push-Location $repositoryRoot
try {
    & $CppcheckPath @cppcheckArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Cppcheck reported one or more findings."
    }
} finally {
    Pop-Location
}

Write-Output "Lizard warnings: 0"
Write-Output "WorldSerializer Lizard regressions: 0"
Write-Output "InputSettingsStore Lizard regressions: 0"
Write-Output "ProjectScriptLibrary Lizard regressions: 0"
Write-Output "ProjectScriptLibrary validation Lizard regressions: 0"
Write-Output "EditorScene Inspector Lizard regressions: 0"
Write-Output "EditorScene Material Inspector Lizard regressions: 0"
Write-Output "EditorScene Animator Inspector Lizard regressions: 0"
Write-Output "EditorScene Image Inspector Lizard regressions: 0"
Write-Output "EditorScene Script Asset Inspector Lizard regressions: 0"
Write-Output "EditorScene Game UI Lizard regressions: 0"
Write-Output "EditorScene Game UI visuals Lizard regressions: 0"
Write-Output "EditorScene Game UI interaction Lizard regressions: 0"
Write-Output "EditorScene Game UI editing Lizard regressions: 0"
Write-Output "EditorScene Gizmo Lizard regressions: 0"
Write-Output "EditorScene Hierarchy Lizard regressions: 0"
Write-Output "EditorScene Hierarchy node Lizard regressions: 0"
Write-Output "EditorScene Project Settings Lizard regressions: 0"
Write-Output "EditorScene Panels Lizard regressions: 0"
Write-Output "EditorScene main menu Lizard regressions: 0"
Write-Output "EditorScene Update Lizard regressions: 0"
Write-Output "EditorScene Rendering Lizard regressions: 0"
Write-Output "EditorScene asset discovery Lizard regressions: 0"
Write-Output "EditorScene asset browser entry Lizard regressions: 0"
Write-Output "EditorScene viewport navigation Lizard regressions: 0"
Write-Output "EditorScene viewport selection Lizard regressions: 0"
Write-Output "EditorScene initialization Lizard regressions: 0"
Write-Output "EditorScene Prefab saving Lizard regressions: 0"
Write-Output "EditorScene Player build Lizard regressions: 0"
Write-Output "EditorScene Asset Preview Lizard regressions: 0"
Write-Output "EditorScene Lighting Lizard regressions: 0"
Write-Output "EditorScene Player Preview Lizard regressions: 0"
Write-Output "EditorScene Audio Preview Lizard regressions: 0"
Write-Output "EditorScene Prefab instantiation Lizard regressions: 0"
Write-Output "EditorScene Entity Rename Lizard regressions: 0"
Write-Output "EditorScene Camera Preview Lizard regressions: 0"
Write-Output "Cppcheck findings: 0"
