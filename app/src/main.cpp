#include "scene/GameScene.h"

#include <Windows.h>
#include <memory>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine,
                   int showCommand) {
    (void)previousInstance;
    (void)commandLine;

    EngineRuntimeConfig config{};
    config.width = 1280;
    config.height = 720;
    config.title = L"WP0";
    config.cursorVisible = true;
    config.logPath = L"generated/logs/app.log";

    EngineRuntime runtime;
    return runtime.Run(instance, showCommand, std::make_unique<GameScene>(), config);
}
