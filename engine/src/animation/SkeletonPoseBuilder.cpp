#include "animation/SkeletonPoseBuilder.h"

#include "animation/AnimationSampler.h"
#include "core/MathUtils.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <new>

using namespace DirectX;

namespace {

bool ResolveReadyParentIndex(int parentIndex, size_t childIndex, size_t boneCount,
                             size_t& resolvedIndex) {
    if (parentIndex < 0) {
        return false;
    }

    const size_t parent = static_cast<size_t>(parentIndex);
    if (parent >= boneCount || parent >= childIndex) {
        return false;
    }

    resolvedIndex = parent;
    return true;
}

XMMATRIX LockTranslationToBindPose(FXMMATRIX animated, CXMMATRIX bindPose) {
    XMVECTOR animatedScale{};
    XMVECTOR animatedRotation{};
    XMVECTOR animatedTranslation{};
    XMVECTOR bindScale{};
    XMVECTOR bindRotation{};
    XMVECTOR bindTranslation{};
    if (!XMMatrixDecompose(&animatedScale, &animatedRotation, &animatedTranslation, animated) ||
        !XMMatrixDecompose(&bindScale, &bindRotation, &bindTranslation, bindPose)) {
        return animated;
    }
    return XMMatrixScalingFromVector(animatedScale) *
           XMMatrixRotationQuaternion(XMQuaternionNormalize(animatedRotation)) *
           XMMatrixTranslationFromVector(bindTranslation);
}

XMMATRIX MakeAnimatedLocalMatrix(const Model& model, const BoneInfo& bone,
                                 const AnimationClip& clip, float time) {
    auto it = clip.nodeAnimations.find(bone.name);
    if (it == clip.nodeAnimations.end()) {
        return XMLoadFloat4x4(&bone.localBindMatrix);
    }

    const NodeAnimation& anim = it->second;

    XMFLOAT3 pos = anim.translate.keyframes.empty()
                       ? XMFLOAT3{0.0f, 0.0f, 0.0f}
                       : AnimationSampler::SampleVec3(anim.translate, time);

    XMFLOAT3 scl = anim.scale.keyframes.empty() ? XMFLOAT3{1.0f, 1.0f, 1.0f}
                                                : AnimationSampler::SampleVec3(anim.scale, time);

    XMFLOAT4 rot = anim.rotate.keyframes.empty() ? XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f}
                                                 : AnimationSampler::SampleQuat(anim.rotate, time);

    XMVECTOR q = MathUtils::LoadNormalizedQuaternionOrIdentity(rot);
    XMMATRIX animatedLocal = XMMatrixScaling(scl.x, scl.y, scl.z) * XMMatrixRotationQuaternion(q) *
                             XMMatrixTranslation(pos.x, pos.y, pos.z);

    XMMATRIX adjustment = XMLoadFloat4x4(&bone.parentAdjustmentMatrix);
    XMMATRIX result = animatedLocal * adjustment;
    const bool isAnimatedSkeletonRoot =
        bone.name == clip.rootNodeName ||
        (bone.parentIndex < 0 && clip.nodeAnimations.contains(bone.name));
    if (model.lockRootAnimationPosition && isAnimatedSkeletonRoot) {
        result = LockTranslationToBindPose(result, XMLoadFloat4x4(&bone.localBindMatrix));
    }
    return result;
}

XMMATRIX BlendLocalMatrices(FXMMATRIX source, CXMMATRIX target, float blend) {
    XMVECTOR sourceScale{};
    XMVECTOR sourceRotation{};
    XMVECTOR sourceTranslation{};
    XMVECTOR targetScale{};
    XMVECTOR targetRotation{};
    XMVECTOR targetTranslation{};
    const float t = std::clamp(std::isfinite(blend) ? blend : 1.0f, 0.0f, 1.0f);
    if (!XMMatrixDecompose(&sourceScale, &sourceRotation, &sourceTranslation, source) ||
        !XMMatrixDecompose(&targetScale, &targetRotation, &targetTranslation, target)) {
        return t < 0.5f ? source : target;
    }
    sourceRotation = XMQuaternionNormalize(sourceRotation);
    targetRotation = XMQuaternionNormalize(targetRotation);
    return XMMatrixScalingFromVector(XMVectorLerp(sourceScale, targetScale, t)) *
           XMMatrixRotationQuaternion(XMQuaternionSlerp(sourceRotation, targetRotation, t)) *
           XMMatrixTranslationFromVector(XMVectorLerp(sourceTranslation, targetTranslation, t));
}

} // namespace

