#include "EditorScene.h"

#include "AssetImportPlanner.h"
#include "sound/ISoundService.h"

#include <algorithm>
#include <ranges>

bool EditorScene::BeginRuntimeAudio(std::string* error) {
    EndRuntimeAudio();
    if (std::ranges::none_of(world_.Entities(), [](const WorldEntity& entity) {
            return entity.audioSource.has_value();
        })) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        if (error != nullptr) {
            *error = "Audio service is unavailable.";
        }
        return false;
    }
    bool valid = true;
    const size_t activeListenerCount = static_cast<size_t>(std::ranges::count_if(
        world_.Entities(), [this](const WorldEntity& entity) {
            return world_.IsActiveInHierarchy(entity.id) && entity.audioListener &&
                   entity.audioListener->enabled;
        }));
    if (activeListenerCount > 1u) {
        AddConsoleEntry("Multiple enabled Audio Listeners found. The first one will be used.",
                        ConsoleSeverity::Warning);
    }
    for (const WorldEntity& entity : world_.Entities()) {
        if (!entity.audioSource) {
            continue;
        }
        RuntimeAudioSource runtime{};
        runtime.entity = entity.id;
        if (!entity.audioSource->clipPath.empty()) {
            const std::optional<std::filesystem::path> clip =
                ResolveProjectAssetPath(entity.audioSource->clipPath);
            if (!clip || !AssetImport::IsAudioFile(*clip) ||
                !sound->TryLoad(clip->wstring(), runtime.soundId)) {
                valid = false;
                if (error != nullptr && error->empty()) {
                    *error = entity.name + ": AudioSource clip could not be loaded.";
                }
            }
        }
        runtimeAudioSources_.push_back(runtime);
    }
    UpdateRuntimeAudio();
    if (valid && error != nullptr) {
        error->clear();
    }
    return valid;
}

const WorldEntity* EditorScene::FindRuntimeAudioListener() const {
    const auto enabledListener =
        std::ranges::find_if(world_.Entities(), [this](const WorldEntity& entity) {
            return world_.IsActiveInHierarchy(entity.id) && entity.audioListener &&
                   entity.audioListener->enabled;
        });
    if (enabledListener != world_.Entities().end()) {
        return &*enabledListener;
    }
    const auto primaryCamera =
        std::ranges::find_if(world_.Entities(), [this](const WorldEntity& entity) {
            return world_.IsActiveInHierarchy(entity.id) && entity.camera &&
                   entity.camera->enabled && entity.camera->primary;
        });
    return primaryCamera != world_.Entities().end() ? &*primaryCamera : nullptr;
}

void EditorScene::UpdateRuntimeAudioListener(ISoundService& sound) const {
    const WorldEntity* listener = FindRuntimeAudioListener();
    DirectX::XMFLOAT4X4 matrix{};
    if (listener == nullptr || !world_.TryGetWorldMatrix(listener->id, matrix)) {
        return;
    }
    using namespace DirectX;
    const XMMATRIX world = XMLoadFloat4x4(&matrix);
    XMVECTOR forward = XMVector3TransformNormal(g_XMIdentityR2, world);
    XMVECTOR up = XMVector3TransformNormal(g_XMIdentityR1, world);
    forward = XMVectorGetX(XMVector3LengthSq(forward)) > 1.0e-8f
                  ? XMVector3Normalize(forward)
                  : g_XMIdentityR2;
    up = XMVectorGetX(XMVector3LengthSq(up)) > 1.0e-8f ? XMVector3Normalize(up)
                                                       : g_XMIdentityR1;
    XMFLOAT3 storedForward{};
    XMFLOAT3 storedUp{};
    XMStoreFloat3(&storedForward, forward);
    XMStoreFloat3(&storedUp, up);
    sound.SetListener({matrix._41, matrix._42, matrix._43}, storedForward, storedUp);
}

void EditorScene::StopRuntimeAudioVoices(RuntimeAudioSource& runtime,
                                         ISoundService& sound) {
    if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
        sound.Stop(runtime.voice);
    }
    runtime.voice = ISoundService::kInvalidVoiceHandle;
    for (const uint32_t voice : runtime.oneShotVoices) {
        sound.Stop(voice);
    }
    runtime.oneShotVoices.clear();
}

uint32_t EditorScene::PlayRuntimeAudioVoice(
    const RuntimeAudioSource& runtime, const AudioSourceComponent& source,
    ISoundService& sound, bool loop) const {
    if (!source.spatial) {
        return sound.Play(runtime.soundId, source.volume, loop);
    }
    DirectX::XMFLOAT4X4 matrix{};
    if (!world_.TryGetWorldMatrix(runtime.entity, matrix)) {
        return ISoundService::kInvalidVoiceHandle;
    }
    return sound.Play3D(runtime.soundId, {matrix._41, matrix._42, matrix._43},
                        source.volume, loop);
}

void EditorScene::StartRuntimeAudioVoice(RuntimeAudioSource& runtime,
                                         const AudioSourceComponent& source,
                                         ISoundService& sound) {
    if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
        sound.Stop(runtime.voice);
    }
    runtime.voice = PlayRuntimeAudioVoice(runtime, source, sound, source.loop);
}

