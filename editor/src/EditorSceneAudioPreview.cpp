#include "EditorScene.h"

#include "imgui.h"
#include "sound/ISoundService.h"

void EditorScene::DrawAudioAssetPreview(const std::filesystem::path& physicalPath) {
    SynchronizeAudioPreviewSelection();
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    const bool playing = IsAudioPreviewPlaying(sound);
    DrawAudioPreviewControls(sound, physicalPath, playing);
    DrawAudioPreviewInfo(sound);
}

void EditorScene::SynchronizeAudioPreviewSelection() {
    const std::filesystem::path selected = selectedAsset_.lexically_normal();
    if (assetPreviewAsset_ == selected) {
        return;
    }
    StopAudioAssetPreview();
    audioPreviewSoundId_ = ISoundService::kInvalidSoundId;
    assetPreviewAsset_ = selected;
    assetPreviewModel_ = {};
    assetPreviewPlan_.clear();
    assetPreviewError_.clear();
}

bool EditorScene::IsAudioPreviewPlaying(const ISoundService* sound) const {
    return sound != nullptr && audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle &&
           sound->IsPlaying(audioPreviewVoice_);
}

void EditorScene::DrawAudioPreviewControls(ISoundService* sound,
                                           const std::filesystem::path& physicalPath,
    bool playing) {
    ImGui::BeginDisabled(sound == nullptr);
    const bool playRequested =
        ImGui::SmallButton(playing ? "Restart Preview" : "Play Preview");
    if (playRequested && sound != nullptr) {
        StartAudioAssetPreview(*sound, physicalPath);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::SmallButton("Stop Preview")) {
        StopAudioAssetPreview();
        status_ = "Stopped audio preview.";
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void EditorScene::StartAudioAssetPreview(ISoundService& sound,
                                         const std::filesystem::path& physicalPath) {
    StopAudioAssetPreview();
    uint32_t soundId = ISoundService::kInvalidSoundId;
    if (!sound.TryLoad(physicalPath.wstring(), soundId)) {
        status_ = "Audio preview failed: the file could not be decoded.";
        return;
    }
    audioPreviewSoundId_ = soundId;
    audioPreviewVoice_ = sound.Play(soundId);
    status_ = audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle
                  ? "Playing audio preview: " + physicalPath.filename().string()
                  : "Audio preview failed: the audio device is unavailable.";
}

void EditorScene::DrawAudioPreviewInfo(const ISoundService* sound) const {
    if (sound == nullptr || audioPreviewSoundId_ == ISoundService::kInvalidSoundId) {
        return;
    }
    const ISoundService::SoundInfo* info = sound->GetInfo(audioPreviewSoundId_);
    if (info != nullptr) {
        ImGui::TextDisabled("Duration: %.2f s   Channels: %u   Sample Rate: %u Hz",
                            info->durationSeconds, static_cast<unsigned>(info->channels),
                            info->sampleRate);
    }
}

void EditorScene::StopAudioAssetPreview() {
    ISoundService* sound = ctx_ != nullptr ? ctx_->systems.sound : nullptr;
    if (sound != nullptr && audioPreviewVoice_ != ISoundService::kInvalidVoiceHandle) {
        sound->Stop(audioPreviewVoice_);
    }
    audioPreviewVoice_ = ISoundService::kInvalidVoiceHandle;
}
