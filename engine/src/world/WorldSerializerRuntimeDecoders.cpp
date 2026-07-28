#include "internal/WorldSerializerComponentDecoders.h"

using namespace WorldSerializerJson;

namespace WorldSerializerDecoding {
namespace {
bool DecodeAudioSourceComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("AudioSource")) {
            const Json& source = encoded["components"]["AudioSource"];
            if (!source.is_object() || !source.contains("enabled") ||
                !source["enabled"].is_boolean() || !source.contains("clip") ||
                !source["clip"].is_string() || !source.contains("playOnAwake") ||
                !source["playOnAwake"].is_boolean() || !source.contains("loop") ||
                !source["loop"].is_boolean() || !source.contains("volume") ||
                !source["volume"].is_number() ||
                (source.contains("pitch") && !source["pitch"].is_number()) ||
                !source.contains("spatial") ||
                !source["spatial"].is_boolean() || !source.contains("minDistance") ||
                !source["minDistance"].is_number() || !source.contains("maxDistance") ||
                !source["maxDistance"].is_number()) {
                SetError(error, "Scene AudioSource component is invalid.");
                return false;
            }
            AudioSourceComponent component{};
            component.enabled = source["enabled"].get<bool>();
            component.clipPath = source["clip"].get<std::string>();
            component.playOnAwake = source["playOnAwake"].get<bool>();
            component.loop = source["loop"].get<bool>();
            component.volume = source["volume"].get<float>();
            component.pitch = source.value("pitch", 1.0f);
            component.spatial = source["spatial"].get<bool>();
            component.minDistance = source["minDistance"].get<float>();
            component.maxDistance = source["maxDistance"].get<float>();
            if (component.clipPath.size() > 1024u ||
                component.clipPath.find('\0') != std::string::npos ||
                !std::isfinite(component.volume) || component.volume < 0.0f ||
                component.volume > 1.0f || !std::isfinite(component.pitch) ||
                component.pitch < AudioSourceComponent::kMinPitch ||
                component.pitch > AudioSourceComponent::kMaxPitch ||
                !std::isfinite(component.minDistance) ||
                !std::isfinite(component.maxDistance) || component.minDistance < 0.0f ||
                component.maxDistance <= component.minDistance) {
                SetError(error, "Scene AudioSource settings are invalid.");
                return false;
            }
            entity.audioSource = std::move(component);
        }
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

bool DecodeAnimatorComponent(const Json& encoded, WorldEntity& entity, std::string* error) {
        if (encoded["components"].contains("Animator")) {
            const Json& animator = encoded["components"]["Animator"];
            if (!animator.is_object() || !animator.contains("enabled") ||
                !animator["enabled"].is_boolean() || !animator.contains("clip") ||
                !animator["clip"].is_string() || !animator.contains("playOnAwake") ||
                !animator["playOnAwake"].is_boolean() || !animator.contains("loop") ||
                !animator["loop"].is_boolean() || !animator.contains("speed") ||
                !animator["speed"].is_number()) {
                SetError(error, "Scene Animator component is invalid.");
                return false;
            }
            AnimatorComponent component{};
            component.enabled = animator["enabled"].get<bool>();
            component.clip = animator["clip"].get<std::string>();
            component.playOnAwake = animator["playOnAwake"].get<bool>();
            component.loop = animator["loop"].get<bool>();
            component.speed = animator["speed"].get<float>();
            if (animator.contains("lockRootPosition")) {
                if (!animator["lockRootPosition"].is_boolean()) {
                    SetError(error, "Scene Animator settings are invalid.");
                    return false;
                }
                component.lockRootPosition =
                    animator["lockRootPosition"].get<bool>();
            }
            if (component.clip.size() > 256u || component.clip.find('\0') != std::string::npos ||
                !std::isfinite(component.speed) || component.speed < 0.0f ||
                component.speed > 100.0f) {
                SetError(error, "Scene Animator settings are invalid.");
                return false;
            }
            entity.animator = std::move(component);
        }
    return true;
}

} // namespace

bool DecodeRuntimeComponents(const Json& encoded, WorldEntity& entity, std::string* error) {
    if (!DecodeAudioSourceComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeAudioListenerComponent(encoded, entity, error)) {
        return false;
    }
    if (!DecodeAnimatorComponent(encoded, entity, error)) {
        return false;
    }
    return true;
}

} // namespace WorldSerializerDecoding
