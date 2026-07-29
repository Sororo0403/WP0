#include "internal/ProjectScriptLibraryUtils.h"

#include "input/Input.h"
#include "runtime/BehaviorRegistry.h"
#include "runtime/ScriptModuleApi.h"

#include <Windows.h>

#include <cmath>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ProjectScriptLibraryUtils {
namespace {
constexpr size_t kMaximumScriptTypes = 1024u;

bool IsInvalidScriptProperty(const ScriptTypeRegistration& registration, size_t propertyIndex) {
    const ScriptPropertyDescriptor& property = registration.properties[propertyIndex];
    if (property.name == nullptr) {
        return true;
    }
    const std::string_view propertyName(property.name);
    const bool invalidFloat =
        property.type == ScriptPropertyType::Float &&
        (!std::isfinite(property.defaultFloat) || !std::isfinite(property.minimumFloat) ||
         !std::isfinite(property.maximumFloat) || property.minimumFloat > property.maximumFloat ||
         property.defaultFloat < property.minimumFloat ||
         property.defaultFloat > property.maximumFloat);
    const bool invalidInteger =
        property.type == ScriptPropertyType::Integer &&
        (property.minimumInteger > property.maximumInteger ||
         property.defaultInteger < property.minimumInteger ||
         property.defaultInteger > property.maximumInteger);
    const bool invalidVector3 =
        property.type == ScriptPropertyType::Vector3 &&
        (!std::isfinite(property.defaultVector3.x) || !std::isfinite(property.defaultVector3.y) ||
         !std::isfinite(property.defaultVector3.z));
    const bool invalidString =
        (property.type == ScriptPropertyType::String ||
         property.type == ScriptPropertyType::AnimationClip ||
         property.type == ScriptPropertyType::Scene) &&
        property.defaultString != nullptr &&
        std::char_traits<char>::length(property.defaultString) > 1024u;
    const bool invalidInputAction =
        property.type == ScriptPropertyType::InputAction && property.defaultString != nullptr &&
        std::char_traits<char>::length(property.defaultString) > 64u;
    const bool invalidInputActionKind =
        property.inputActionKind < ScriptInputActionKind::Any ||
        property.inputActionKind > ScriptInputActionKind::Axis ||
        (property.type != ScriptPropertyType::InputAction &&
         property.inputActionKind != ScriptInputActionKind::Any);
    bool duplicate = false;
    for (size_t previous = 0u; previous < propertyIndex; ++previous) {
        duplicate = duplicate || propertyName == registration.properties[previous].name;
    }
    return propertyName.empty() || propertyName.size() > 128u || duplicate ||
           property.type < ScriptPropertyType::Float || property.type > ScriptPropertyType::Scene ||
           invalidFloat || invalidInteger || invalidVector3 || invalidString ||
           invalidInputAction || invalidInputActionKind;
}

bool IsInvalidScriptType(const ScriptTypeRegistration& registration,
                         const BehaviorRegistry& registry) {
    const std::string_view type(registration.type);
    const std::string_view sourceAsset(registration.sourceAsset);
    return type.empty() || type.size() > 128u || sourceAsset.empty() ||
           sourceAsset.size() > 1024u || !sourceAsset.starts_with("asset://") ||
           registry.Requirements(type) != nullptr ||
           !registry.TypeFromSourceAsset(sourceAsset).empty();
}

std::vector<ScriptPropertyDefinition> BuildScriptProperties(
    const ScriptTypeRegistration& registration, const Input* input) {
    std::vector<ScriptPropertyDefinition> properties;
    properties.reserve(registration.propertyCount);
    for (size_t propertyIndex = 0u; propertyIndex < registration.propertyCount; ++propertyIndex) {
        const ScriptPropertyDescriptor& property = registration.properties[propertyIndex];
        std::string defaultString = property.defaultString != nullptr ? property.defaultString : "";
        if (property.type == ScriptPropertyType::InputAction && input != nullptr) {
            const std::string actionId = input->GetActionId(defaultString);
            if (!actionId.empty()) {
                defaultString = actionId;
            }
        }
        properties.push_back(
            {property.name, property.type, property.defaultFloat, property.minimumFloat,
             property.maximumFloat, property.defaultBoolean, property.defaultInteger,
             property.minimumInteger, property.maximumInteger, property.defaultVector3,
             std::move(defaultString), property.inputActionKind});
    }
    return properties;
}
}

