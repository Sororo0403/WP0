#pragma once
#include "model/Model.h"

/// <summary>
/// モデルアニメーションの再生制御を担当する
/// </summary>
class Animator {
public:
    /// <summary>
    /// 指定したアニメーションをモデルへ設定して再生を開始する
    /// </summary>
    static void Play(Model& model, const std::string& animationName, bool loop = true);

    /// <summary>
    /// 現在姿勢から指定Animationへ滑らかに遷移する
    /// </summary>
    static void CrossFade(Model& model, const std::string& animationName,
                          float duration, bool loop = true);

    /// <summary>
    /// 再生中アニメーションの時間を進めてモデル姿勢を更新する
    /// </summary>
    static void Update(Model& model, float deltaTime);

    /// <summary>
    /// モデルの現在のアニメーションが終端まで再生されたかを返す
    /// </summary>
    static bool IsFinished(const Model& model);

private:
    /// <summary>
    /// モデルをバインドポーズへ戻す
    /// </summary>
};
