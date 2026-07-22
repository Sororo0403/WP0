#include "ScriptBuildService.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
#ifdef _DEBUG
constexpr wchar_t kConfiguration[] = L"Debug";
#else
constexpr wchar_t kConfiguration[] = L"Release";
#endif

struct ScriptSource {
    std::filesystem::path path;
    std::string registrationFunction;
};

class ScopedBuildMutex {
public:
    ScopedBuildMutex() = default;

    ~ScopedBuildMutex() {
        if (handle_ != nullptr) {
            if (owns_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
    }

    ScopedBuildMutex(const ScopedBuildMutex&) = delete;
    ScopedBuildMutex& operator=(const ScopedBuildMutex&) = delete;

    bool Acquire(const std::filesystem::path& buildProject, std::string& error) {
        std::error_code filesystemError;
        std::wstring key =
            std::filesystem::absolute(buildProject, filesystemError).generic_wstring();
        if (filesystemError) {
            error = "Could not resolve the Project Script build path.";
            return false;
        }
        std::ranges::transform(key, key.begin(), ::towlower);
        uint64_t hash = 14695981039346656037ull;
        for (const wchar_t character : key) {
            hash ^= static_cast<uint64_t>(character);
            hash *= 1099511628211ull;
        }
        const std::wstring name =
            L"Local\\LikeEngine.ProjectScriptBuild." + std::to_wstring(hash);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_ == nullptr) {
            error = "Could not create the Project Script build lock.";
            return false;
        }
        constexpr DWORD waitMilliseconds = 120000u;
        const DWORD waitResult = WaitForSingleObject(handle_, waitMilliseconds);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            error = waitResult == WAIT_TIMEOUT ?
                        "Timed out waiting for another Project Script build." :
                        "Could not acquire the Project Script build lock.";
            return false;
        }
        owns_ = true;
        return true;
    }

private:
    HANDLE handle_ = nullptr;
    bool owns_ = false;
};

std::filesystem::path FindEngineRoot(std::filesystem::path directory) {
    std::error_code error;
    directory = std::filesystem::absolute(directory, error).lexically_normal();
    if (error) {
        return {};
    }
    for (size_t depth = 0u; depth < 16u && !directory.empty(); ++depth) {
        if (std::filesystem::is_regular_file(directory / L"engine" / L"Engine.vcxproj",
                                             error) &&
            !error) {
            return directory;
        }
        error.clear();
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return {};
}

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0u) {
            return {};
        }
        if (length < buffer.size() - 1u) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2u);
    }
}

