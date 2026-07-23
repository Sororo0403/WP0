#include "animation/Animator.h"

#include "animation/AnimationSampler.h"
#include "animation/SkeletonPoseBuilder.h"
#include "core/MathUtils.h"

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <new>

using namespace DirectX;

namespace {

void ResetRootAnimation(Model& model) {
    model.hasRootAnimation = false;
    XMStoreFloat4x4(&model.rootAnimationMatrix, XMMatrixIdentity());
}

void ResetBlend(Model& model) {
    model.blendSourceAnimation.clear();
    model.blendSourceTime = 0.0f;
    model.blendSourceLoop = true;
    model.blendDuration = 0.0f;
    model.blendTime = 0.0f;
}

struct AnimationPlaybackPolicy {
    bool loop = false;
    void (*finish)(Model&, const AnimationClip&) = nullptr;
};

void FinishLoopingPlayback(Model& model, const AnimationClip& clip) {
    if (model.animationTime >= clip.duration) {
        model.animationTime = std::fmod(model.animationTime, clip.duration);
    }
}

void FinishOneShotPlayback(Model& model, const AnimationClip& clip) {
    if (model.animationTime >= clip.duration) {
        model.animationTime = clip.duration;
        model.isPlaying = false;
        model.animationFinished = true;
    }
}

const std::array<AnimationPlaybackPolicy, 2>& AnimationPlaybackPolicies() {
    static const std::array<AnimationPlaybackPolicy, 2> kPolicies = {{
        {.loop = false, .finish = FinishOneShotPlayback},
        {.loop = true, .finish = FinishLoopingPlayback},
    }};
    return kPolicies;
}

const AnimationPlaybackPolicy& PlaybackPolicyFor(bool loop) {
    const auto& policies = AnimationPlaybackPolicies();
    const auto found = std::ranges::find_if(
        policies, [loop](const AnimationPlaybackPolicy& policy) { return policy.loop == loop; });
    return found != policies.end() ? *found : policies.front();
}

void AdvancePlayback(Model& model, const AnimationClip& clip, float deltaTime) {
    if (!model.isPlaying) {
        return;
    }

    if (!std::isfinite(model.animationTime) || model.animationTime < 0.0f) {
        model.animationTime = 0.0f;
    }
    const float safeDeltaTime = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
    model.animationTime += safeDeltaTime;
    if (!std::isfinite(model.animationTime)) {
        model.animationTime = model.isLoop ? 0.0f : clip.duration;
    }

    PlaybackPolicyFor(model.isLoop).finish(model, clip);
}

void AdvanceBlendSource(Model& model, const AnimationClip& clip, float deltaTime) {
    const float safeDeltaTime = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
    model.blendSourceTime += safeDeltaTime;
    if (!std::isfinite(model.blendSourceTime) || model.blendSourceTime < 0.0f) {
        model.blendSourceTime = 0.0f;
    }
    if (model.blendSourceTime >= clip.duration) {
        model.blendSourceTime = model.blendSourceLoop
                                    ? std::fmod(model.blendSourceTime, clip.duration)
                                    : clip.duration;
    }
}

bool TrySampleRootMatrix(const AnimationClip& clip, float time, XMMATRIX& matrix) {
    const auto root = clip.nodeAnimations.find(clip.rootNodeName);
    if (clip.rootNodeName.empty() || root == clip.nodeAnimations.end()) {
        return false;
    }
    const NodeAnimation& animation = root->second;
    const XMFLOAT3 position = animation.translate.keyframes.empty()
                                  ? XMFLOAT3{0.0f, 0.0f, 0.0f}
                                  : AnimationSampler::SampleVec3(animation.translate, time);
    const XMFLOAT3 scale = animation.scale.keyframes.empty()
                               ? XMFLOAT3{1.0f, 1.0f, 1.0f}
                               : AnimationSampler::SampleVec3(animation.scale, time);
    const XMFLOAT4 rotation = animation.rotate.keyframes.empty()
                                  ? XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f}
                                  : AnimationSampler::SampleQuat(animation.rotate, time);
    matrix = XMMatrixScaling(scale.x, scale.y, scale.z) *
             XMMatrixRotationQuaternion(
                 MathUtils::LoadNormalizedQuaternionOrIdentity(rotation)) *
             XMMatrixTranslation(position.x, position.y, position.z);
    return true;
}

XMMATRIX BlendRootMatrices(FXMMATRIX source, CXMMATRIX target, float blend) {
    XMVECTOR sourceScale{};
    XMVECTOR sourceRotation{};
    XMVECTOR sourceTranslation{};
    XMVECTOR targetScale{};
    XMVECTOR targetRotation{};
    XMVECTOR targetTranslation{};
    if (!XMMatrixDecompose(&sourceScale, &sourceRotation, &sourceTranslation, source) ||
        !XMMatrixDecompose(&targetScale, &targetRotation, &targetTranslation, target)) {
        return target;
    }
    const float t = std::clamp(blend, 0.0f, 1.0f);
    return XMMatrixScalingFromVector(XMVectorLerp(sourceScale, targetScale, t)) *
           XMMatrixRotationQuaternion(XMQuaternionSlerp(
               XMQuaternionNormalize(sourceRotation), XMQuaternionNormalize(targetRotation), t)) *
           XMMatrixTranslationFromVector(XMVectorLerp(sourceTranslation, targetTranslation, t));
}

} // namespace