void EditorScene::ProcessRuntimeAudioPlayback(
    RuntimeAudioSource& runtime, const AudioSourceComponent& source,
    AudioSourceComponent::RuntimeCommand command, uint32_t pendingOneShots,
    ISoundService& sound) {
    if (command == AudioSourceComponent::RuntimeCommand::Stop) {
        StopRuntimeAudioVoices(runtime, sound);
        runtime.activated = true;
    }
    std::erase_if(runtime.oneShotVoices,
                  [&sound](uint32_t voice) { return !sound.IsPlaying(voice); });
    if (command == AudioSourceComponent::RuntimeCommand::Play) {
        runtime.activated = true;
        StartRuntimeAudioVoice(runtime, source, sound);
    } else if (!runtime.activated) {
        runtime.activated = true;
        if (source.playOnAwake) {
            StartRuntimeAudioVoice(runtime, source, sound);
        }
    }
    for (uint32_t index = 0;
         index < pendingOneShots &&
         runtime.oneShotVoices.size() < AudioSourceComponent::kMaxOneShotVoices;
         ++index) {
        const uint32_t voice = PlayRuntimeAudioVoice(runtime, source, sound, false);
        if (voice != ISoundService::kInvalidVoiceHandle) {
            runtime.oneShotVoices.push_back(voice);
        }
    }
    if (runtime.voice != ISoundService::kInvalidVoiceHandle &&
        !sound.IsPlaying(runtime.voice)) {
        runtime.voice = ISoundService::kInvalidVoiceHandle;
    }
}

void EditorScene::UpdateRuntimeAudioVoiceSettings(
    const RuntimeAudioSource& runtime, const AudioSourceComponent& source,
    ISoundService& sound) const {
    std::optional<DirectX::XMFLOAT3> sourcePosition;
    if (source.spatial) {
        DirectX::XMFLOAT4X4 matrix{};
        if (world_.TryGetWorldMatrix(runtime.entity, matrix)) {
            sourcePosition = {matrix._41, matrix._42, matrix._43};
        }
    }
    const auto updateVoice = [&](uint32_t voice) {
        sound.SetVoiceVolume(voice, source.volume);
        sound.SetVoiceFrequencyRatio(voice, source.pitch);
        if (sourcePosition) {
            sound.SetVoicePosition(voice, *sourcePosition);
            sound.SetVoice3DRange(voice, source.minDistance, source.maxDistance);
        }
    };
    if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
        updateVoice(runtime.voice);
    }
    for (const uint32_t voice : runtime.oneShotVoices) {
        updateVoice(voice);
    }
}

void EditorScene::UpdateRuntimeAudioSource(RuntimeAudioSource& runtime,
                                            ISoundService& sound) {
    WorldEntity* entity = world_.Find(runtime.entity);
    AudioSourceComponent* source =
        entity != nullptr && entity->audioSource ? &*entity->audioSource : nullptr;
    const AudioSourceComponent::RuntimeCommand command =
        source != nullptr ? source->runtimeCommand
                          : AudioSourceComponent::RuntimeCommand::None;
    const uint32_t pendingOneShots = source != nullptr ? source->pendingOneShots : 0u;
    if (source != nullptr) {
        source->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
        source->pendingOneShots = 0u;
    }
    const bool active = source != nullptr && source->enabled &&
                        world_.IsActiveInHierarchy(runtime.entity) &&
                        runtime.soundId != ISoundService::kInvalidSoundId;
    if (!active) {
        StopRuntimeAudioVoices(runtime, sound);
        runtime.activated = false;
        if (source != nullptr) {
            source->runtimePlaying = false;
        }
        return;
    }
    ProcessRuntimeAudioPlayback(runtime, *source, command, pendingOneShots, sound);
    UpdateRuntimeAudioVoiceSettings(runtime, *source, sound);
    source->runtimePlaying = runtime.voice != ISoundService::kInvalidVoiceHandle ||
                             !runtime.oneShotVoices.empty();
}

void EditorScene::UpdateRuntimeAudio() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        return;
    }
    UpdateRuntimeAudioListener(*sound);
    for (RuntimeAudioSource& runtime : runtimeAudioSources_) {
        UpdateRuntimeAudioSource(runtime, *sound);
    }
}

void EditorScene::PauseRuntimeAudio(bool paused) {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound == nullptr) {
        return;
    }
    for (const RuntimeAudioSource& runtime : runtimeAudioSources_) {
        const auto setPaused = [&](uint32_t voice) {
            if (paused) {
                sound->Pause(voice);
            } else {
                sound->Resume(voice);
            }
        };
        if (runtime.voice != ISoundService::kInvalidVoiceHandle) {
            setPaused(runtime.voice);
        }
        for (const uint32_t voice : runtime.oneShotVoices) {
            setPaused(voice);
        }
    }
}

void EditorScene::EndRuntimeAudio() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound != nullptr) {
        for (RuntimeAudioSource& runtime : runtimeAudioSources_) {
            StopRuntimeAudioVoices(runtime, *sound);
        }
    }
    for (RuntimeAudioSource& runtime : runtimeAudioSources_) {
        if (WorldEntity* entity = world_.Find(runtime.entity);
            entity != nullptr && entity->audioSource) {
            entity->audioSource->runtimeCommand = AudioSourceComponent::RuntimeCommand::None;
            entity->audioSource->pendingOneShots = 0u;
            entity->audioSource->runtimePlaying = false;
        }
    }
    runtimeAudioSources_.clear();
}