bool EnsureLowercaseLibraryDirectory(const std::filesystem::path& projectRoot,
                                     std::string& error) {
    std::error_code filesystemError;
    for (std::filesystem::directory_iterator iterator(projectRoot, filesystemError), end;
         iterator != end && !filesystemError; iterator.increment(filesystemError)) {
        if (!iterator->is_directory(filesystemError) || filesystemError) {
            filesystemError.clear();
            continue;
        }
        std::wstring lowercase = iterator->path().filename().wstring();
        std::ranges::transform(lowercase, lowercase.begin(), ::towlower);
        if (lowercase != L"library" || iterator->path().filename() == L"library") {
            continue;
        }
        const std::filesystem::path temporary =
            projectRoot / L".likeengine_library_case_migration";
        const std::filesystem::path destination = projectRoot / L"library";
        if (std::filesystem::exists(temporary, filesystemError) || filesystemError) {
            error = "Could not migrate the Project library directory casing.";
            return false;
        }
        const std::filesystem::path source = iterator->path();
        std::filesystem::rename(source, temporary, filesystemError);
        if (filesystemError) {
            error = "Could not rename the Project Library directory to library.";
            return false;
        }
        std::filesystem::rename(temporary, destination, filesystemError);
        if (filesystemError) {
            std::error_code rollbackError;
            std::filesystem::rename(temporary, source, rollbackError);
            error = "Could not finish renaming the Project library directory.";
            return false;
        }
        return true;
    }
    if (filesystemError) {
        error = "Could not inspect the Project library directory.";
        return false;
    }
    return true;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool WriteIfChanged(const std::filesystem::path& path, const std::string& contents,
                    std::string& error) {
    if (ReadText(path) == contents) {
        return true;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) {
        error = "Could not create the Script build directory.";
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) {
        error = "Could not write generated Script build file: " +
                path.generic_string();
        return false;
    }
    return true;
}

std::string XmlEscape(std::string value) {
    const std::array replacements = {
        std::pair{"&", "&amp;"}, std::pair{"\"", "&quot;"},
        std::pair{"<", "&lt;"}, std::pair{">", "&gt;"}};
    for (const auto& [from, to] : replacements) {
        size_t position = 0u;
        while ((position = value.find(from, position)) != std::string::npos) {
            value.replace(position, std::char_traits<char>::length(from), to);
            position += std::char_traits<char>::length(to);
        }
    }
    return value;
}

std::vector<ScriptSource> DiscoverScripts(const std::filesystem::path& projectRoot,
                                          std::string& error) {
    const std::filesystem::path assetRoot = projectRoot / L"assets";
    std::vector<ScriptSource> scripts;
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(assetRoot, filesystemError) || filesystemError) {
        error = "Project assets directory was not found.";
        return {};
    }
    const std::regex registrationPattern(
        R"(ScriptTypeRegistration\s+(Get[A-Za-z_][A-Za-z0-9_]*ScriptRegistration)\s*\()",
        std::regex::ECMAScript);
    for (std::filesystem::recursive_directory_iterator iterator(assetRoot, filesystemError), end;
         iterator != end && !filesystemError; iterator.increment(filesystemError)) {
        std::wstring extension = iterator->path().extension().wstring();
        std::ranges::transform(extension, extension.begin(), ::towlower);
        if (!iterator->is_regular_file(filesystemError) || filesystemError ||
            extension != L".cpp") {
            filesystemError.clear();
            continue;
        }
        const std::string source = ReadText(iterator->path());
        std::sregex_iterator match(source.begin(), source.end(), registrationPattern);
        const std::sregex_iterator matchEnd;
        if (match == matchEnd) {
            error = "Script source does not define a registration function: " +
                    iterator->path().generic_string();
            return {};
        }
        const std::string function = (*match)[1].str();
        ++match;
        if (match != matchEnd) {
            error = "Script source defines more than one registration function: " +
                    iterator->path().generic_string();
            return {};
        }
        scripts.push_back({iterator->path().lexically_normal(), function});
    }
    if (filesystemError) {
        error = "Could not enumerate Project Script sources.";
        return {};
    }
    std::ranges::sort(scripts, {}, [](const ScriptSource& script) {
        return script.path.generic_string();
    });
    std::unordered_set<std::string> registrations;
    for (const ScriptSource& script : scripts) {
        if (!registrations.insert(script.registrationFunction).second) {
            error = "Project Scripts contain a duplicate registration function: " +
                    script.registrationFunction;
            return {};
        }
    }
    return scripts;
}

std::string GenerateRegistry(const std::vector<ScriptSource>& scripts) {
    std::ostringstream output;
    output << "// Generated by LikeEngine. Do not edit.\n"
              "#include \"runtime/ScriptModuleApi.h\"\n\n"
              "#include <array>\n\n";
    for (const ScriptSource& script : scripts) {
        output << "ScriptTypeRegistration " << script.registrationFunction << "();\n";
    }
    output << "\nextern \"C\" __declspec(dllexport) uint32_t "
              "LikeEngineScriptModuleApiVersion() {\n"
              "    return kScriptModuleApiVersion;\n"
              "}\n\n"
              "extern \"C\" __declspec(dllexport) const ScriptTypeRegistration*\n"
              "LikeEngineGetScriptTypes(size_t* count) {\n";
    if (scripts.empty()) {
        output << "    if (count != nullptr) { *count = 0u; }\n"
                  "    return nullptr;\n";
    } else {
        output << "    static const std::array registrations = {\n";
        for (const ScriptSource& script : scripts) {
            output << "        " << script.registrationFunction << "(),\n";
        }
        output << "    };\n"
                  "    if (count != nullptr) { *count = registrations.size(); }\n"
                  "    return registrations.data();\n";
    }
    output << "}\n";
    return output.str();
}

