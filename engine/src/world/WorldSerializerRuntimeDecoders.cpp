#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeAudioPlaybackFields(const Json& encoded, AudioSourceComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("clip") ||
        !encoded["clip"].is_string() || !encoded.contains("playOnAwake") ||
        !encoded["playOnAwake"].is_boolean() || !encoded.contains("loop") ||
        !encoded["loop"].is_boolean() || !encoded.contains("volume") ||
        !encoded["volume"].is_number() ||
        (encoded.contains("pitch") && !encoded["pitch"].is_number())) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.clipPath = encoded["clip"].get<std::string>();
    component.playOnAwake = encoded["playOnAwake"].get<bool>();
    component.loop = encoded["loop"].get<bool>();
    component.volume = encoded["volume"].get<float>();
    component.pitch = encoded.value("pitch", 1.0f);
    return true;
}

bool DecodeAudioSpatialFields(const Json& encoded, AudioSourceComponent& component) {
    if (!encoded.contains("spatial") || !encoded["spatial"].is_boolean() ||
        !encoded.contains("minDistance") || !encoded["minDistance"].is_number() ||
        !encoded.contains("maxDistance") || !encoded["maxDistance"].is_number()) {
        return false;
    }
    component.spatial = encoded["spatial"].get<bool>();
    component.minDistance = encoded["minDistance"].get<float>();
    component.maxDistance = encoded["maxDistance"].get<float>();
    return true;
}

bool IsValidAudioPlayback(const AudioSourceComponent& component) {
    return component.clipPath.size() <= 1024u &&
           component.clipPath.find('\0') == std::string::npos &&
           std::isfinite(component.volume) && component.volume >= 0.0f &&
           component.volume <= 1.0f && std::isfinite(component.pitch) &&
           component.pitch >= AudioSourceComponent::kMinPitch &&
           component.pitch <= AudioSourceComponent::kMaxPitch;
}

bool IsValidAudioSpatialSettings(const AudioSourceComponent& component) {
    return std::isfinite(component.minDistance) &&
           std::isfinite(component.maxDistance) && component.minDistance >= 0.0f &&
           component.maxDistance > component.minDistance;
}

bool DecodeAudioSourceComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("AudioSource")) {
        return true;
    }
    const Json& source = encoded["components"]["AudioSource"];
    AudioSourceComponent component{};
    if (!DecodeAudioPlaybackFields(source, component) ||
        !DecodeAudioSpatialFields(source, component)) {
        SetError(error, "Scene AudioSource component is invalid.");
        return false;
    }
    if (!IsValidAudioPlayback(component) || !IsValidAudioSpatialSettings(component)) {
        SetError(error, "Scene AudioSource settings are invalid.");
        return false;
    }
    entity.audioSource = std::move(component);
    return true;
}

bool DecodeAudioListenerComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("AudioListener")) {
            const Json& listener = encoded["components"]["AudioListener"];
            if (!listener.is_object() || !listener.contains("enabled") ||
                !listener["enabled"].is_boolean()) {
                SetError(error, "Scene AudioListener component is invalid.");
                return false;
            }
            entity.audioListener = AudioListenerComponent{listener["enabled"].get<bool>()};
        }
    return true;
}

bool DecodeAnimatorFields(const Json& encoded, AnimatorComponent& component) {
    if (!encoded.is_object() || !encoded.contains("enabled") ||
        !encoded["enabled"].is_boolean() || !encoded.contains("clip") ||
        !encoded["clip"].is_string() || !encoded.contains("playOnAwake") ||
        !encoded["playOnAwake"].is_boolean() || !encoded.contains("loop") ||
        !encoded["loop"].is_boolean() || !encoded.contains("speed") ||
        !encoded["speed"].is_number()) {
        return false;
    }
    component.enabled = encoded["enabled"].get<bool>();
    component.clip = encoded["clip"].get<std::string>();
    component.playOnAwake = encoded["playOnAwake"].get<bool>();
    component.loop = encoded["loop"].get<bool>();
    component.speed = encoded["speed"].get<float>();
    return true;
}

bool DecodeAnimatorOptions(const Json& encoded, AnimatorComponent& component) {
    if (!encoded.contains("lockRootPosition")) {
        return true;
    }
    if (!encoded["lockRootPosition"].is_boolean()) {
        return false;
    }
    component.lockRootPosition = encoded["lockRootPosition"].get<bool>();
    return true;
}

bool IsValidAnimatorSettings(const AnimatorComponent& component) {
    return component.clip.size() <= 256u &&
           component.clip.find('\0') == std::string::npos &&
           std::isfinite(component.speed) && component.speed >= 0.0f &&
           component.speed <= 100.0f;
}

bool DecodeAnimatorComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!encoded["components"].contains("Animator")) {
        return true;
    }
    const Json& animator = encoded["components"]["Animator"];
    AnimatorComponent component{};
    if (!DecodeAnimatorFields(animator, component)) {
        SetError(error, "Scene Animator component is invalid.");
        return false;
    }
    if (!DecodeAnimatorOptions(animator, component) ||
        !IsValidAnimatorSettings(component)) {
        SetError(error, "Scene Animator settings are invalid.");
        return false;
    }
    entity.animator = std::move(component);
    return true;
}

} // namespace

bool DecodeRuntimeComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    return DecodeAudioSourceComponent(encoded, entity, error) &&
           DecodeAudioListenerComponent(encoded, entity, error) &&
           DecodeAnimatorComponent(encoded, entity, error);
}

} // namespace WorldSerializerDecoding
