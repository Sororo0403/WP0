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
Write-Output "Cppcheck findings: 0"
