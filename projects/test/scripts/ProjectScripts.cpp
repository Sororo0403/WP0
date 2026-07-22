#include "runtime/ScriptModuleApi.h"

#include <array>
#include <iterator>

ScriptTypeRegistration GetFirstPersonControllerScriptRegistration();
ScriptTypeRegistration GetRotatorScriptRegistration();

extern "C" __declspec(dllexport) uint32_t LikeEngineScriptModuleApiVersion() {
    return kScriptModuleApiVersion;
}

extern "C" __declspec(dllexport) const ScriptTypeRegistration*
LikeEngineGetScriptTypes(size_t* count) {
    static const std::array registrations = {
        GetFirstPersonControllerScriptRegistration(), GetRotatorScriptRegistration()};
    if (count != nullptr) {
        *count = registrations.size();
    }
    return registrations.data();
}