std::string GenerateProject(const std::filesystem::path& projectRoot,
                            const std::filesystem::path& engineRoot,
                            const std::vector<ScriptSource>& scripts) {
    const std::string project = XmlEscape(projectRoot.generic_string());
    const std::string engine = XmlEscape(engineRoot.generic_string());
    std::ostringstream sources;
    sources << "    <ClCompile Include=\"$(MSBuildThisFileDirectory)GeneratedScriptRegistry.cpp\" />\n";
    for (const ScriptSource& script : scripts) {
        sources << "    <ClCompile Include=\"" << XmlEscape(script.path.generic_string())
                << "\" />\n";
    }
    std::ostringstream output;
    output << R"(<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>18.0</VCProjectVersion><Keyword>Win32Proj</Keyword>
    <ProjectGuid>{5E4892FD-8050-46B8-AC75-0D29816A29D3}</ProjectGuid>
    <RootNamespace>ProjectScripts</RootNamespace><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>ProjectScripts</ProjectName>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType><UseDebugLibraries>true</UseDebugLibraries><PlatformToolset>v145</PlatformToolset><CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>DynamicLibrary</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries><PlatformToolset>v145</PlatformToolset><WholeProgramOptimization>true</WholeProgramOptimization><CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup>
    <OutDir>)" << project << R"(/library/ScriptAssemblies/$(Platform)/$(Configuration)/</OutDir>
    <IntDir>)" << project << R"(/library/ScriptBuild/obj/$(Platform)/$(Configuration)/</IntDir>
  </PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile><WarningLevel>Level4</WarningLevel><SDLCheck>true</SDLCheck><PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions><ConformanceMode>true</ConformanceMode><LanguageStandard>stdcpp20</LanguageStandard><TreatWarningAsError>true</TreatWarningAsError><RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary><DebugInformationFormat>OldStyle</DebugInformationFormat><AdditionalIncludeDirectories>)"
           << project << "/assets/Scripts;" << engine << "/engine/public;" << engine
           << R"(/engine/include;)" << engine << R"(/engine/externals;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><AdditionalOptions>/utf-8 /FS %(AdditionalOptions)</AdditionalOptions></ClCompile>
    <Link><AdditionalDependencies>dinput8.lib;dxguid.lib;xinput.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile><WarningLevel>Level4</WarningLevel><SDLCheck>true</SDLCheck><PreprocessorDefinitions>NDEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions><ConformanceMode>true</ConformanceMode><LanguageStandard>stdcpp20</LanguageStandard><TreatWarningAsError>true</TreatWarningAsError><RuntimeLibrary>MultiThreaded</RuntimeLibrary><AdditionalIncludeDirectories>)"
           << project << "/assets/Scripts;" << engine << "/engine/public;" << engine
           << R"(/engine/include;)" << engine << R"(/engine/externals;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><AdditionalOptions>/utf-8 /FS %(AdditionalOptions)</AdditionalOptions></ClCompile>
    <Link><AdditionalDependencies>dinput8.lib;dxguid.lib;xinput.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>
  </ItemDefinitionGroup>
  <ItemGroup>
)" << sources.str() << R"(  </ItemGroup>
  <ItemGroup>
    <ProjectReference Include=")" << engine << R"(/engine/Engine.vcxproj"><Project>{8D69C2B4-4F39-47F0-9A40-4CB78EC442A1}</Project><SetPlatform>Platform=$(Platform)</SetPlatform><Properties>Configuration=$(Configuration);Platform=$(Platform)</Properties></ProjectReference>
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
)";
    return output.str();
}

