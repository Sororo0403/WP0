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

$inspectorSources = @(
    "editor/src/EditorSceneInspector.cpp",
    "editor/src/EditorSceneInspectorMedia.cpp",
    "editor/src/EditorSceneInspectorPhysics.cpp",
    "editor/src/EditorSceneInspectorRendering.cpp",
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

$gameUiLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-final.txt"
& lizard "editor/src/EditorSceneGameUi.cpp" -l cpp -t 1 -C 249 -L 538 -w -i 0 `
    -o $gameUiLizardReport
if ($LASTEXITCODE -ne 0) {
    throw "EditorScene Game UI exceeded its refactored complexity ceiling."
}

$gameUiEditingLizardReport =
    Join-Path $reportDirectory "lizard-editor-game-ui-editing-final.txt"
& lizard "editor/src/EditorSceneGameUiEditing.cpp" -l cpp -t 1 -C 53 -L 152 -w -i 0 `
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
Write-Output "EditorScene Inspector Lizard regressions: 0"
Write-Output "EditorScene Game UI Lizard regressions: 0"
Write-Output "EditorScene Game UI editing Lizard regressions: 0"
Write-Output "EditorScene Gizmo Lizard regressions: 0"
Write-Output "EditorScene Project Settings Lizard regressions: 0"
Write-Output "EditorScene Panels Lizard regressions: 0"
Write-Output "EditorScene main menu Lizard regressions: 0"
Write-Output "EditorScene Update Lizard regressions: 0"
Write-Output "EditorScene Rendering Lizard regressions: 0"
Write-Output "EditorScene asset discovery Lizard regressions: 0"
Write-Output "EditorScene viewport navigation Lizard regressions: 0"
Write-Output "EditorScene viewport selection Lizard regressions: 0"
Write-Output "EditorScene initialization Lizard regressions: 0"
Write-Output "Cppcheck findings: 0"
