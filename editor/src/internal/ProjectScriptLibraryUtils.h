#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

class BehaviorRegistry;
class Input;
struct ScriptTypeRegistration;

namespace ProjectScriptLibraryUtils {
bool PrepareAndLoadModule(const std::filesystem::path& sourcePath,
                          const std::filesystem::path& loadedPath, void*& module,
                          std::string& error);
bool ResolveScriptRegistrations(void* module, const ScriptTypeRegistration*& registrations,
                                size_t& count, std::string& error);
bool ValidateScriptRegistrations(const ScriptTypeRegistration* registrations, size_t count,
                                 const BehaviorRegistry& registry, std::string& error);
bool RegisterScriptTypes(const ScriptTypeRegistration* registrations, size_t count, Input* input,
                         BehaviorRegistry& registry, std::string& error);
void ReleaseModule(void* module);
}