std::filesystem::path FindMsBuild() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD found = SearchPathW(nullptr, L"MSBuild.exe", nullptr,
                                    static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (found > 0u && found < buffer.size()) {
        return buffer.data();
    }
    wchar_t programFiles[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", programFiles,
                                                  static_cast<DWORD>(std::size(programFiles)));
    if (length == 0u || length >= std::size(programFiles)) {
        return {};
    }
    const std::filesystem::path visualStudio =
        std::filesystem::path(programFiles) / L"Microsoft Visual Studio";
    constexpr std::array versions = {L"18", L"2022"};
    constexpr std::array editions = {L"Community", L"Professional", L"Enterprise",
                                     L"BuildTools", L"Preview"};
    std::error_code error;
    for (const wchar_t* version : versions) {
        for (const wchar_t* edition : editions) {
            const std::filesystem::path candidate =
                visualStudio / version / edition / L"MSBuild" / L"Current" / L"Bin" /
                L"MSBuild.exe";
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
            error.clear();
        }
    }
    return {};
}

bool RunBuild(const std::filesystem::path& buildProject,
              const std::filesystem::path& logPath, std::string& error,
              std::string* output) {
    const std::filesystem::path msbuild = FindMsBuild();
    if (msbuild.empty()) {
        error = "MSBuild was not found. Install Visual Studio C++ build tools.";
        return false;
    }
    ScopedBuildMutex buildMutex;
    if (!buildMutex.Acquire(buildProject, error)) {
        return false;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        error = "Could not create the Project Script build log.";
        return false;
    }
    std::wstring command = L"\"" + msbuild.wstring() + L"\" \"" +
                           buildProject.wstring() +
                           L"\" /nologo /m:1 /nodeReuse:false /verbosity:minimal "
                           L"/p:Configuration=" +
                           kConfiguration +
                           L" /p:Platform=x64 /p:BuildProjectReferences=false";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr,
                                        buildProject.parent_path().c_str(), &startup, &process);
    CloseHandle(log);
    if (!started) {
        error = "Could not start MSBuild for Project Scripts.";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1u;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (output != nullptr) {
        *output = ReadText(logPath);
    }
    if (exitCode != 0u) {
        error = "Project Script compilation failed. See " + logPath.generic_string();
        return false;
    }
    return true;
}

bool GenerateBuildFiles(const std::filesystem::path& projectRoot,
                        std::vector<ScriptSource>& scripts, std::string& error) {
    if (!EnsureLowercaseLibraryDirectory(projectRoot, error)) {
        return false;
    }
    std::filesystem::path engineRoot = FindEngineRoot(projectRoot);
    if (engineRoot.empty()) {
        engineRoot = FindEngineRoot(ExecutableDirectory());
    }
    std::error_code filesystemError;
    if (engineRoot.empty()) {
        engineRoot = FindEngineRoot(std::filesystem::current_path(filesystemError));
    }
    if (engineRoot.empty()) {
        error = "LikeEngine source root was not found for Project Script compilation.";
        return false;
    }
    scripts = DiscoverScripts(projectRoot, error);
    if (!error.empty()) {
        return false;
    }
    const std::filesystem::path buildRoot = projectRoot / L"library" / L"ScriptBuild";
    return WriteIfChanged(buildRoot / L"GeneratedScriptRegistry.cpp",
                          GenerateRegistry(scripts), error) &&
           WriteIfChanged(buildRoot / L"ProjectScripts.vcxproj",
                          GenerateProject(projectRoot, engineRoot, scripts), error);
}

std::filesystem::path AssemblyPath(const std::filesystem::path& projectRoot) {
    return projectRoot / L"library" / L"ScriptAssemblies" / L"x64" /
           kConfiguration / L"ProjectScripts.dll";
}

bool IsScriptDependency(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), ::towlower);
    return extension == L".cpp" || extension == L".h" || extension == L".hpp" ||
           extension == L".hxx" || extension == L".inl";
}

bool IsNewerThan(const std::filesystem::path& path,
                 std::filesystem::file_time_type comparison) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error || time > comparison;
}
} // namespace

