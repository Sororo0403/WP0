#include "scene/GameScene.h"

#include <dinput.h>

#ifdef DrawText
#undef DrawText
#endif

void GameScene::Initialize(const SceneContext& ctx) {
    BaseScene::Initialize(ctx);
}

void GameScene::Update() {}

void GameScene::Draw() {}

void GameScene::DrawPostProcessOverlay() {}