void Animator::Play(Model& model, const std::string& animationName, bool loop) {
    auto it = model.animations.find(animationName);
    if (it == model.animations.end()) {
        return;
    }

    model.currentAnimation = animationName;
    model.animationTime = 0.0f;
    model.isLoop = loop;
    model.isPlaying = true;
    model.animationFinished = false;
    ResetBlend(model);
}

void Animator::CrossFade(Model& model, const std::string& animationName, float duration,
                         bool loop) {
    const auto target = model.animations.find(animationName);
    if (target == model.animations.end()) {
        return;
    }
    const float safeDuration = std::isfinite(duration) ? (std::max)(duration, 0.0f) : 0.0f;
    const auto source = model.animations.find(model.currentAnimation);
    if (safeDuration <= 0.0f || source == model.animations.end() ||
        !std::isfinite(source->second.duration) || source->second.duration <= 0.0f ||
        model.currentAnimation == animationName) {
        if (model.currentAnimation != animationName || !model.isPlaying) {
            Play(model, animationName, loop);
        } else {
            model.isLoop = loop;
        }
        return;
    }
    model.blendSourceAnimation = model.currentAnimation;
    model.blendSourceTime = model.animationTime;
    model.blendSourceLoop = model.isLoop;
    model.blendDuration = safeDuration;
    model.blendTime = 0.0f;
    model.currentAnimation = animationName;
    model.animationTime = 0.0f;
    model.isLoop = loop;
    model.isPlaying = true;
    model.animationFinished = false;
}

bool Animator::IsFinished(const Model& model) {
    return model.animationFinished;
}

void Animator::ApplyBindPose(Model& model) {
    const size_t boneCount = model.bones.size();

    std::vector<XMMATRIX> localMatrices;
    SkeletonPoseBuilder::BuildBindPoseLocals(model, localMatrices);
    if (localMatrices.size() != boneCount) {
        return;
    }
    SkeletonPoseBuilder::UpdateSkeleton(model, localMatrices);
}

void Animator::Update(Model& model, float deltaTime) {
    const auto applyBindPoseIfPresent = [](Model& target) {
        ResetRootAnimation(target);
        if (!target.bones.empty()) {
            Animator::ApplyBindPose(target);
        }
    };

    if (model.currentAnimation.empty()) {
        applyBindPoseIfPresent(model);
        return;
    }

    auto clipIt = model.animations.find(model.currentAnimation);
    if (clipIt == model.animations.end()) {
        applyBindPoseIfPresent(model);
        return;
    }

    const AnimationClip& clip = clipIt->second;
    if (!std::isfinite(clip.duration) || clip.duration <= 0.0f) {
        applyBindPoseIfPresent(model);
        return;
    }

    AdvancePlayback(model, clip, deltaTime);

    const auto blendSource = model.animations.find(model.blendSourceAnimation);
    const bool blending = blendSource != model.animations.end() &&
                          model.blendDuration > 0.0f &&
                          model.blendTime < model.blendDuration;
    float blend = 1.0f;
    if (blending) {
        AdvanceBlendSource(model, blendSource->second, deltaTime);
        const float safeDeltaTime =
            std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
        model.blendTime = (std::min)(model.blendTime + safeDeltaTime, model.blendDuration);
        blend = std::clamp(model.blendTime / model.blendDuration, 0.0f, 1.0f);
    }

    if (model.bones.empty()) {
        ResetRootAnimation(model);
        XMMATRIX targetRoot{};
        if (TrySampleRootMatrix(clip, model.animationTime, targetRoot)) {
            XMMATRIX root = targetRoot;
            XMMATRIX sourceRoot{};
            if (blending && TrySampleRootMatrix(blendSource->second, model.blendSourceTime,
                                                sourceRoot)) {
                root = BlendRootMatrices(sourceRoot, targetRoot, blend);
            }
            XMStoreFloat4x4(&model.rootAnimationMatrix, root);
            model.hasRootAnimation = true;
        }

        if (blend >= 1.0f) {
            ResetBlend(model);
        }

        return;
    }

    const size_t boneCount = model.bones.size();

    std::vector<XMMATRIX> localMatrices;
    if (blending) {
        SkeletonPoseBuilder::BuildBlendedLocals(
            model, blendSource->second, model.blendSourceTime, clip, model.animationTime,
            blend, localMatrices);
    } else {
        SkeletonPoseBuilder::BuildAnimatedLocals(model, clip, model.animationTime, localMatrices);
    }
    if (localMatrices.size() != boneCount) {
        return;
    }
    SkeletonPoseBuilder::UpdateSkeleton(model, localMatrices);
    if (blend >= 1.0f) {
        ResetBlend(model);
    }
}