bool PrepareAndLoadModule(const std::filesystem::path& sourcePath,
                          const std::filesystem::path& loadedPath, void*& module,
                          std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(loadedPath.parent_path(), filesystemError);
    if (filesystemError ||
        !std::filesystem::copy_file(sourcePath, loadedPath,
                                    std::filesystem::copy_options::overwrite_existing,
                                    filesystemError)) {
        error = "Project Script module could not be prepared for loading: " +
                sourcePath.generic_string();
        return false;
    }
    module = LoadLibraryW(loadedPath.c_str());
    if (module != nullptr) {
        return true;
    }
    std::filesystem::remove(loadedPath, filesystemError);
    error = "Project Script module could not be loaded: " + sourcePath.generic_string();
    return false;
}

bool ResolveScriptRegistrations(void* module, const ScriptTypeRegistration*& registrations,
                                size_t& count, std::string& error) {
    const HMODULE nativeModule = static_cast<HMODULE>(module);
    const auto getVersion = reinterpret_cast<GetScriptModuleApiVersion>(
        GetProcAddress(nativeModule, kScriptModuleVersionExport));
    const auto getTypes = reinterpret_cast<GetScriptTypeRegistrations>(
        GetProcAddress(nativeModule, kScriptModuleTypesExport));
    if (getVersion == nullptr || getTypes == nullptr ||
        getVersion() != kScriptModuleApiVersion) {
        error = "Project Script module uses an unsupported API version.";
        return false;
    }
    registrations = getTypes(&count);
    if ((count > 0u && registrations == nullptr) || count > kMaximumScriptTypes) {
        error = "Project Script module returned an invalid registration table.";
        return false;
    }
    return true;
}

bool ValidateScriptRegistrations(const ScriptTypeRegistration* registrations, size_t count,
                                 const BehaviorRegistry& registry, std::string& error) {
    std::vector<std::string_view> types;
    types.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const ScriptTypeRegistration& registration = registrations[index];
        if (registration.type == nullptr || registration.sourceAsset == nullptr ||
            registration.create == nullptr || registration.propertyCount > 128u ||
            (registration.propertyCount > 0u && registration.properties == nullptr)) {
            error = "Project Script module contains an invalid Script type.";
            return false;
        }
        if (IsInvalidScriptType(registration, registry)) {
            error = "Project Script module contains an empty or duplicate Script type.";
            return false;
        }
        const std::string_view type(registration.type);
        for (const std::string_view existing : types) {
            if (existing == type) {
                error = "Project Script module contains a duplicate Script type.";
                return false;
            }
        }
        for (size_t propertyIndex = 0u; propertyIndex < registration.propertyCount;
             ++propertyIndex) {
            if (IsInvalidScriptProperty(registration, propertyIndex)) {
                error = "Project Script module contains an invalid property.";
                return false;
            }
        }
        types.push_back(type);
    }
    return true;
}

bool RegisterScriptTypes(const ScriptTypeRegistration* registrations, size_t count, Input* input,
                         BehaviorRegistry& registry, std::string& error) {
    for (size_t index = 0; index < count; ++index) {
        const ScriptTypeRegistration registration = registrations[index];
        const std::string type(registration.type);
        if (!registry.Register(
                type,
                [factory = registration.create, input] {
                    return std::unique_ptr<Behavior>(factory(input));
                },
                registration.requirements, registration.sourceAsset,
                BuildScriptProperties(registration, input))) {
            error = "Project Script type could not be registered: " + type;
            return false;
        }
    }
    return true;
}

void ReleaseModule(void* module) {
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
}
}