void SkeletonPoseBuilder::BuildBindPoseLocals(const Model& model,
                                              std::vector<XMMATRIX>& localMatrices) {
    const size_t boneCount = model.bones.size();
    try {
        localMatrices.resize(boneCount);
    } catch (const std::exception&) {
        localMatrices.clear();
        return;
    }
    for (size_t i = 0; i < boneCount; i++) {
        localMatrices[i] = XMLoadFloat4x4(&model.bones[i].localBindMatrix);
    }
}

void SkeletonPoseBuilder::BuildAnimatedLocals(const Model& model, const AnimationClip& clip,
                                              float time, std::vector<XMMATRIX>& localMatrices) {
    const size_t boneCount = model.bones.size();
    try {
        localMatrices.resize(boneCount);
    } catch (const std::exception&) {
        localMatrices.clear();
        return;
    }
    for (size_t i = 0; i < boneCount; i++) {
        localMatrices[i] = MakeAnimatedLocalMatrix(model, model.bones[i], clip, time);
    }
}

void SkeletonPoseBuilder::BuildBlendedLocals(
    const Model& model, const AnimationClip& source, float sourceTime,
    const AnimationClip& target, float targetTime, float blend,
    std::vector<XMMATRIX>& localMatrices) {
    const size_t boneCount = model.bones.size();
    try {
        localMatrices.resize(boneCount);
    } catch (const std::exception&) {
        localMatrices.clear();
        return;
    }
    for (size_t i = 0; i < boneCount; ++i) {
        const XMMATRIX sourceLocal =
            MakeAnimatedLocalMatrix(model, model.bones[i], source, sourceTime);
        const XMMATRIX targetLocal =
            MakeAnimatedLocalMatrix(model, model.bones[i], target, targetTime);
        localMatrices[i] = BlendLocalMatrices(sourceLocal, targetLocal, blend);
    }
}

void SkeletonPoseBuilder::UpdateSkeleton(Model& model, const std::vector<XMMATRIX>& localMatrices) {
    const size_t boneCount = model.bones.size();
    std::vector<XMFLOAT4X4> skeletonSpaceMatrices;
    std::vector<XMFLOAT4X4> finalBoneMatrices;
    std::vector<XMMATRIX> globalMatrices;
    try {
        skeletonSpaceMatrices.resize(boneCount);
        finalBoneMatrices.resize(boneCount);
        globalMatrices.resize(boneCount);
    } catch (const std::exception&) {
        return;
    }

    for (size_t i = 0; i < boneCount; i++) {
        const XMMATRIX local = i < localMatrices.size()
                                   ? localMatrices[i]
                                   : XMLoadFloat4x4(&model.bones[i].localBindMatrix);

        size_t parent = 0;
        if (ResolveReadyParentIndex(model.bones[i].parentIndex, i, boneCount, parent)) {
            globalMatrices[i] = local * globalMatrices[parent];
        } else {
            globalMatrices[i] = local;
        }

        XMStoreFloat4x4(&skeletonSpaceMatrices[i], globalMatrices[i]);
    }

    for (size_t i = 0; i < boneCount; i++) {
        XMMATRIX offset = XMLoadFloat4x4(&model.bones[i].offsetMatrix);
        XMMATRIX final = offset * globalMatrices[i];
        XMStoreFloat4x4(&finalBoneMatrices[i], final);
    }
    model.skeletonSpaceMatrices = std::move(skeletonSpaceMatrices);
    model.finalBoneMatrices = std::move(finalBoneMatrices);
}