bool ScriptBuildService::BuildIfNeeded(const std::filesystem::path& projectRoot,
                                       std::string& error) {
    error.clear();
    std::vector<ScriptSource> scripts;
    if (!GenerateBuildFiles(projectRoot, scripts, error)) {
        return false;
    }
    const std::filesystem::path assembly = AssemblyPath(projectRoot);
    std::error_code filesystemError;
    const auto assemblyTime = std::filesystem::last_write_time(assembly, filesystemError);
    if (filesystemError) {
        return Build(projectRoot, error);
    }
    for (const ScriptSource& script : scripts) {
        if (IsNewerThan(script.path, assemblyTime)) {
            return Build(projectRoot, error);
        }
    }
    const std::filesystem::path buildRoot = projectRoot / L"library" / L"ScriptBuild";
    if (IsNewerThan(buildRoot / L"GeneratedScriptRegistry.cpp", assemblyTime) ||
        IsNewerThan(buildRoot / L"ProjectScripts.vcxproj", assemblyTime)) {
        return Build(projectRoot, error);
    }
    const std::filesystem::path assetRoot = projectRoot / L"assets";
    for (std::filesystem::recursive_directory_iterator iterator(assetRoot, filesystemError), end;
         iterator != end && !filesystemError; iterator.increment(filesystemError)) {
        if (iterator->is_regular_file(filesystemError) && !filesystemError &&
            IsScriptDependency(iterator->path()) &&
            IsNewerThan(iterator->path(), assemblyTime)) {
            return Build(projectRoot, error);
        }
        filesystemError.clear();
    }
    if (filesystemError) {
        return Build(projectRoot, error);
    }
    return true;
}

bool ScriptBuildService::Build(const std::filesystem::path& projectRoot,
                               std::string& error, std::string* output) {
    error.clear();
    if (output != nullptr) {
        output->clear();
    }
    std::vector<ScriptSource> scripts;
    if (!GenerateBuildFiles(projectRoot, scripts, error)) {
        return false;
    }
    const std::filesystem::path buildRoot = projectRoot / L"library" / L"ScriptBuild";
    return RunBuild(buildRoot / L"ProjectScripts.vcxproj",
                    buildRoot / L"ProjectScripts.log", error, output);
}

bool ScriptBuildService::GetSourceFingerprint(
    const std::filesystem::path& projectRoot, uint64_t& fingerprint,
    std::string& error) {
    error.clear();
    fingerprint = 14695981039346656037ull;
    const std::filesystem::path assetRoot = projectRoot / L"assets";
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(assetRoot, filesystemError) || filesystemError) {
        error = "Project assets directory was not found.";
        return false;
    }
    std::vector<std::filesystem::path> sources;
    for (std::filesystem::recursive_directory_iterator iterator(assetRoot, filesystemError), end;
         iterator != end && !filesystemError; iterator.increment(filesystemError)) {
        if (iterator->is_regular_file(filesystemError) && !filesystemError &&
            IsScriptDependency(iterator->path())) {
            sources.push_back(iterator->path().lexically_normal());
        }
        filesystemError.clear();
    }
    if (filesystemError) {
        error = "Could not enumerate Project Script sources.";
        return false;
    }
    std::ranges::sort(sources, {}, [](const std::filesystem::path& path) {
        return path.generic_string();
    });
    const auto hashBytes = [&fingerprint](std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            fingerprint ^= byte;
            fingerprint *= 1099511628211ull;
        }
    };
    for (const std::filesystem::path& source : sources) {
        const std::filesystem::path relative =
            std::filesystem::relative(source, assetRoot, filesystemError);
        if (filesystemError) {
            error = "Could not resolve a Project Script source path.";
            return false;
        }
        const std::string relativeText = relative.generic_string();
        hashBytes(relativeText);
        hashBytes(std::string_view("\0", 1u));
        std::ifstream stream(source, std::ios::binary);
        if (!stream) {
            error = "Could not read Project Script source: " + source.generic_string();
            return false;
        }
        std::array<char, 8192> buffer{};
        while (stream) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            hashBytes(std::string_view(buffer.data(),
                                       static_cast<size_t>(stream.gcount())));
        }
        if (!stream.eof()) {
            error = "Could not read Project Script source: " + source.generic_string();
            return false;
        }
        hashBytes(std::string_view("\xff", 1u));
    }
    return true;
}
